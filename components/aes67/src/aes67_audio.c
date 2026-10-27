/*
 * SPDX-FileCopyrightText: 2026 DatanoiseTV
 *
 * SPDX-License-Identifier: Apache-2.0
 */
/*
 * I2S audio driver for AES67 on ESP32-P4.
 *
 * Uses ESP-IDF I2S standard mode with DMA for audio capture and playback.
 * Ring buffers bridge between I2S DMA transfers and the RTP engine.
 * DMA buffers reside in internal SRAM; ring buffers may use PSRAM.
 */

#include "aes67_audio.h"
#include "aes67_audio_internal.h"
#include <string.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2s_std.h"
#include "driver/i2s_common.h"
#include "esp_timer.h"
#include "esp_cache.h"

static const char *TAG = "aes67_audio";

/* Kept for future capture (RX) task */

/* I2S DMA: 2 descriptors at 16 frames (0.33ms) each.
 * ISR fires at 3000Hz. DMA ring latency = 0.33ms.
 * Stable at all ptimes (0.125ms to 4ms) with 100% utilization.
 * DMA_FRAME_NUM=6 (8000Hz ISR) causes 0.03% loss at the edge. */
#define DMA_DESC_NUM            2
#define DMA_FRAME_NUM           16


struct aes67_audio_ctx {
    i2s_chan_handle_t tx_chan;
    i2s_chan_handle_t rx_chan;
    aes67_audio_config_t config;
    bool running;
    uint32_t frame_size;        /* frames per packet period */
};

/* --- Helpers --------------------------------------------------------------- */


/* In ESP32-P4 Philips I2S mode with 32-bit slot width, the DMA data
 * is already left-justified (MSB-first) in the 32-bit word. This means
 * the I2S DMA format matches our internal left-justified int32 format
 * directly. No bit shifting is needed regardless of the configured
 * data bit width (16/24/32), because the I2S hardware handles the
 * slot-to-wire packing. */



/* --- DMA <-> Ring Buffer Conversion --------------------------------------- */



/* --- DMA callbacks -------------------------------------------------------- */

/* --- StreamBuffer-based ISR playback --- */

/* The Ethernet hook writes variable-size packets (52-192 frames depending
 * on source ptime). The DMA ISR reads exactly 1536 bytes per fire
 * (one descriptor). A byte-stream buffer naturally handles variable
 * packet sizes by concatenating them until the ISR has a full descriptor. */
static StreamBufferHandle_t s_isr_stream_buf = NULL;
#define AES67_AUDIO_STREAM_BUF_BYTES 65536   /* ~170 ms of stereo int32 at 48 kHz */
static StreamBufferHandle_t s_capture_stream_buf = NULL;
/* Task to wake once when the next capture chunk has landed (one-shot) */
static TaskHandle_t volatile s_capture_waiter = NULL;

/* ISR diagnostics: interval counters reset by aes67_audio_log_isr_stats()
 * and totals that are never reset */
static volatile uint32_t s_isr_fires = 0;
static volatile uint32_t s_isr_full = 0;
static volatile uint32_t s_isr_empty = 0;
static volatile uint32_t s_isr_fires_total = 0;
static volatile uint32_t s_isr_dropouts = 0;     /* playout ran dry while it was playing */
static bool s_isr_was_full = false;

/* Playout clock snapshot. The TX DMA ISR runs once per descriptor, i.e.
 * every DMA_FRAME_NUM frames the DAC has consumed; it records how many
 * frames have gone out and what the reference clock read at that moment.
 * The clock-recovery loop compares the two to see how far the audio clock
 * has run against PTP. A sequence counter makes the pair consistent for a
 * reader that is interrupted by the writer. */
#define PLAYOUT_FRAME_BYTES     (2 * sizeof(int32_t))   /* two 32-bit slots */
static aes67_audio_clock_fn_t s_clock_fn = NULL;
static volatile uint32_t s_snap_seq = 0;
static uint64_t s_snap_frames = 0;
static uint64_t s_snap_time_ns = 0;
static uint64_t s_frames_out = 0;
/* A descriptor whose interrupt was not serviced in time (interrupts held
 * off for longer than one descriptor period) was still clocked out by the
 * DMA, replaying stale data. It is detected from the gap between service
 * times and counted, so the frame count stays true to what the DAC did. */
