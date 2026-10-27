/*
 * SPDX-FileCopyrightText: 2026 DatanoiseTV
 *
 * SPDX-License-Identifier: Apache-2.0
 */
/*
 * RTP audio streaming engine implementation.
 *
 * Manages source (TX) and sink (RX) streams with per-stream ring buffers.
 * The RX task receives packets and dispatches to sink jitter buffers.
 * The TX path is driven by the audio frame timer calling process_tx().
 *
 * Ring buffers use single-reader/single-writer lock-free design with
 * volatile read/write positions for safe concurrent access.
 */

#include "aes67_rtp.h"
#include "aes67_config.h"
#include "aes67_convert.h"
#include "aes67_net.h"
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include "esp_log.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "lwip/udp.h"
#include "lwip/pbuf.h"
#include "lwip/igmp.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"

static const char *TAG = "aes67_rtp";

#define AES67_RTP_VERSION       2
#define AES67_RTP_HEADER_SIZE   12
#define AES67_MAX_STREAMS       (CONFIG_AES67_MAX_SOURCES + CONFIG_AES67_MAX_SINKS)
#define AES67_RX_TASK_STACK     4096
#define AES67_RX_BUF_SIZE       2048
#define AES67_JITTER_BUF_MULT   6   /* 6 * packet_size for jitter absorption */

/* Ring buffer for audio data. Single reader, single writer. */
typedef struct {
    uint8_t *data;
    uint32_t size;              /* Total buffer size in bytes */
    volatile uint32_t write_pos;
    volatile uint32_t read_pos;
} aes67_ringbuf_t;

/* Internal stream structure */
struct aes67_rtp_stream {
    aes67_rtp_stream_config_t config;
    aes67_stream_params_t params;       /* Computed stream parameters */
    aes67_stream_dir_t direction;
    int tx_sock;
    struct sockaddr_in dest_addr;
    uint16_t seq_num;
    uint32_t ssrc;
    uint32_t rtp_timestamp;     /* Running RTP timestamp, increments by samples_per_packet */
    bool timestamp_initialized; /* Set once from PTP time on first TX */
    aes67_rtp_stream_status_t status;
    aes67_ringbuf_t ring;
    uint32_t samples_per_packet;
    uint32_t packet_payload_size;
    uint8_t *tx_packet_buf;     /* Scratch buffer for packet assembly (internal SRAM) */
    bool active;
    bool first_packet;          /* True until first RTP packet received (sinks) */
    bool rx_reported_active;    /* last state given to the rx-state callback */
    /* Codec conversion function pointers (MTConvert pattern).
     * Resolved once at stream creation, called per-packet with no branching. */
    aes67_convert_to_net_fn convert_to_net;
    aes67_convert_from_net_fn convert_from_net;
    /* Jitter measurement for RX */
    int64_t last_rx_time_us;    /* Timestamp of last received packet in microseconds */
    int32_t jitter_us;          /* Estimated inter-packet jitter in microseconds */
    /* Output routing: which stream channel feeds each of the two I2S slots */
    uint8_t out_map[2];
};

/* Receive lookup table consulted by the Ethernet frame path. Entries are
 * maintained under rx_lock by stream add/remove; the frame path holds the
 * same spinlock while it matches, decodes and updates statistics, so a
 * removal can never free a stream the frame path is using. */
typedef struct {
    uint32_t dest_ip;               /* network order */
    uint16_t dest_port;             /* host order */
    uint8_t  payload_type;
    struct aes67_rtp_stream *stream;
} rx_table_entry_t;

#define RX_SCRATCH_FRAMES 192       /* 4 ms at 48 kHz, the AES67 practical maximum */
#define RX_ACTIVE_WINDOW_US 500000  /* "receiving" if a packet arrived within this */

/* Internal engine structure */
struct aes67_rtp_engine {
    /* streams[] is read by the TIC task (process_tx, feed_capture) and the
     * RX task, and mutated by add/remove from the httpd or SAP tasks, on
     * both cores. Every access to the table or to a stream reached through
     * it holds this mutex. Hold times are one tick's worth of work. */
    SemaphoreHandle_t lock;
    SemaphoreHandle_t rx_done;     /* given by the RX task when it exits */
    struct aes67_rtp_stream *streams[AES67_MAX_STREAMS];
    int stream_count;
    int rx_sock;
    TaskHandle_t rx_task;
    bool running;
    bool use_psram;
    aes67_net_config_t net_config;
    uint32_t tic_frames;            /* frames the TIC feeds per tick (sizes source rings) */
    /* Sink receive-state watcher: a periodic timer compares each sink's
     * last arrival with now and reports transitions */
    esp_timer_handle_t rx_watch;
    aes67_rtp_rx_state_cb_t rx_state_cb;
    void *rx_state_user;
    /* Direct I2S playback from RX task */
    bool i2s_playback_enabled;
    void *audio_handle;
    /* Task to notify when new sink data arrives */
    TaskHandle_t playback_notify_task;
    /* Stream buffer for decoupling RTP RX from I2S DMA */
    StreamBufferHandle_t audio_stream_buf;
    /* Where decoded sink audio goes (audio_stream_buf unless redirected) */
    StreamBufferHandle_t playout_buf;

    /* Ethernet frame receive path */
    portMUX_TYPE rx_lock;
    rx_table_entry_t rx_table[AES67_MAX_STREAMS];
    int32_t *rx_scratch;            /* RX_SCRATCH_FRAMES x max channels, internal */
    int32_t *rx_out;                /* RX_SCRATCH_FRAMES x 2 slots, internal */
    int32_t *tx_scratch;            /* capture routing for non-stereo sources */
    aes67_rtp_rx_stats_t rx_stats;
    /* Raw lwIP UDP PCB -- processes packets directly in lwIP context,
     * bypassing socket layer and its ~412us per-packet overhead. */
    struct udp_pcb *raw_pcb;
    int32_t *raw_sample_buf;  /* Pre-allocated conversion buffer */
};

/* --- Ring buffer helpers --- */

/* Available bytes to read */
static inline uint32_t ringbuf_available(const aes67_ringbuf_t *rb)
{
    uint32_t wp = rb->write_pos;
    uint32_t rp = rb->read_pos;
    if (wp >= rp) {
        return wp - rp;
    }
    return rb->size - rp + wp;
}

/* Free space available for writing */
static inline uint32_t ringbuf_free(const aes67_ringbuf_t *rb)
{
    /* Reserve one byte to distinguish full from empty */
    uint32_t avail = ringbuf_available(rb);
    return rb->size - 1 - avail;
}

/* Write data into ring buffer. Returns bytes actually written. */
static uint32_t ringbuf_write(aes67_ringbuf_t *rb, const uint8_t *data, uint32_t len)
{
    uint32_t free_bytes = ringbuf_free(rb);
    if (len > free_bytes) {
        len = free_bytes;
    }
    if (len == 0) {
        return 0;
    }

    uint32_t wp = rb->write_pos;
    uint32_t first = rb->size - wp;
    if (first >= len) {
        memcpy(rb->data + wp, data, len);
    } else {
        memcpy(rb->data + wp, data, first);
        memcpy(rb->data, data + first, len - first);
    }

    /* Atomic update of write position */
    rb->write_pos = (wp + len) % rb->size;
    return len;
}

/* Read data from ring buffer. Returns bytes actually read. */
static uint32_t ringbuf_read(aes67_ringbuf_t *rb, uint8_t *data, uint32_t len)
{
    uint32_t avail = ringbuf_available(rb);
    if (len > avail) {
        len = avail;
    }
    if (len == 0) {
        return 0;
    }

    uint32_t rp = rb->read_pos;
    uint32_t first = rb->size - rp;
    if (first >= len) {
        memcpy(data, rb->data + rp, len);
    } else {
        memcpy(data, rb->data + rp, first);
        memcpy(data + first, rb->data, len - first);
    }

    /* Atomic update of read position */
    rb->read_pos = (rp + len) % rb->size;
    return len;
}

/* --- RTP header construction/parsing --- */

/* --- Stream table lock --- */

static inline void engine_lock(struct aes67_rtp_engine *engine)
{
    xSemaphoreTake(engine->lock, portMAX_DELAY);
}

static inline void engine_unlock(struct aes67_rtp_engine *engine)
{
    xSemaphoreGive(engine->lock);
}

static void rtp_build_header(uint8_t *buf, uint8_t pt, uint16_t seq,
                             uint32_t timestamp, uint32_t ssrc)
{
    buf[0] = (AES67_RTP_VERSION << 6); /* V=2, no padding, no extension, CC=0 */
    buf[1] = pt & 0x7F;                /* No marker bit */
    buf[2] = (seq >> 8) & 0xFF;
    buf[3] = seq & 0xFF;
    buf[4] = (timestamp >> 24) & 0xFF;
    buf[5] = (timestamp >> 16) & 0xFF;
    buf[6] = (timestamp >> 8) & 0xFF;
    buf[7] = timestamp & 0xFF;
    buf[8] = (ssrc >> 24) & 0xFF;
    buf[9] = (ssrc >> 16) & 0xFF;
    buf[10] = (ssrc >> 8) & 0xFF;
    buf[11] = ssrc & 0xFF;
}

static bool rtp_parse_header(const uint8_t *buf, size_t len,
                             aes67_rtp_header_t *hdr)
{
    if (len < AES67_RTP_HEADER_SIZE) {
        return false;
    }

    uint8_t version = (buf[0] >> 6) & 0x03;
    if (version != AES67_RTP_VERSION) {
        return false;
    }

    hdr->flags = buf[0];
    hdr->pt = buf[1] & 0x7F;
    hdr->seq = (uint16_t)(buf[2] << 8) | buf[3];
    hdr->timestamp = ((uint32_t)buf[4] << 24) | ((uint32_t)buf[5] << 16) |
                     ((uint32_t)buf[6] << 8) | buf[7];
    hdr->ssrc = ((uint32_t)buf[8] << 24) | ((uint32_t)buf[9] << 16) |
                ((uint32_t)buf[10] << 8) | buf[11];
    return true;
}

/* --- Stream allocation helpers --- */