static uint64_t s_last_isr_ns = 0;
static uint32_t s_desc_period_ns = 0;
static volatile uint32_t s_isr_missed = 0;


/* Last frame for sample-hold on underrun (less audible than silence) */
static int32_t s_last_frame[2] = {0, 0};  /* Stereo max */

/* ISR callback: reads from the stream buffer to fill one DMA descriptor.
 * On underrun, uses sample-hold (repeat last frame) instead of silence
 * to mask the ~8% EMAC packet loss with minimal artifacts. */
static IRAM_ATTR bool on_i2s_tx_sent_sbuf(i2s_chan_handle_t handle,
                                            i2s_event_data_t *event,
                                            void *user_ctx)
{
    BaseType_t woken = pdFALSE;
    s_isr_fires++;
    s_isr_fires_total++;

    /* Frames clocked out so far, stamped with the reference clock */
    uint32_t frames = event->size / PLAYOUT_FRAME_BYTES;
    s_frames_out += frames;
    if (s_clock_fn) {
        uint64_t now = s_clock_fn();
        if (s_last_isr_ns != 0 && s_desc_period_ns != 0 && now > s_last_isr_ns) {
            uint64_t gap = now - s_last_isr_ns;
            if (gap > s_desc_period_ns + s_desc_period_ns / 2) {
                uint32_t missed = (uint32_t)((gap + s_desc_period_ns / 2) / s_desc_period_ns) - 1;
                /* Beyond 100 ms it is a step of the reference clock, not a
                 * stall; leave the count alone (the clock loop re-anchors) */
                if (missed <= 300) {
                    s_frames_out += (uint64_t)missed * frames;
                    s_isr_missed += missed;
                }
            }
        }
        s_last_isr_ns = now;
        s_snap_seq++;                   /* odd: being written */
        s_snap_frames = s_frames_out;
        s_snap_time_ns = now;
        s_snap_seq++;                   /* even: consistent */
    }

    if (s_isr_stream_buf) {
        size_t received = xStreamBufferReceiveFromISR(s_isr_stream_buf,
                                                       event->dma_buf,
                                                       event->size,
                                                       &woken);
        if (received >= event->size) {
            s_isr_full++;
            s_isr_was_full = true;
            /* Save last frame for sample-hold on next underrun */
            int32_t *samples = (int32_t *)event->dma_buf;
            uint32_t last_idx = (event->size / sizeof(int32_t)) - 2;
            s_last_frame[0] = samples[last_idx];
            s_last_frame[1] = samples[last_idx + 1];
        } else {
            s_isr_empty++;
            /* A dropout is the moment a playing stream runs dry; descriptors
             * with nothing to play while no sink is active are not counted */
            if (s_isr_was_full) {
                s_isr_dropouts++;
                s_isr_was_full = false;
            }
            /* Sample-hold: fill with last known frame instead of silence.
             * Decays toward zero to avoid DC offset on stream stop. */
            int32_t *dst = (int32_t *)event->dma_buf;
            uint32_t n_samples = event->size / sizeof(int32_t);
            /* Fill any partial data gap */
            uint32_t start = received / sizeof(int32_t);
            for (uint32_t i = start; i < n_samples; i += 2) {
                dst[i]     = s_last_frame[0];
                dst[i + 1] = s_last_frame[1];
            }
            /* Decay toward zero (fade over ~10 underruns = 10ms) */
            s_last_frame[0] = (s_last_frame[0] * 7) / 8;
            s_last_frame[1] = (s_last_frame[1] * 7) / 8;
        }
    } else {
        s_isr_empty++;
        memset(event->dma_buf, 0, event->size);
    }

    esp_cache_msync(event->dma_buf, event->size, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    return woken == pdTRUE;
}

/* Log ISR stats -- only on first lock and on underruns */
void aes67_audio_log_isr_stats(void)
{
    static bool first_100_logged = false;
    static uint32_t prev_empty_total = 0;

    uint32_t fires = s_isr_fires;
    uint32_t full = s_isr_full;
    uint32_t empty = s_isr_empty;
    s_isr_fires = 0;
    s_isr_full = 0;
    s_isr_empty = 0;

    if (fires == 0) return;

    uint32_t pct = full * 100 / fires;

    /* Log once when first reaching 100% */
    if (!first_100_logged && pct == 100) {
        first_100_logged = true;
        ESP_LOGI("dma_isr", "locked: %lu Hz, 100%% utilization", (unsigned long)(fires / 2));
    }

    /* Log whenever there are underruns (after initial lock) */
    if (first_100_logged && empty > 0) {
        prev_empty_total += empty;
        ESP_LOGW("dma_isr", "underrun: %lu empty in %lu fires (%lu%%), total=%lu",
                 (unsigned long)empty, (unsigned long)fires,
                 (unsigned long)pct, (unsigned long)prev_empty_total);
    }
}

StreamBufferHandle_t aes67_audio_get_playout_buf(aes67_audio_handle_t handle)
{
    (void)handle;
    return s_isr_stream_buf;
}

void aes67_audio_set_clock_source(aes67_audio_handle_t handle, aes67_audio_clock_fn_t now_ns)
{
    (void)handle;
    s_clock_fn = now_ns;
}

esp_err_t aes67_audio_get_playout_snapshot(aes67_audio_handle_t handle,
                                           uint64_t *frames_out, uint64_t *time_ns)
{
    if (!handle || !frames_out || !time_ns) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_clock_fn) {
        return ESP_ERR_INVALID_STATE;
    }
    /* Retry while the ISR is writing (odd) or wrote in between */
    for (int tries = 0; tries < 8; tries++) {
        uint32_t seq1 = s_snap_seq;
        if (seq1 & 1) {
            continue;
        }
        uint64_t f = s_snap_frames;
        uint64_t t = s_snap_time_ns;
        if (s_snap_seq == seq1) {
            if (seq1 == 0) {
                return ESP_ERR_NOT_FOUND;       /* no snapshot yet */
            }
            *frames_out = f;
            *time_ns = t;
            return ESP_OK;
        }
    }
    return ESP_ERR_TIMEOUT;
}