static esp_err_t stream_alloc_buffers(struct aes67_rtp_stream *stream,
                                      bool use_psram, uint32_t tic_frames)
{
    uint32_t buf_size;

    if (stream->direction == AES67_STREAM_SINK) {
        /* Sink ring buffer stores int32_t samples (4 bytes each) regardless
         * of the wire word length. Jitter buffer: 4x packet size. */
        uint32_t frame_size = stream->config.channels * sizeof(int32_t);
        uint32_t packet_bytes = stream->samples_per_packet * frame_size;
        buf_size = packet_bytes * AES67_JITTER_BUF_MULT;
    } else {
        /* Source TX buffer stores int32_t samples (4 bytes each); the
         * convert-to-net step packs them into word_length on send. The TIC
         * writes a tick's worth of frames at a time, so the ring must hold
         * several ticks even when the packet is much shorter than a tick. */
        uint32_t frame_size = stream->config.channels * sizeof(int32_t);
        uint32_t unit = stream->samples_per_packet;
        if (tic_frames > unit) {
            unit = tic_frames;
        }
        buf_size = unit * frame_size * AES67_JITTER_BUF_MULT;
    }

    /* Add 1 byte for ring buffer full/empty distinction */
    buf_size += 1;

    /* Always use internal SRAM for jitter buffer -- PSRAM reads add
     * latency that causes the 10% throughput loss in i2s_channel_write path */
    uint32_t caps = MALLOC_CAP_INTERNAL;
    stream->ring.data = heap_caps_malloc(buf_size, caps);
    if (!stream->ring.data) {
        ESP_LOGE(TAG, "Failed to allocate ring buffer (%lu bytes)", (unsigned long)buf_size);
        return ESP_ERR_NO_MEM;
    }
    memset(stream->ring.data, 0, buf_size);
    stream->ring.size = buf_size;
    stream->ring.write_pos = 0;
    stream->ring.read_pos = 0;

    /* TX packet scratch buffer always in internal SRAM for DMA access */
    if (stream->direction == AES67_STREAM_SOURCE) {
        uint32_t pkt_size = AES67_RTP_HEADER_SIZE + stream->packet_payload_size;
        stream->tx_packet_buf = heap_caps_malloc(pkt_size,
                                                 MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        if (!stream->tx_packet_buf) {
            ESP_LOGE(TAG, "Failed to allocate TX packet buffer (%lu bytes)",
                     (unsigned long)pkt_size);
            heap_caps_free(stream->ring.data);
            stream->ring.data = NULL;
            return ESP_ERR_NO_MEM;
        }
    }

    return ESP_OK;
}

static void stream_free_buffers(struct aes67_rtp_stream *stream)
{
    if (stream->ring.data) {
        heap_caps_free(stream->ring.data);
        stream->ring.data = NULL;
    }
    if (stream->tx_packet_buf) {
        heap_caps_free(stream->tx_packet_buf);
        stream->tx_packet_buf = NULL;
    }
}

/* Find a sink stream matching the incoming packet by SSRC or dest addr */
static struct aes67_rtp_stream *find_sink_stream(struct aes67_rtp_engine *engine,
                                                  uint32_t ssrc,
                                                  const struct sockaddr_in *src_addr)
{
    for (int i = 0; i < AES67_MAX_STREAMS; i++) {
        struct aes67_rtp_stream *s = engine->streams[i];
        if (!s || s->direction != AES67_STREAM_SINK || !s->active) {
            continue;
        }

        /* Match by SSRC first */
        if (s->ssrc == ssrc) {
            return s;
        }

        /* Match by configured source SSRC */
        if (s->config.ssrc != 0 && s->config.ssrc == ssrc) {
            s->ssrc = ssrc;
            return s;
        }
    }

    /* If no SSRC match, find a sink that has SSRC=0 (auto-detect mode) */
    for (int i = 0; i < AES67_MAX_STREAMS; i++) {
        struct aes67_rtp_stream *s = engine->streams[i];
        if (!s || s->direction != AES67_STREAM_SINK || !s->active) {
            continue;
        }
        if (s->config.ssrc == 0 && s->first_packet) {
            /* Lock onto this SSRC */
            s->ssrc = ssrc;
            s->first_packet = false;
            ESP_LOGI(TAG, "Sink stream locked to SSRC 0x%08lx",
                     (unsigned long)ssrc);
            return s;
        }
    }

    return NULL;
}

/* --- Raw lwIP UDP callback (runs in lwIP task context) --- */

/* Called directly by lwIP when a UDP packet arrives on port 5004.
 * No socket layer, no message queue, no context switch.
 * Processes RTP packets with ~10us overhead instead of ~412us. */
static void raw_udp_recv_cb(void *arg, struct udp_pcb *pcb,
                             struct pbuf *p, const ip_addr_t *addr, u16_t port)
{
    struct aes67_rtp_engine *engine = (struct aes67_rtp_engine *)arg;

    static uint32_t raw_cb_count = 0;
    static uint32_t raw_cb_processed = 0;
    raw_cb_count++;

    if (!p || p->tot_len < AES67_RTP_HEADER_SIZE) {
        if (p) pbuf_free(p);
        return;
    }

    /* Copy pbuf chain to contiguous buffer */
    uint8_t pkt_buf[2048];
    if (p->tot_len > sizeof(pkt_buf)) {
        pbuf_free(p);
        return;
    }
    pbuf_copy_partial(p, pkt_buf, p->tot_len, 0);
    uint16_t recv_len = p->tot_len;
    pbuf_free(p);

    /* Parse RTP header */
    aes67_rtp_header_t hdr;
    if (!rtp_parse_header(pkt_buf, recv_len, &hdr)) {
        return;
    }

    /* Skip own TX packets */
    for (int i = 0; i < AES67_MAX_STREAMS; i++) {
        struct aes67_rtp_stream *s = engine->streams[i];
        if (s && s->direction == AES67_STREAM_SOURCE && s->ssrc == hdr.ssrc) {
            return;
        }
    }

    /* Find matching sink stream */
    struct aes67_rtp_stream *stream = find_sink_stream(engine, hdr.ssrc, NULL);
    if (!stream) return;

    /* Validate payload type */
    if (hdr.pt != stream->config.payload_type) return;

    /* Track sequence */
    uint16_t expected_seq = stream->status.last_seq + 1;
    if (stream->status.packets_received > 0 && hdr.seq != expected_seq) {
        int16_t diff = (int16_t)(hdr.seq - expected_seq);
        if (diff > 0) stream->status.packets_lost += diff;
        stream->status.seq_errors++;
    }
    stream->status.last_seq = hdr.seq;
    stream->status.packets_received++;

    /* Convert from network format to int32 */
    const uint8_t *payload = pkt_buf + AES67_RTP_HEADER_SIZE;
    uint32_t payload_len = recv_len - AES67_RTP_HEADER_SIZE;
    uint8_t channels = stream->config.channels;
    uint8_t wl = stream->config.word_length;
    uint32_t frame_size_bytes = channels * wl;
    if (frame_size_bytes == 0 || payload_len < frame_size_bytes) return;
    uint32_t frames = payload_len / frame_size_bytes;
    if (frames > AES67_MAX_TIC_FRAMES) frames = AES67_MAX_TIC_FRAMES;

    int32_t *sample_buf = engine->raw_sample_buf;
    stream->convert_from_net(payload, sample_buf, frames, channels);

    /* Write converted int32 samples to stream buffer for playback */
    if (engine->audio_stream_buf) {
        xStreamBufferSend(engine->audio_stream_buf, sample_buf,
                           frames * channels * sizeof(int32_t), 0);
    }

    raw_cb_processed++;

    /* Log first packet and periodic stats */
    if (stream->status.packets_received == 1) {
        const char *codec_str = (wl == 2) ? "L16" : (wl == 3) ? "L24" : "L32";
        ESP_LOGI(TAG, "RAW RX: %s/%luHz/%uch, %u frames",
                 codec_str, (unsigned long)stream->config.sample_rate,
                 channels, frames);
    }
    if ((raw_cb_count % 5000) == 0) {
        ESP_LOGI(TAG, "RAW: cb=%lu proc=%lu lost=%lu",
                 (unsigned long)raw_cb_count,
                 (unsigned long)raw_cb_processed,
                 (unsigned long)stream->status.packets_lost);
    }
}

/* --- RX task --- */

static void rx_task_func(void *arg)
{
    struct aes67_rtp_engine *engine = (struct aes67_rtp_engine *)arg;
    uint8_t *rx_buf = heap_caps_malloc(AES67_RX_BUF_SIZE,
                                       MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!rx_buf) {
        ESP_LOGE(TAG, "Failed to allocate RX buffer");
            xSemaphoreGive(engine->rx_done);
    vTaskDelete(NULL);
        return;
    }

    /* Temporary buffer for converted samples. 192 frames covers 4ms at 48kHz,
     * which is the practical AES67 maximum. */
    int32_t *sample_buf = heap_caps_malloc(
        CONFIG_AES67_MAX_CHANNELS_PER_STREAM * 192 * sizeof(int32_t),
        MALLOC_CAP_INTERNAL);
    if (!sample_buf) {
        ESP_LOGE(TAG, "Failed to allocate sample conversion buffer");
        heap_caps_free(rx_buf);
            xSemaphoreGive(engine->rx_done);
    vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "RX task started on port %u", engine->net_config.rtp_port);

    /* Timing stats and packet arrival watchdog */
    static int64_t t_recv_total = 0, t_proc_total = 0;
    static uint32_t t_count = 0;
    static int64_t last_pkt_time_us = 0;
    static int64_t last_stall_warn_us = 0;

    while (engine->running) {
        struct sockaddr_in src_addr;
        socklen_t addr_len = sizeof(src_addr);

        /* First recv blocks until a packet arrives.
         * Subsequent recvs use MSG_DONTWAIT to drain the socket buffer
         * without blocking, reducing the per-packet lwIP overhead. */
        static int drain_pass = 0;
        int flags = drain_pass ? MSG_DONTWAIT : 0;

        int64_t t0 = esp_timer_get_time();
        int recv_len = recvfrom(engine->rx_sock, rx_buf, AES67_RX_BUF_SIZE, flags,
                                (struct sockaddr *)&src_addr, &addr_len);
        int64_t t1 = esp_timer_get_time();
        if (recv_len < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                drain_pass = 0;
                /* Stall watchdog disabled: Ethernet hook handles RTP
                 * reception at L2, so the socket RX path sees nothing. */
                continue;
            }
            if (!engine->running) {
                break;
            }
            ESP_LOGW(TAG, "recvfrom error: %d (%s)", errno, strerror(errno));
            drain_pass = 0;
            continue;
        }
        drain_pass = 1;
        last_pkt_time_us = esp_timer_get_time();

        /* Everything below touches the stream table and a stream found in
         * it; a 'continue' inside the do-while(0) leaves the block and
         * releases the lock. */
        engine_lock(engine);
        do {
        if (recv_len < AES67_RTP_HEADER_SIZE) {
            continue;
        }

        /* Parse RTP header */
        aes67_rtp_header_t hdr;
        if (!rtp_parse_header(rx_buf, recv_len, &hdr)) {
            continue;
        }

        /* Skip our own TX packets (same port, different multicast group).
         * Check all source streams for SSRC match. */
        bool is_own = false;
        for (int i = 0; i < AES67_MAX_STREAMS && !is_own; i++) {
            struct aes67_rtp_stream *s = engine->streams[i];
            if (s && s->direction == AES67_STREAM_SOURCE && s->ssrc == hdr.ssrc) {
                is_own = true;
            }
        }
        if (is_own) continue;

        /* Find the matching sink stream */
        struct aes67_rtp_stream *stream = find_sink_stream(engine, hdr.ssrc,
                                                            &src_addr);
        if (!stream) {
            continue;
        }

        /* Validate payload type */
        if (hdr.pt != stream->config.payload_type) {
            stream->status.status_flags |= AES67_RTP_STATUS_PT_ERROR;
            continue;
        }

        /* Check sequence continuity */
        uint16_t expected_seq = stream->status.last_seq + 1;
        if (stream->status.packets_received > 0 && hdr.seq != expected_seq) {
            int16_t diff = (int16_t)(hdr.seq - expected_seq);
            if (diff > 0) {
                stream->status.packets_lost += diff;
            }
            stream->status.seq_errors++;
            stream->status.status_flags |= AES67_RTP_STATUS_SEQ_ERROR;
        }

        stream->status.last_seq = hdr.seq;
        stream->status.last_rtp_timestamp = hdr.timestamp;
        stream->status.status_flags |= AES67_RTP_STATUS_RECEIVING;

        /* Measure inter-packet arrival jitter (RFC 3550 algorithm).
         * Uses esp_timer for microsecond precision. */
        int64_t now_us = esp_timer_get_time();
        if (stream->last_rx_time_us > 0) {
            int64_t expected_us = (int64_t)stream->samples_per_packet
                                  * 1000000LL / stream->config.sample_rate;
            int64_t actual_us = now_us - stream->last_rx_time_us;
            int32_t deviation = (int32_t)(actual_us - expected_us);
            if (deviation < 0) deviation = -deviation;
            /* Exponential moving average: jitter += (deviation - jitter) / 16 */
            stream->jitter_us += (deviation - stream->jitter_us) / 16;
            stream->status.jitter_us = stream->jitter_us;
        }
        stream->last_rx_time_us = now_us;

        /* Convert from network byte order to native samples.
         * Use actual payload size to determine frame count, not the
         * SDP-declared samples_per_packet (which may differ). */
        const uint8_t *payload = rx_buf + AES67_RTP_HEADER_SIZE;
        uint32_t payload_len = recv_len - AES67_RTP_HEADER_SIZE;
        uint8_t channels = stream->config.channels;
        uint8_t wl = stream->config.word_length;

        /* Compute actual frames from payload length */
        uint32_t frame_size_bytes = channels * wl;
        if (frame_size_bytes == 0 || payload_len < frame_size_bytes) {
            continue;
        }
        uint32_t frames = payload_len / frame_size_bytes;

        /* Limit to conversion buffer capacity (192 frames max) */
        uint32_t max_frames = CONFIG_AES67_MAX_CHANNELS_PER_STREAM *
                              192 / channels;
        if (frames > max_frames) frames = max_frames;

        stream->convert_from_net(payload, sample_buf, frames, channels);

        /* Route converted int32 samples to stream buffer or jitter buffer */
        if (engine->playout_buf) {
            xStreamBufferSend(engine->playout_buf, sample_buf,
                               frames * channels * sizeof(int32_t), 0);
        } else {
            uint32_t data_bytes = frames * channels * sizeof(int32_t);
            ringbuf_write(&stream->ring,
                          (const uint8_t *)sample_buf, data_bytes);
        }

        stream->status.packets_received++;

        int64_t t2 = esp_timer_get_time();
        t_recv_total += (t1 - t0);
        t_proc_total += (t2 - t1);
        t_count++;

        if ((stream->status.packets_received % 10000) == 0) {
            ESP_LOGI(TAG, "RX: %luk pkts, %lu lost, j=%ldus | "
                     "recv=%lldus proc=%lldus cycle=%lldus (avg over %lu)",
                     (unsigned long)(stream->status.packets_received / 1000),
                     (unsigned long)stream->status.packets_lost,
                     (long)stream->jitter_us,
                     t_count > 0 ? (long long)(t_recv_total / t_count) : 0,
                     t_count > 0 ? (long long)(t_proc_total / t_count) : 0,
                     t_count > 0 ? (long long)((t_recv_total + t_proc_total) / t_count) : 0,
                     (unsigned long)t_count);
        }

        /* Log first packet only */
        if (stream->status.packets_received == 1) {
            const char *codec_str = (wl == 2) ? "L16" : (wl == 3) ? "L24" : (wl == 4) ? "L32" : "???";
            ESP_LOGI(TAG, "RX: %s/%luHz/%uch, %lu bytes, %u frames",
                     codec_str, (unsigned long)stream->config.sample_rate,
                     channels, (unsigned long)payload_len, frames);
        }
        } while (0);
        engine_unlock(engine);
    }

    heap_caps_free(sample_buf);
    heap_caps_free(rx_buf);
    ESP_LOGI(TAG, "RX task stopped");
        xSemaphoreGive(engine->rx_done);
    vTaskDelete(NULL);
}