esp_err_t aes67_audio_get_dma_stats(aes67_audio_handle_t handle, aes67_audio_dma_stats_t *stats)
{
    if (!handle || !stats) {
        return ESP_ERR_INVALID_ARG;
    }
    stats->isr_fires = s_isr_fires_total;
    stats->isr_missed = s_isr_missed;
    stats->dropouts = s_isr_dropouts;
    return ESP_OK;
}


/* --- I2S RX Capture ISR (I2S -> AES67 TX source) --- */

/* Capture stream buffer: RX DMA ISR writes here, TIC task reads (declared above) */

/* RX DMA ISR: fires when a DMA descriptor finishes capturing.
 * Copies captured int32 samples to the capture stream buffer.
 * The TIC task reads from this buffer to feed RTP source streams. */
static IRAM_ATTR bool on_i2s_rx_recv(i2s_chan_handle_t handle,
                                      i2s_event_data_t *event,
                                      void *user_ctx)
{
    if (s_capture_stream_buf && event->size > 0) {
        /* Invalidate CPU cache: DMA wrote to memory, CPU cache may be stale */
        esp_cache_msync(event->dma_buf, event->size, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
        BaseType_t woken = pdFALSE;
        xStreamBufferSendFromISR(s_capture_stream_buf, event->dma_buf,
                                  event->size, &woken);
        TaskHandle_t waiter = s_capture_waiter;
        if (waiter) {
            s_capture_waiter = NULL;
            vTaskNotifyGiveFromISR(waiter, &woken);
        }
        return woken == pdTRUE;
    }
    return false;
}

/* Get the capture stream buffer (called by TIC task to read captured audio) */
StreamBufferHandle_t aes67_audio_get_capture_buf(aes67_audio_handle_t handle)
{
    (void)handle;
    return s_capture_stream_buf;
}

void aes67_audio_capture_notify_once(aes67_audio_handle_t handle, TaskHandle_t task)
{
    (void)handle;
    s_capture_waiter = task;
}



/* --- Public API ----------------------------------------------------------- */

esp_err_t aes67_audio_init(const aes67_audio_config_t *audio_config,
                           const aes67_i2s_pins_t *pins,
                           bool use_psram,
                           uint16_t ring_buf_ms,
                           aes67_audio_handle_t *handle)
{
    if (!audio_config || !pins || !handle) {
        return ESP_ERR_INVALID_ARG;
    }

    struct aes67_audio_ctx *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) {
        ESP_LOGE(TAG, "Failed to allocate audio context");
        return ESP_ERR_NO_MEM;
    }

    memcpy(&ctx->config, audio_config, sizeof(aes67_audio_config_t));
    (void)use_psram;            /* the playout and capture buffers are internal */
    (void)ring_buf_ms;

    /* Compute frame size: samples per packet period */
    ctx->frame_size = aes67_frames_per_packet(audio_config->sample_rate,
                                              audio_config->packet_time_us);
    if (ctx->frame_size == 0) {
        ESP_LOGE(TAG, "Invalid sample_rate/packet_time combination");
        free(ctx);
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Init: %lu Hz, %u ch, %u bytes/sample, frame_size=%lu",
             (unsigned long)audio_config->sample_rate,
             audio_config->channels,
             audio_config->word_length,
             (unsigned long)ctx->frame_size);

    esp_err_t ret;

    /* --- Configure I2S channels --- */
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG((i2s_port_t)CONFIG_AES67_I2S_PORT, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = DMA_DESC_NUM;
    chan_cfg.dma_frame_num = DMA_FRAME_NUM;
    /* When using ISR callback, auto_clear doesn't matter because the ISR
     * fills every descriptor (with data or silence). Set false for
     * i2s_channel_write compatibility in non-ISR modes. */
    chan_cfg.auto_clear = true;

    /* The DMA ISR plays from, and the capture ISR records into, two
     * stream buffers the driver owns (single instance, file statics). */
    if (!s_isr_stream_buf) {
        s_isr_stream_buf = xStreamBufferCreate(AES67_AUDIO_STREAM_BUF_BYTES, 1);
    }
    if (!s_capture_stream_buf) {
        s_capture_stream_buf = xStreamBufferCreate(AES67_AUDIO_STREAM_BUF_BYTES, 1);
    }
    if (!s_isr_stream_buf || !s_capture_stream_buf) {
        ESP_LOGE(TAG, "Failed to create playout/capture stream buffers");
        ret = ESP_ERR_NO_MEM;
        goto err_free_bufs;
    }

    ret = i2s_new_channel(&chan_cfg, &ctx->tx_chan, &ctx->rx_chan);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel failed: %s", esp_err_to_name(ret));
        goto err_free_bufs;
    }

    /* Map word_length to I2S bit depth */
    i2s_slot_mode_t slot_mode = I2S_SLOT_MODE_STEREO;

    /* 32-bit I2S: int32 maps directly to DMA, no conversion needed.
     * ES8311 supports 32-bit (SDP_IN_WL = 100). */
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(audio_config->sample_rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, slot_mode),
    };

    /* Use APLL clock source for precise audio frequency control.
     * APLL has SDM (Sigma-Delta Modulator) for sub-ppm tuning.
     * ESP32-P4 Ethernet uses MPLL, so no APLL conflict. */
    std_cfg.clk_cfg.clk_src = I2S_CLK_SRC_APLL;
    std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_384;

    std_cfg.gpio_cfg = (i2s_std_gpio_config_t){
        .mclk = pins->mck_gpio,
        .bclk = pins->bck_gpio,
        .ws   = pins->ws_gpio,
        .dout = pins->dout_gpio,
        .din  = pins->din_gpio,
        .invert_flags = {
            .mclk_inv = false,
            .bclk_inv = false,
            .ws_inv   = false,
        },
    };

    ret = i2s_channel_init_std_mode(ctx->tx_chan, &std_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_init_std_mode TX failed: %s", esp_err_to_name(ret));
        goto err_del_channel;
    }

    ret = i2s_channel_init_std_mode(ctx->rx_chan, &std_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_init_std_mode RX failed: %s", esp_err_to_name(ret));
        goto err_del_channel;
    }

    ctx->running = false;

    *handle = ctx;
    ESP_LOGI(TAG, "Audio driver initialized (DMA %d x %d frames, playout/capture buffers %u bytes)",
             DMA_DESC_NUM, DMA_FRAME_NUM, (unsigned)AES67_AUDIO_STREAM_BUF_BYTES);
    return ESP_OK;