/* --- Public API --- */

esp_err_t aes67_rtp_engine_init(const aes67_net_config_t *net_config,
                                bool use_psram,
                                aes67_rtp_engine_handle_t *handle)
{
    if (!net_config || !handle) {
        return ESP_ERR_INVALID_ARG;
    }

    struct aes67_rtp_engine *engine = calloc(1, sizeof(*engine));
    if (!engine) {
        ESP_LOGE(TAG, "Failed to allocate engine");
        return ESP_ERR_NO_MEM;
    }

    memcpy(&engine->net_config, net_config, sizeof(aes67_net_config_t));
    engine->use_psram = use_psram;
    engine->running = false;
    engine->rx_sock = -1;
    engine->rx_task = NULL;
    engine->stream_count = 0;

    engine->lock = xSemaphoreCreateMutex();
    engine->rx_done = xSemaphoreCreateBinary();
    if (!engine->lock || !engine->rx_done) {
        ESP_LOGE(TAG, "Failed to create engine semaphores");
        if (engine->lock) vSemaphoreDelete(engine->lock);
        if (engine->rx_done) vSemaphoreDelete(engine->rx_done);
        free(engine);
        return ESP_ERR_NO_MEM;
    }

    /* Stream buffer: sized for practical worst-case, not theoretical max.
     * AES67 mandates max 8 channels, max 4ms packet time at 48kHz = 192 frames.
     * 192 * 8ch * 4 bytes = 6144 per packet. Buffer 8 packets = 49152. */
    uint32_t practical_max_pkt = 192 * CONFIG_AES67_MAX_CHANNELS_PER_STREAM * sizeof(int32_t);
    uint32_t sbuf_size = practical_max_pkt * 8;
    engine->audio_stream_buf = xStreamBufferCreate(sbuf_size, practical_max_pkt);

    /* Raw lwIP UDP PCB disabled when Ethernet hook is active.
     * The hook intercepts RTP at L2 before lwIP, so the raw PCB would
     * either see nothing (packets freed by hook) or duplicate-process
     * packets that slip through, wasting CPU and memory.
     * The raw PCB is only useful as fallback when the hook is not installed. */
#if 0  /* Disabled: Ethernet hook handles RTP reception */
    engine->raw_pcb = udp_new();
    if (engine->raw_pcb) {
        err_t err = udp_bind(engine->raw_pcb, IP_ADDR_ANY, net_config->rtp_port);
        if (err == ERR_OK) {
            udp_recv(engine->raw_pcb, raw_udp_recv_cb, engine);
            ESP_LOGI(TAG, "Raw UDP PCB bound to port %u", net_config->rtp_port);
        } else {
            udp_remove(engine->raw_pcb);
            engine->raw_pcb = NULL;
        }
    }
    engine->raw_sample_buf = heap_caps_malloc(
        CONFIG_AES67_MAX_CHANNELS_PER_STREAM * 192 * sizeof(int32_t),
        MALLOC_CAP_INTERNAL);
#else
    engine->raw_pcb = NULL;
    engine->raw_sample_buf = NULL;
    ESP_LOGI(TAG, "Raw UDP PCB disabled (Ethernet hook handles RTP)");
#endif
    if (!engine->audio_stream_buf) {
        ESP_LOGE(TAG, "Failed to create audio stream buffer");
    }
    engine->playout_buf = engine->audio_stream_buf;

    portMUX_INITIALIZE(&engine->rx_lock);
    engine->rx_scratch = heap_caps_malloc(
        RX_SCRATCH_FRAMES * CONFIG_AES67_MAX_CHANNELS_PER_STREAM * sizeof(int32_t),
        MALLOC_CAP_INTERNAL);
    engine->rx_out = heap_caps_malloc(RX_SCRATCH_FRAMES * 2 * sizeof(int32_t), MALLOC_CAP_INTERNAL);
    engine->tx_scratch = heap_caps_malloc(
        RX_SCRATCH_FRAMES * CONFIG_AES67_MAX_CHANNELS_PER_STREAM * sizeof(int32_t),
        MALLOC_CAP_INTERNAL);
    if (!engine->rx_scratch || !engine->rx_out || !engine->tx_scratch) {
        ESP_LOGE(TAG, "Failed to allocate frame scratch buffers");
        heap_caps_free(engine->rx_scratch);
        heap_caps_free(engine->rx_out);
        heap_caps_free(engine->tx_scratch);
        if (engine->audio_stream_buf) vStreamBufferDelete(engine->audio_stream_buf);
        vSemaphoreDelete(engine->lock);
        vSemaphoreDelete(engine->rx_done);
        free(engine);
        return ESP_ERR_NO_MEM;
    }

    /* Create the RX socket only if raw PCB is not active.
     * Raw PCB already binds to the port, so socket bind would fail. */
    if (!engine->raw_pcb) {
        engine->rx_sock = aes67_net_create_udp_socket(net_config->rtp_port, true);
        if (engine->rx_sock < 0) {
            ESP_LOGE(TAG, "Failed to create RX socket on port %u",
                     net_config->rtp_port);
            if (engine->audio_stream_buf) vStreamBufferDelete(engine->audio_stream_buf);
            heap_caps_free(engine->rx_scratch);
            heap_caps_free(engine->rx_out);
            heap_caps_free(engine->tx_scratch);
            vSemaphoreDelete(engine->lock);
            vSemaphoreDelete(engine->rx_done);
            free(engine);
            return ESP_FAIL;
        }
    } else {
        /* Still need a socket for TX and multicast joins */
        engine->rx_sock = aes67_net_create_udp_socket(0, true);  /* Ephemeral port */
    }

    /* Set socket receive timeout so the RX task can check the running flag */
    struct timeval tv = { .tv_sec = 0, .tv_usec = 100000 }; /* 100ms */
    setsockopt(engine->rx_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    /* RX buffer: 32KB holds ~28 packets at 1152 bytes each = ~112ms */
    aes67_net_set_buffers(engine->rx_sock, 0, 32768);

    *handle = engine;
    ESP_LOGI(TAG, "Engine initialized, RX socket on port %u",
             net_config->rtp_port);
    return ESP_OK;
}

StreamBufferHandle_t aes67_rtp_engine_get_stream_buf(aes67_rtp_engine_handle_t handle)
{
    return handle ? handle->audio_stream_buf : NULL;
}

void aes67_rtp_engine_set_rx_state_cb(aes67_rtp_engine_handle_t handle,
                                      aes67_rtp_rx_state_cb_t cb, void *user_data)
{
    if (handle) {
        handle->rx_state_cb = cb;
        handle->rx_state_user = user_data;
    }
}

/* Periodic (250 ms, esp_timer task): report sinks that started or stopped
 * receiving. The frame path only records arrival times, so the transition
 * to silence has to be noticed here. */
static void rx_watch_cb(void *arg)
{
    struct aes67_rtp_engine *engine = arg;
    if (!engine->rx_state_cb) {
        return;
    }
    int64_t now_us = esp_timer_get_time();
    for (int i = 0; i < AES67_MAX_STREAMS; i++) {
        struct aes67_rtp_stream *s;
        bool active, reported, changed = false;
        portENTER_CRITICAL(&engine->rx_lock);
        s = engine->streams[i];
        if (s && s->direction == AES67_STREAM_SINK) {
            active = s->last_rx_time_us > 0 &&
                     now_us - s->last_rx_time_us < AES67_RTP_RX_TIMEOUT_US;
            reported = s->rx_reported_active;
            if (active != reported) {
                s->rx_reported_active = active;
                changed = true;
            }
        }
        portEXIT_CRITICAL(&engine->rx_lock);
        if (changed) {
            engine->rx_state_cb(s, active, engine->rx_state_user);
        }
    }
}

void aes67_rtp_engine_set_tic_frames(aes67_rtp_engine_handle_t handle, uint32_t frames)
{
    if (handle) {
        handle->tic_frames = frames;
    }
}

void aes67_rtp_engine_set_notify_task(aes67_rtp_engine_handle_t handle,
                                      TaskHandle_t task)
{
    if (handle) {
        handle->playback_notify_task = task;
    }
}

void aes67_rtp_engine_set_playback(aes67_rtp_engine_handle_t handle,
                                   void *audio_handle)
{
    if (handle) {
        handle->audio_handle = audio_handle;
        handle->i2s_playback_enabled = (audio_handle != NULL);
        ESP_LOGI(TAG, "I2S playback from RX task %s",
                 handle->i2s_playback_enabled ? "enabled" : "disabled");
    }
}

esp_err_t aes67_rtp_engine_start(aes67_rtp_engine_handle_t handle)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }

    struct aes67_rtp_engine *engine = handle;
    if (engine->running) {
        return ESP_ERR_INVALID_STATE;
    }

    engine->running = true;

    /* Start the RX task */
    /* RX task priority must be BELOW lwIP (22) so lwIP can process
     * incoming Ethernet frames. RX task runs when lwIP delivers
     * data to the socket, not the other way around. */
    BaseType_t ret = xTaskCreatePinnedToCore(
        rx_task_func,
        "aes67_rx",
        AES67_RX_TASK_STACK,
        engine,
        17,  /* Below lwIP (23) */
        &engine->rx_task,
        0   /* Core 0: all audio/network on same core for cache coherency */
    );
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create RX task");
        engine->running = false;
        return ESP_FAIL;
    }

    if (!engine->rx_watch) {
        const esp_timer_create_args_t args = {
            .callback = rx_watch_cb,
            .arg = engine,
            .name = "aes67_rxw",
        };
        if (esp_timer_create(&args, &engine->rx_watch) != ESP_OK) {
            ESP_LOGW(TAG, "receive-state watcher not created");
        }
    }
    if (engine->rx_watch) {
        esp_timer_start_periodic(engine->rx_watch, 250000);
    }

    ESP_LOGI(TAG, "Engine started");
    return ESP_OK;
}

esp_err_t aes67_rtp_engine_stop(aes67_rtp_engine_handle_t handle)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }

    struct aes67_rtp_engine *engine = handle;
    if (!engine->running) {
        return ESP_ERR_INVALID_STATE;
    }

    engine->running = false;
    if (engine->rx_watch) {
        esp_timer_stop(engine->rx_watch);
    }

    /* The RX task sees the flag on its next 100 ms receive timeout and
     * signals rx_done on the way out; wait for that instead of guessing. */
    if (engine->rx_task) {
        if (xSemaphoreTake(engine->rx_done, pdMS_TO_TICKS(1000)) != pdTRUE) {
            ESP_LOGW(TAG, "RX task did not exit within 1 s");
        }
        engine->rx_task = NULL;
    }

    ESP_LOGI(TAG, "Engine stopped");
    return ESP_OK;
}

esp_err_t aes67_rtp_engine_destroy(aes67_rtp_engine_handle_t handle)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }

    struct aes67_rtp_engine *engine = handle;
    if (engine->rx_watch) {
        esp_timer_stop(engine->rx_watch);
        esp_timer_delete(engine->rx_watch);
        engine->rx_watch = NULL;
    }

    /* Stop if still running */
    if (engine->running) {
        aes67_rtp_engine_stop(engine);
    }

    /* Remove all streams */
    for (int i = 0; i < AES67_MAX_STREAMS; i++) {
        if (engine->streams[i]) {
            aes67_rtp_stream_remove(engine, engine->streams[i]);
        }
    }

    /* Close RX socket */
    if (engine->rx_sock >= 0) {
        close(engine->rx_sock);
        engine->rx_sock = -1;
    }

    if (engine->audio_stream_buf) {
        vStreamBufferDelete(engine->audio_stream_buf);
    }
    heap_caps_free(engine->rx_scratch);
    heap_caps_free(engine->rx_out);
    heap_caps_free(engine->tx_scratch);
    vSemaphoreDelete(engine->lock);
    vSemaphoreDelete(engine->rx_done);
    free(engine);
    ESP_LOGI(TAG, "Engine destroyed");
    return ESP_OK;
}