err_del_channel:
    /* Only reached once both channels exist; the i2s_new_channel failure
     * path lands at err_free_bufs instead. */
    i2s_del_channel(ctx->tx_chan);
    i2s_del_channel(ctx->rx_chan);
err_free_bufs:
    free(ctx);
    return ret;
}

esp_err_t aes67_audio_start(aes67_audio_handle_t handle)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }
    if (handle->running) {
        ESP_LOGW(TAG, "Audio already running");
        return ESP_OK;
    }

    s_desc_period_ns = (uint32_t)((uint64_t)DMA_FRAME_NUM * 1000000000ULL / handle->config.sample_rate);
    s_last_isr_ns = 0;

    /* Register DMA ISR callback for direct stream buffer -> DMA transfer.
     * This bypasses i2s_channel_write and the playback task entirely. */
    /* Register TX DMA ISR (playback: stream buffer -> I2S) */
    if ((s_isr_stream_buf != NULL)) {
        i2s_event_callbacks_t tx_cbs = {
            .on_recv = NULL,
            .on_recv_q_ovf = NULL,
            .on_sent = on_i2s_tx_sent_sbuf,
            .on_send_q_ovf = NULL,
        };
        esp_err_t cb_ret = i2s_channel_register_event_callback(
            handle->tx_chan, &tx_cbs, handle);
        if (cb_ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to register TX ISR: %s", esp_err_to_name(cb_ret));
        } else {
            ESP_LOGI(TAG, "TX DMA ISR registered (playback)");
        }
    }

    /* Register RX DMA ISR (capture: I2S -> stream buffer) */
    if (s_capture_stream_buf != NULL) {
        i2s_event_callbacks_t rx_cbs = {
            .on_recv = on_i2s_rx_recv,
            .on_recv_q_ovf = NULL,
            .on_sent = NULL,
            .on_send_q_ovf = NULL,
        };
        esp_err_t cb_ret = i2s_channel_register_event_callback(
            handle->rx_chan, &rx_cbs, handle);
        if (cb_ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to register RX ISR: %s", esp_err_to_name(cb_ret));
        } else {
            ESP_LOGI(TAG, "RX DMA ISR registered (capture)");
        }
    }

    /* Enable both TX and RX channels. The ES8311 codec requires both
     * to be active (matching the official ESP-IDF es8311 example). */
    esp_err_t ret = i2s_channel_enable(handle->tx_chan);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable TX channel: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = i2s_channel_enable(handle->rx_chan);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to enable RX channel: %s", esp_err_to_name(ret));
        /* Non-fatal: TX still works */
    }

    handle->running = true;

    ESP_LOGI(TAG, "Audio started (DMA: %d desc x %d frames, DMA-paced)",
             DMA_DESC_NUM, DMA_FRAME_NUM);
    return ESP_OK;
}