esp_err_t aes67_rtp_stream_add(aes67_rtp_engine_handle_t engine_handle,
                               const aes67_rtp_stream_config_t *config,
                               aes67_rtp_stream_handle_t *stream)
{
    if (!engine_handle || !config || !stream) {
        return ESP_ERR_INVALID_ARG;
    }

    struct aes67_rtp_engine *engine = engine_handle;

    engine_lock(engine);

    /* Find a free slot */
    int slot = -1;
    for (int i = 0; i < AES67_MAX_STREAMS; i++) {
        if (!engine->streams[i]) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        engine_unlock(engine);
        ESP_LOGE(TAG, "Maximum number of streams reached (%d)", AES67_MAX_STREAMS);
        return ESP_ERR_NO_MEM;
    }

    struct aes67_rtp_stream *s = calloc(1, sizeof(*s));
    if (!s) {
        engine_unlock(engine);
        return ESP_ERR_NO_MEM;
    }

    memcpy(&s->config, config, sizeof(aes67_rtp_stream_config_t));
    s->direction = config->direction;
    s->ssrc = config->ssrc;
    s->seq_num = 0;
    s->tx_sock = -1;
    s->active = false;
    s->first_packet = true;
    memset(&s->status, 0, sizeof(s->status));

    /* Compute all derived stream parameters from base config */
    aes67_stream_params_compute(&s->params,
                                config->sample_rate,
                                config->channels,
                                config->word_length,
                                config->packet_time_us);
    s->samples_per_packet = s->params.samples_per_packet;
    s->packet_payload_size = s->params.packet_payload_size;

    /* Resolve codec conversion function pointers (MTConvert pattern).
     * This avoids branching on word_length in the per-packet hot path. */
    s->convert_to_net = aes67_get_to_net_fn(config->word_length);
    s->convert_from_net = aes67_get_from_net_fn(config->word_length);

    ESP_LOGD(TAG, "Stream: spp=%lu, payload=%lu, native=%lu bytes",
             (unsigned long)s->params.samples_per_packet,
             (unsigned long)s->params.packet_payload_size,
             (unsigned long)s->params.packet_native_size);

    /* Allocate ring and scratch buffers */
    esp_err_t err = stream_alloc_buffers(s, engine->use_psram, engine->tic_frames);
    if (err != ESP_OK) {
        free(s);
        engine_unlock(engine);
        return err;
    }

    /* Set up destination address */
    memset(&s->dest_addr, 0, sizeof(s->dest_addr));
    s->dest_addr.sin_family = AF_INET;
    s->dest_addr.sin_port = htons(config->dest_port);
    s->dest_addr.sin_addr.s_addr = config->dest_ip;

    /* Source streams: create a TX socket */
    if (s->direction == AES67_STREAM_SOURCE) {
        s->tx_sock = aes67_net_create_udp_socket(config->src_port, true);
        if (s->tx_sock < 0) {
            ESP_LOGE(TAG, "Failed to create TX socket on port %u",
                     config->src_port);
            stream_free_buffers(s);
            free(s);
            engine_unlock(engine);
            return ESP_FAIL;
        }

        /* Set QoS and TTL on the TX socket */
        aes67_net_set_dscp(s->tx_sock, config->dscp);
        aes67_net_set_multicast_ttl(s->tx_sock, config->ttl);
    }

    /* Sink streams: join multicast group */
    if (s->direction == AES67_STREAM_SINK && aes67_net_is_multicast(config->dest_ip)) {
        char mcast_str[16];
        aes67_net_u32_to_ip(config->dest_ip, mcast_str, sizeof(mcast_str));
        /* Join via socket (for legacy RX task) */
        err = aes67_net_join_multicast(engine->rx_sock, mcast_str, NULL);
        /* Also join via lwIP IGMP for the raw PCB path */
        if (engine->raw_pcb) {
            ip4_addr_t mcast_ip;
            mcast_ip.addr = config->dest_ip;
            igmp_joingroup(IP4_ADDR_ANY4, &mcast_ip);
        }
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to join multicast group %s", mcast_str);
        }
    }

    /* Output routing to the two I2S slots: routing[] names the stream
     * channel for each slot when it is in range; a mono stream feeds both
     * slots; otherwise channels 0 and 1. */
    for (int k = 0; k < 2; k++) {
        uint8_t r = config->routing[k];
        if (config->channels == 1) {
            s->out_map[k] = 0;
        } else if (r < config->channels) {
            s->out_map[k] = r;
        } else {
            s->out_map[k] = (uint8_t)(k < config->channels ? k : 0);
        }
    }

    s->active = true;
    engine->streams[slot] = s;
    engine->stream_count++;
    *stream = s;
    if (s->direction == AES67_STREAM_SINK) {
        portENTER_CRITICAL(&engine->rx_lock);
        engine->rx_table[slot].dest_ip = config->dest_ip;
        engine->rx_table[slot].dest_port = config->dest_port;
        engine->rx_table[slot].payload_type = config->payload_type;
        engine->rx_table[slot].stream = s;
        portEXIT_CRITICAL(&engine->rx_lock);
    }
    engine_unlock(engine);

    char ip_str[16];
    aes67_net_u32_to_ip(config->dest_ip, ip_str, sizeof(ip_str));
    ESP_LOGI(TAG, "Added %s stream [%d]: %s:%u, %u ch, %u Hz, %u-bit, SSRC=0x%08lx",
             s->direction == AES67_STREAM_SOURCE ? "source" : "sink",
             slot, ip_str, config->dest_port,
             config->channels, config->sample_rate,
             config->word_length * 8, (unsigned long)config->ssrc);

    return ESP_OK;
}

esp_err_t aes67_rtp_stream_remove(aes67_rtp_engine_handle_t engine_handle,
                                  aes67_rtp_stream_handle_t stream)
{
    if (!engine_handle || !stream) {
        return ESP_ERR_INVALID_ARG;
    }

    struct aes67_rtp_engine *engine = engine_handle;
    struct aes67_rtp_stream *s = stream;

    /* Unlink under the lock. Once the slot is clear and the lock released,
     * neither the TIC nor the RX task can reach s any more, so the rest of
     * the teardown runs without holding up the audio paths. */
    engine_lock(engine);
    int slot = -1;
    for (int i = 0; i < AES67_MAX_STREAMS; i++) {
        if (engine->streams[i] == s) {
            slot = i;
            engine->streams[i] = NULL;
            break;
        }
    }
    if (slot >= 0) {
        s->active = false;
        engine->stream_count--;
        /* The frame path holds rx_lock across its use of the entry, so
         * after this nobody reaches s through the table. */
        portENTER_CRITICAL(&engine->rx_lock);
        engine->rx_table[slot].stream = NULL;
        portEXIT_CRITICAL(&engine->rx_lock);
    }
    engine_unlock(engine);
    if (slot < 0) {
        return ESP_ERR_NOT_FOUND;
    }

    /* Leave multicast group for sink streams */
    if (s->direction == AES67_STREAM_SINK &&
        aes67_net_is_multicast(s->config.dest_ip)) {
        char mcast_str[16];
        aes67_net_u32_to_ip(s->config.dest_ip, mcast_str, sizeof(mcast_str));
        aes67_net_leave_multicast(engine->rx_sock, mcast_str, NULL);
    }

    /* Close TX socket for source streams */
    if (s->tx_sock >= 0) {
        close(s->tx_sock);
        s->tx_sock = -1;
    }

    stream_free_buffers(s);

    ESP_LOGI(TAG, "Removed stream [%d]", slot);
    free(s);
    return ESP_OK;
}

esp_err_t aes67_rtp_stream_get_status(aes67_rtp_stream_handle_t stream,
                                      aes67_rtp_stream_status_t *status)
{
    if (!stream || !status) {
        return ESP_ERR_INVALID_ARG;
    }

    memcpy(status, &stream->status, sizeof(aes67_rtp_stream_status_t));
    status->ssrc = stream->ssrc;
    if (stream->direction == AES67_STREAM_SINK) {
        int64_t now = esp_timer_get_time();
        if (stream->last_rx_time_us > 0 && now - stream->last_rx_time_us < RX_ACTIVE_WINDOW_US) {
            status->status_flags |= AES67_RTP_STATUS_RECEIVING;
        } else {
            status->status_flags &= ~AES67_RTP_STATUS_RECEIVING;
        }
    }
    return ESP_OK;
}

esp_err_t aes67_rtp_source_write(aes67_rtp_stream_handle_t stream,
                                 const void *samples, uint32_t frame_count)
{
    if (!stream || !samples) {
        return ESP_ERR_INVALID_ARG;
    }
    if (stream->direction != AES67_STREAM_SOURCE) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!stream->active) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Caller provides int32_t samples (left-justified). Store 4 bytes per
     * sample in the ring buffer regardless of the wire word_length. */
    uint32_t byte_count = frame_count * stream->config.channels * sizeof(int32_t);
    uint32_t written = ringbuf_write(&stream->ring, (const uint8_t *)samples,
                                     byte_count);
    if (written < byte_count) {
        stream->status.status_flags |= AES67_RTP_STATUS_OVERFLOW;
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

esp_err_t aes67_rtp_engine_set_playout_buf(aes67_rtp_engine_handle_t handle,
                                           StreamBufferHandle_t sbuf)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }
    handle->playout_buf = sbuf ? sbuf : handle->audio_stream_buf;
    return ESP_OK;
}

esp_err_t aes67_rtp_engine_get_rx_stats(aes67_rtp_engine_handle_t handle,
                                        aes67_rtp_rx_stats_t *stats)
{
    if (!handle || !stats) {
        return ESP_ERR_INVALID_ARG;
    }
    *stats = handle->rx_stats;
    return ESP_OK;
}

/*
 * Ethernet frame receive path. Runs in the Ethernet driver's task for every
 * frame, so the common case (not ours) must be a few comparisons. Headers
 * are validated in order: Ethernet type, IPv4 header and total length, UDP
 * length, then RTP fixed header, CSRC list, extension and padding, every
 * offset checked against the UDP payload end.
 */