esp_err_t aes67_audio_stop(aes67_audio_handle_t handle)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!handle->running) {
        return ESP_OK;
    }

    handle->running = false;

    /* Disable I2S channels */
    i2s_channel_disable(handle->tx_chan);
    i2s_channel_disable(handle->rx_chan);

    ESP_LOGI(TAG, "Audio stopped");
    return ESP_OK;
}

esp_err_t aes67_audio_destroy(aes67_audio_handle_t handle)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Stop if still running */
    if (handle->running) {
        aes67_audio_stop(handle);
    }

    /* Delete I2S channels */
    i2s_del_channel(handle->tx_chan);
    i2s_del_channel(handle->rx_chan);

    /* The two ISR stream buffers are file statics (single instance) */
    if (s_isr_stream_buf) {
        vStreamBufferDelete(s_isr_stream_buf);
        s_isr_stream_buf = NULL;
    }
    if (s_capture_stream_buf) {
        vStreamBufferDelete(s_capture_stream_buf);
        s_capture_stream_buf = NULL;
    }

    free(handle);
    ESP_LOGI(TAG, "Audio driver destroyed");
    return ESP_OK;
}





esp_err_t aes67_audio_get_config(aes67_audio_handle_t handle,
                                 aes67_audio_config_t *config)
{
    if (!handle || !config) {
        return ESP_ERR_INVALID_ARG;
    }

    memcpy(config, &handle->config, sizeof(aes67_audio_config_t));
    return ESP_OK;
}





i2s_chan_handle_t aes67_audio_get_tx_chan(aes67_audio_handle_t handle)
{
    return handle ? handle->tx_chan : NULL;
}