bool IRAM_ATTR aes67_rtp_engine_rx_frame(aes67_rtp_engine_handle_t handle,
                                         const uint8_t *frame, uint32_t len)
{
    struct aes67_rtp_engine *engine = handle;
    if (!engine || len < 14 + 20 + 8 + 12) {
        return false;
    }
    if (frame[12] != 0x08 || frame[13] != 0x00) {
        return false;                                   /* not IPv4 */
    }
    const uint8_t *ip = frame + 14;
    uint32_t ihl = (ip[0] & 0x0F) * 4;
    if ((ip[0] >> 4) != 4 || ihl < 20 || ip[9] != 17) {
        return false;                                   /* not UDP */
    }
    if ((ip[16] & 0xF0) != 0xE0) {
        return false;                                   /* not multicast */
    }
    uint32_t ip_total = ((uint32_t)ip[2] << 8) | ip[3];
    if (ip_total < ihl + 8 || 14 + ip_total > len) {
        return false;
    }
    const uint8_t *udp = ip + ihl;
    uint16_t dport = (uint16_t)((udp[2] << 8) | udp[3]);
    uint32_t udp_len = ((uint32_t)udp[4] << 8) | udp[5];
    if (udp_len < 8 + 12 || udp_len > ip_total - ihl) {
        return false;
    }
    const uint8_t *rtp = udp + 8;
    uint32_t rtp_len = udp_len - 8;                     /* RTP header + payload */
    if ((rtp[0] >> 6) != 2) {
        return false;                                   /* not RTP v2 */
    }
    uint32_t dst_ip;
    memcpy(&dst_ip, ip + 16, 4);                        /* network order */
    uint8_t pt = rtp[1] & 0x7F;

    /* Match group, port and payload type; everything below runs under the
     * spinlock so the stream cannot be removed meanwhile. */
    portENTER_CRITICAL(&engine->rx_lock);
    struct aes67_rtp_stream *s = NULL;
    for (int i = 0; i < AES67_MAX_STREAMS; i++) {
        rx_table_entry_t *e = &engine->rx_table[i];
        if (e->stream && e->dest_ip == dst_ip && e->dest_port == dport) {
            if (e->payload_type == pt) {
                s = e->stream;
            }
            break;
        }
    }
    if (!s) {
        portEXIT_CRITICAL(&engine->rx_lock);
        engine->rx_stats.rtp_unmatched++;
        return false;                                   /* let lwIP have it */
    }

    /* RTP header: CSRC list, extension, padding */
    uint32_t cc = rtp[0] & 0x0F;
    uint32_t hdr_len = 12 + cc * 4;
    if (rtp[0] & 0x10) {                                /* extension */
        if (hdr_len + 4 > rtp_len) goto malformed;
        uint32_t ext_words = ((uint32_t)rtp[hdr_len + 2] << 8) | rtp[hdr_len + 3];
        hdr_len += 4 + ext_words * 4;
    }
    if (hdr_len >= rtp_len) goto malformed;
    uint32_t payload_len = rtp_len - hdr_len;
    if (rtp[0] & 0x20) {                                /* padding: last payload byte */
        uint32_t pad = rtp[rtp_len - 1];
        if (pad == 0 || pad > payload_len) goto malformed;
        payload_len -= pad;
    }
    const uint8_t *payload = rtp + hdr_len;

    uint32_t ssrc = ((uint32_t)rtp[8] << 24) | ((uint32_t)rtp[9] << 16) |
                    ((uint32_t)rtp[10] << 8) | rtp[11];
    uint16_t seq = (uint16_t)((rtp[2] << 8) | rtp[3]);
    uint32_t ts = ((uint32_t)rtp[4] << 24) | ((uint32_t)rtp[5] << 16) |
                  ((uint32_t)rtp[6] << 8) | rtp[7];

    /* SSRC: lock to the first sender on this group and stay with it while
     * it is sending. A different SSRC is accepted once the locked one has
     * been silent for the timeout: the source restarted (senders pick a
     * new SSRC) and the sink follows without being re-subscribed. */
    int64_t now_us = esp_timer_get_time();
    if (s->first_packet) {
        s->ssrc = ssrc;
        s->first_packet = false;
    } else if (s->ssrc != ssrc) {
        if (now_us - s->last_rx_time_us > AES67_RTP_RX_TIMEOUT_US) {
            s->ssrc = ssrc;
            s->status.ssrc_relocks++;
            s->status.last_seq = (uint16_t)(seq - 1);   /* no loss across the restart */
        } else {
            s->status.ssrc_mismatches++;
            s->status.status_flags |= AES67_RTP_STATUS_SSRC_ERROR;
            engine->rx_stats.ssrc_rejected++;
            portEXIT_CRITICAL(&engine->rx_lock);
            return true;                                /* ours, but not this sender */
        }
    }

    uint8_t channels = s->config.channels;
    uint8_t wl = s->config.word_length;
    uint32_t frame_bytes = (uint32_t)channels * wl;
    if (frame_bytes == 0 || payload_len < frame_bytes) goto malformed;
    uint32_t frames = payload_len / frame_bytes;
    if (frames > RX_SCRATCH_FRAMES) frames = RX_SCRATCH_FRAMES;

    /* Statistics: sequence, loss, inter-arrival jitter */
    if (s->status.packets_received > 0) {
        uint16_t expected = (uint16_t)(s->status.last_seq + 1);
        if (seq != expected) {
            int16_t diff = (int16_t)(seq - expected);
            if (diff > 0) s->status.packets_lost += diff;
            s->status.seq_errors++;
            s->status.status_flags |= AES67_RTP_STATUS_SEQ_ERROR;
        }
        if (s->last_rx_time_us > 0) {
            int64_t expected_us = (int64_t)s->samples_per_packet * 1000000LL /
                                  s->config.sample_rate;
            int32_t dev = (int32_t)((now_us - s->last_rx_time_us) - expected_us);
            if (dev < 0) dev = -dev;
            s->jitter_us += (dev - s->jitter_us) / 16;
            s->status.jitter_us = s->jitter_us;
        }
    }
    s->status.last_seq = seq;
    s->status.last_rtp_timestamp = ts;
    s->status.packets_received++;
    s->status.bytes_received += payload_len;
    s->last_rx_time_us = now_us;

    /* Decode to int32 and route stream channels onto the two output slots */
    s->convert_from_net(payload, engine->rx_scratch, frames, channels);
    const int32_t *in = engine->rx_scratch;
    int32_t *out = engine->rx_out;
    uint8_t m0 = s->out_map[0], m1 = s->out_map[1];
    for (uint32_t f = 0; f < frames; f++) {
        out[2 * f]     = in[f * channels + m0];
        out[2 * f + 1] = in[f * channels + m1];
    }
    StreamBufferHandle_t sbuf = engine->playout_buf;
    portEXIT_CRITICAL(&engine->rx_lock);

    engine->rx_stats.rtp_consumed++;
    if (sbuf) {
        size_t bytes = frames * 2 * sizeof(int32_t);
        if (xStreamBufferSend(sbuf, engine->rx_out, bytes, 0) < bytes) {
            engine->rx_stats.playout_overflow++;
        }
    }
    return true;

malformed:
    s->status.status_flags |= AES67_RTP_STATUS_PT_ERROR;
    portEXIT_CRITICAL(&engine->rx_lock);
    engine->rx_stats.rtp_malformed++;
    return true;                                        /* ours, drop it */
}

esp_err_t aes67_rtp_engine_feed_capture(aes67_rtp_engine_handle_t handle,
                                        const int32_t *samples, uint32_t frame_count)
{
    if (!handle || !samples) {
        return ESP_ERR_INVALID_ARG;
    }
    struct aes67_rtp_engine *engine = handle;

    if (frame_count > RX_SCRATCH_FRAMES) {
        frame_count = RX_SCRATCH_FRAMES;
    }

    engine_lock(engine);
    for (int i = 0; i < AES67_MAX_STREAMS; i++) {
        struct aes67_rtp_stream *s = engine->streams[i];
        if (!s || s->direction != AES67_STREAM_SOURCE || !s->active) {
            continue;
        }
        uint8_t ch = s->config.channels;
        const int32_t *src = samples;
        if (ch != 2) {
            /* Capture is two slots; a mono source takes the left slot, a
             * wider source gets the two slots in channels 0 and 1 and
             * silence in the rest. */
            int32_t *dst = engine->tx_scratch;
            for (uint32_t f = 0; f < frame_count; f++) {
                for (uint8_t c = 0; c < ch; c++) {
                    dst[f * ch + c] = (c < 2) ? samples[2 * f + c] : 0;
                }
            }
            src = dst;
        }
        uint32_t byte_count = frame_count * ch * sizeof(int32_t);
        uint32_t written = ringbuf_write(&s->ring, (const uint8_t *)src, byte_count);
        if (written < byte_count) {
            s->status.status_flags |= AES67_RTP_STATUS_OVERFLOW;
        }
    }
    engine_unlock(engine);
    return ESP_OK;
}

esp_err_t aes67_rtp_sink_read(aes67_rtp_stream_handle_t stream,
                              void *samples, uint32_t frame_count)
{
    if (!stream || !samples) {
        return ESP_ERR_INVALID_ARG;
    }
    if (stream->direction != AES67_STREAM_SINK) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Ring buffer stores int32_t samples (4 bytes each) */
    uint32_t byte_count = frame_count * stream->config.channels * sizeof(int32_t);

    /* If stream is muted, return silence */
    if (stream->status.status_flags & AES67_RTP_STATUS_MUTED) {
        memset(samples, 0, byte_count);
        return ESP_OK;
    }

    uint32_t avail = ringbuf_available(&stream->ring);
    if (avail < byte_count) {
        memset(samples, 0, byte_count);
        stream->status.status_flags |= AES67_RTP_STATUS_UNDERFLOW;
        return ESP_ERR_NOT_FOUND;
    }

    uint32_t read_bytes = ringbuf_read(&stream->ring, (uint8_t *)samples,
                                       byte_count);
    if (read_bytes < byte_count) {
        /* Partial read, zero remainder */
        memset((uint8_t *)samples + read_bytes, 0, byte_count - read_bytes);
        stream->status.status_flags |= AES67_RTP_STATUS_UNDERFLOW;
        return ESP_ERR_NOT_FOUND;
    }

    /* Clear underflow flag on successful read */
    stream->status.status_flags &= ~AES67_RTP_STATUS_UNDERFLOW;
    return ESP_OK;
}

uint32_t aes67_rtp_sink_available(aes67_rtp_stream_handle_t stream)
{
    if (!stream || stream->direction != AES67_STREAM_SINK) return 0;
    uint32_t bytes = ringbuf_available(&stream->ring);
    uint32_t frame_bytes = stream->config.channels * sizeof(int32_t);
    return frame_bytes > 0 ? bytes / frame_bytes : 0;
}

esp_err_t aes67_rtp_engine_process_tx(aes67_rtp_engine_handle_t handle,
                                      uint32_t rtp_timestamp)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }

    struct aes67_rtp_engine *engine = handle;
    if (!engine->running) {
        return ESP_ERR_INVALID_STATE;
    }

    engine_lock(engine);
    for (int i = 0; i < AES67_MAX_STREAMS; i++) {
        struct aes67_rtp_stream *s = engine->streams[i];
        if (!s || s->direction != AES67_STREAM_SOURCE || !s->active) {
            continue;
        }

        /* Initialize stream timestamp from PTP on first TX, then
         * increment sequentially. The receiver validates that each
         * packet's timestamp = prev_timestamp + prev_sample_count.
         * (RAVENNA RTP_audio_stream.c line 647) */
        /* The audio in this packet was captured during the previous
         * frame period, so stamp it one packet behind the PTP-derived
         * "now" (quantized to the packet grid). Receivers then measure a
         * small positive network delay instead of a zero or negative one. */
        uint32_t spp = s->samples_per_packet;
        uint32_t ptp_ts = rtp_timestamp - (rtp_timestamp % spp) - spp;

        if (!s->timestamp_initialized) {
            s->rtp_timestamp = ptp_ts;
            s->timestamp_initialized = true;
        } else {
            /* Stay anchored to PTP: receivers (AES67/RAVENNA) compute
             * playout from RTP timestamp vs. their own PTP time, so a
             * timestamp seeded before PTP sync, or left behind by a clock
             * step or underruns, makes every packet "late" and dropped.
             * Re-anchor to the PTP-derived value, quantized to the packet
             * grid, when the running timestamp deviates by more than a
             * few packets. Small deviations keep sequential continuity. */
            int32_t diff = (int32_t)(ptp_ts - s->rtp_timestamp);
            /* A source whose packet is shorter than the tick is legitimately
             * up to a tick's worth of packets behind at the start of a tick */
            uint32_t unit = spp > engine->tic_frames ? spp : engine->tic_frames;
            int32_t limit = (int32_t)(unit * 4);
            if (diff > limit || diff < -limit) {
                /* Log at most once per 10 s: a blocking UART print here
                 * costs several ticks and would re-trigger this path. */
                static int64_t s_last_log_us;
                int64_t now_us = esp_timer_get_time();
                if (now_us - s_last_log_us > 10000000LL) {
                    s_last_log_us = now_us;
                    ESP_LOGW(TAG, "stream [%d] RTP timestamp re-anchored to PTP (off by %ld samples)",
                             i, (long)diff);
                }
                s->status.timestamp_resyncs++;
                s->rtp_timestamp = ptp_ts;
            }
        }

        /* Send every packet that has fallen due. The tick is the node's
         * packet time; a source with a shorter one sends several packets
         * per tick, a source with a longer one sends every few ticks. A
         * packet is due once the tick has passed the end of its audio. */
        uint32_t samples_total = spp * s->config.channels;
        uint32_t needed_bytes = samples_total * sizeof(int32_t);
        uint8_t channels = s->config.channels;
        uint8_t wl = s->config.word_length;
        uint32_t guard = 0;

        while ((int32_t)(ptp_ts - s->rtp_timestamp) >= 0 && guard++ < 64) {
            if (ringbuf_available(&s->ring) < needed_bytes) {
                /* Capture underrun: skip this packet but keep media time
                 * moving, so the stream reads as one lost packet instead
                 * of sliding behind PTP time. */
                s->status.status_flags |= AES67_RTP_STATUS_UNDERFLOW;
                s->seq_num++;
                s->rtp_timestamp += spp;
                continue;
            }

            uint8_t *pkt = s->tx_packet_buf;
            uint8_t *payload = pkt + AES67_RTP_HEADER_SIZE;

            /* The ring holds int32 samples; the payload area is sized for
             * the packed wire format, so read through a stack buffer in
             * chunks and convert each chunk into place. */
            int32_t sample_buf[CONFIG_AES67_MAX_CHANNELS_PER_STREAM * 48];
            uint32_t chunk_frames = sizeof(sample_buf) / (channels * sizeof(int32_t));
            uint32_t frames_done = 0;
            uint8_t *payload_ptr = payload;

            while (frames_done < spp) {
                uint32_t batch = spp - frames_done;
                if (batch > chunk_frames) batch = chunk_frames;

                uint32_t batch_bytes = batch * channels * sizeof(int32_t);
                uint32_t read_bytes = ringbuf_read(&s->ring,
                                                   (uint8_t *)sample_buf,
                                                   batch_bytes);
                if (read_bytes < batch_bytes) {
                    s->status.status_flags |= AES67_RTP_STATUS_UNDERFLOW;
                    break;
                }

                /* Convert int32_t native samples to packed big-endian wire format */
                s->convert_to_net(sample_buf, payload_ptr, batch, channels);

                payload_ptr += batch * channels * wl;
                frames_done += batch;
            }

            if (frames_done < spp) {
                break;              /* cannot happen with a single reader; be safe */
            }

            rtp_build_header(pkt, s->config.payload_type, s->seq_num,
                             s->rtp_timestamp, s->ssrc);
            s->seq_num++;
            s->rtp_timestamp += spp;

            /* Send the packet */
            uint32_t pkt_size = AES67_RTP_HEADER_SIZE + s->packet_payload_size;
            ssize_t sent = sendto(s->tx_sock, pkt, pkt_size, 0,
                                  (struct sockaddr *)&s->dest_addr,
                                  sizeof(s->dest_addr));
            if (sent < 0) {
                ESP_LOGW(TAG, "sendto failed for stream [%d]: %d (%s)",
                         i, errno, strerror(errno));
            } else {
                s->status.packets_sent++;
                s->status.last_rtp_timestamp = s->rtp_timestamp - spp;
                s->status.last_seq = s->seq_num - 1;
                /* Clear underflow on successful send */
                s->status.status_flags &= ~AES67_RTP_STATUS_UNDERFLOW;
            }
        }
    }
    engine_unlock(engine);

    return ESP_OK;
}
