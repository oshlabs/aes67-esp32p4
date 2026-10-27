/*
 * SPDX-FileCopyrightText: 2026 DatanoiseTV
 *
 * SPDX-License-Identifier: Apache-2.0
 */
/*
 * AES67/RAVENNA node lifecycle management.
 *
 * Coordinates initialization, startup and teardown of all AES67
 * subsystems: PTP, RTP engine, SAP discovery, audio I/O and the
 * session manager that ties them together.
 */

#include "aes67.h"
#include "aes67_config.h"
#include "aes67_ptp.h"
#include "aes67_rtp.h"
#include "aes67_sap.h"
#include "aes67_audio.h"
#include "aes67_session.h"
#include "aes67_hw_timer.h"
#include "aes67_net.h"
#include "aes67_eth_rx.h"
#include "aes67_clock.h"

#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"
#include "driver/i2s_std.h"

static const char *TAG = "aes67";

/* mDNS init/deinit -- declared here since aes67_mdns.c has no header */
esp_err_t aes67_mdns_init(const char *node_name, uint16_t rtp_port);
void aes67_mdns_deinit(void);

/* Define the event base for the ESP event loop */
ESP_EVENT_DEFINE_BASE(AES67_EVENT);

/* The TIC task, woken by the hardware PTP timer ISR with a task notification */
static TaskHandle_t s_tic_task = NULL;

/* Internal node structure holding all subsystem handles */
struct aes67_node {
    aes67_config_t              config;
    aes67_ptp_handle_t          ptp;
    aes67_rtp_engine_handle_t   rtp;
    aes67_sap_handle_t          sap;
    aes67_audio_handle_t        audio;
    aes67_session_handle_t      session;
    TaskHandle_t                tx_task;
    TaskHandle_t                pb_task;
    SemaphoreHandle_t           tic_done;   /* given by the TIC task on exit */
    SemaphoreHandle_t           pb_done;    /* given by the playback task on exit */
    bool                        running;
};

/* Global node pointer for the ISR callback (single instance) */
static aes67_node_handle_t s_node = NULL;

/* Frame task counters, read by aes67_node_get_tic_stats() (single writer) */
static aes67_node_tic_stats_t s_tic_stats;

/*
 * Hardware PTP timer ISR callback. Fires at exact PTP-clock-aligned
 * audio frame boundaries and wakes the TIC task. The wake-up is only a
 * signal: the task works out how many frame periods have elapsed from
 * PTP time, so a wake-up lost to a busy core does not lose a packet.
 */
static void IRAM_ATTR hw_frame_isr(void *user_data)
{
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    if (s_tic_task) {
        vTaskNotifyGiveFromISR(s_tic_task, &xHigherPriorityTaskWoken);
    }
    if (xHigherPriorityTaskWoken) {
        portYIELD_FROM_ISR();
    }
}

/* Catch-up bound: more than this many periods behind means a stall, not a
 * busy core; the capture backlog is dropped instead of being replayed. */
#define TIC_MAX_CATCHUP 8

/*
 * Unified audio frame task (TIC). Driven by the hardware PTP timer ISR,
 * this task handles both TX and RX audio processing at PTP-aligned
 * frame boundaries:
 *
 * TX path: Read from source ring buffers -> convert -> send RTP packets
 * RX path: Read from sink jitter buffers -> write to I2S playback
 *
 * This ensures both directions are synchronized to the PTP clock with
 * hardware-precision timing.
 */
static void audio_frame_task(void *arg)
{
    aes67_node_handle_t node = (aes67_node_handle_t)arg;
    uint32_t frame_count = 0;

    /* Pre-allocate playback buffer for sink -> I2S path */
    const uint32_t rate = node->config.audio.sample_rate;
    uint32_t spp = aes67_frames_per_packet(rate, node->config.audio.packet_time_us);
    int32_t *playback_buf = heap_caps_malloc(
        spp * node->config.audio.channels * sizeof(int32_t),
        MALLOC_CAP_INTERNAL);

    /* Capture buffer for I2S RX -> source stream feeding */
    StreamBufferHandle_t cap_buf = aes67_audio_get_capture_buf(node->audio);
    int32_t *cap_samples = NULL;
    if (cap_buf) {
        cap_samples = heap_caps_malloc(
            spp * node->config.audio.channels * sizeof(int32_t),
            MALLOC_CAP_INTERNAL);
    }

    ESP_LOGI(TAG, "Audio frame task started (hw-timed TX%s)",
             cap_buf ? "+capture" : "");

    const uint64_t period_ns = aes67_hw_timer_period_ns();
    const uint32_t need = spp * node->config.audio.channels * sizeof(int32_t);
    /* Wait at most two periods (at least 2 ms) for a wake-up before
     * treating the timer as stalled. */
    const TickType_t wait_ticks = pdMS_TO_TICKS((period_ns * 2) / 1000000 < 2 ? 2 : (period_ns * 2) / 1000000);
    /* Tick boundaries live on the media clock's frame grid (frame k of
     * PTP time is at k / rate seconds), so a tick's RTP timestamp is its
     * boundary's frame count and the grid never drifts from the seconds */
    uint64_t last_boundary_fr = 0;      /* frame count of the last processed boundary */
    uint32_t missed_waits = 0;
    uint32_t catchups = 0, stalls = 0, deferrals = 0;
    int64_t last_stall_log_us = 0;

    s_tic_task = xTaskGetCurrentTaskHandle();
    memset(&s_tic_stats, 0, sizeof(s_tic_stats));

    while (node->running) {
        /* Wait for the hardware PTP frame timer ISR */
        if (ulTaskNotifyTake(pdTRUE, wait_ticks) == 0) {
            if (++missed_waits >= 3) {
                /* Six periods without a tick: the timer is dead (a target
                 * written in the past, a backward PTP step). Re-arm it,
                 * drop the capture backlog so the stream does not carry a
                 * permanent extra delay, and restart the tick grid. */
                missed_waits = 0;
                stalls++;
                if (aes67_hw_timer_rearm() == ESP_OK && cap_buf && cap_samples) {
                    while (xStreamBufferBytesAvailable(cap_buf) > 2 * need) {
                        xStreamBufferReceive(cap_buf, cap_samples, need, 0);
                    }
                }
                last_boundary_fr = 0;
            }
            continue;
        }
        missed_waits = 0;

        /* How many frame periods have passed since the last one we
         * processed? Normally one. A wake-up lost while lwIP or the EMAC
         * task held the core shows up as two or three; process them all,
         * each with its own packet, so the stream stays on PTP time. */
        uint64_t now_ns;
        aes67_ptp_get_time_ns(node->ptp, &now_ns);
        uint64_t boundary_fr = (aes67_frames_from_ns(now_ns, rate) / spp) * spp;
        uint32_t due = 1;
        if (last_boundary_fr != 0 && boundary_fr > last_boundary_fr) {
            due = (uint32_t)((boundary_fr - last_boundary_fr) / spp);
            if (due > TIC_MAX_CATCHUP) {
                /* Too far behind to be a busy core: treat as a stall,
                 * drop the backlog and resume from here. */
                due = 1;
                stalls++;
                if (cap_buf && cap_samples) {
                    while (xStreamBufferBytesAvailable(cap_buf) > 2 * need) {
                        xStreamBufferReceive(cap_buf, cap_samples, need, 0);
                    }
                }
            }
        } else if (last_boundary_fr != 0 && boundary_fr == last_boundary_fr) {
            due = 0;                    /* woken twice within one period */
        }
        last_boundary_fr = boundary_fr;

        for (uint32_t k = 0; k < due; k++) {
            /* A tick is processed only once its packet of captured audio
             * is complete. The capture DMA delivers 16-frame chunks, so a
             * tick can find the newest packet one chunk short (with the
             * audio clock locked to PTP the chunk may land just after
             * every tick); sending nothing in that slot would leave a
             * sequence gap. Defer this and the following ticks and ask the
             * capture ISR for a wake-up when the chunk has landed, at most
             * one chunk period away, rather than wait a whole frame period. */
            if (cap_buf && cap_samples) {
                if (xStreamBufferBytesAvailable(cap_buf) < need) {
                    last_boundary_fr = boundary_fr - (uint64_t)(due - k) * spp;
                    aes67_audio_capture_notify_once(node->audio, s_tic_task);
                    deferrals++;
                    break;
                }
                xStreamBufferReceive(cap_buf, cap_samples, need, 0);
                aes67_rtp_engine_feed_capture(node->rtp, cap_samples, spp);
            }

            frame_count++;
            if (k > 0) {
                catchups++;
            }

            /* RTP timestamp for this tick's boundary: its frame count */
            uint32_t rtp_ts = (uint32_t)(boundary_fr - (uint64_t)(due - 1 - k) * spp);

            /* TX: Process all active source streams - read from ring buffers,
             * convert to network format, send RTP packets */
            aes67_rtp_engine_process_tx(node->rtp, rtp_ts);
        }

        /* Publish the counters; nothing is logged from here in steady
         * state, since a log line on the console holds this task for
         * longer than several frame periods. A stall is worth a (short,
         * rate-limited) warning. */
        s_tic_stats.ticks = frame_count;
        s_tic_stats.catchups = catchups;
        s_tic_stats.deferrals = deferrals;
        s_tic_stats.stalls = stalls;
        s_tic_stats.capture_backlog_frames = cap_buf
            ? xStreamBufferBytesAvailable(cap_buf) / (node->config.audio.channels * sizeof(int32_t)) : 0;
        if (stalls != s_tic_stats.stalls_logged) {
            int64_t now_us = esp_timer_get_time();
            if (now_us - last_stall_log_us > 10000000) {
                last_stall_log_us = now_us;
                ESP_LOGW(TAG, "TIC stall %lu", (unsigned long)stalls);
            }
            s_tic_stats.stalls_logged = stalls;
        }
    }

    s_tic_task = NULL;

    heap_caps_free(cap_samples);
    heap_caps_free(playback_buf);
    ESP_LOGI(TAG, "Audio frame task stopped");
    xSemaphoreGive(node->tic_done);
    vTaskDelete(NULL);
}

/*
 * Dedicated playback task. Runs in a tight loop reading from the
 * first active sink's jitter buffer and writing to I2S.
 * i2s_channel_write blocks until DMA has room, which naturally
 * paces output at 48kHz. This task is completely decoupled from
 * both the RTP RX task and the TIC task.
 */
static void playback_task(void *arg)
{
    aes67_node_handle_t node = (aes67_node_handle_t)arg;
    const uint8_t ch = node->config.audio.channels;

    /* Playback buffer: holds up to 768 frames (16ms at 48kHz = 4 DMA descriptors).
     * Reading multiple packets per i2s_channel_write call amortizes the ~0.4ms
     * overhead of xStreamBufferReceive + context switches. Without this, the
     * per-iteration overhead causes ~10% throughput loss. */
    const uint32_t max_frames = 768;
    const uint32_t buf_bytes = max_frames * ch * sizeof(int32_t);
    int32_t *pcm_buf = heap_caps_malloc(buf_bytes, MALLOC_CAP_INTERNAL);
    if (!pcm_buf) {
        ESP_LOGE(TAG, "Playback task: alloc failed");
            xSemaphoreGive(node->pb_done);
    vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Playback task started (ch=%u, DMA-paced, buf=%lu bytes)",
             ch, (unsigned long)buf_bytes);

    uint32_t total_frames = 0;
    uint32_t write_count = 0;
    bool sink_found = false;

    while (node->running) {
        /* Wait for a sink to be added */
        if (!sink_found) {
            int sink_count = aes67_session_get_sink_count(node->session);
            for (int s = 0; s < sink_count && s < CONFIG_AES67_MAX_SINKS; s++) {
                aes67_sink_t sink;
                if (aes67_session_get_sink(node->session, s, &sink) == ESP_OK &&
                    sink.enabled && sink.rtp_stream) {
                    sink_found = true;
                    ESP_LOGI(TAG, "Playback: locked to sink '%s' "
                             "(ch=%u, wl=%u, rate=%lu, ptime=%lu us)",
                             sink.name, sink.rtp_config.channels,
                             sink.rtp_config.word_length,
                             (unsigned long)sink.rtp_config.sample_rate,
                             (unsigned long)sink.rtp_config.packet_time_us);
                    break;
                }
            }
            if (!sink_found) {
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }
        }

        /* Get the stream buffer: the optional external feed (e.g. an Rx-hook
         * integration) if one was registered, otherwise the RTP engine's. */
        StreamBufferHandle_t sbuf = aes67_rtp_engine_get_stream_buf(node->rtp);
        if (!sbuf) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        size_t frame_align = ch * sizeof(int32_t);
        if (!sink_found) {
            continue;
        }

        /* Two-stage read: first blocking wait for at least one packet,
         * then immediately drain ALL accumulated data non-blocking.
         * This batches multiple packets into one i2s_channel_write call,
         * amortizing the ~0.4ms task scheduling overhead that otherwise
         * causes 10% throughput loss (4ms DMA cycle + 0.4ms overhead). */
        size_t received = xStreamBufferReceive(sbuf, pcm_buf, frame_align,
                                                pdMS_TO_TICKS(20));
        if (received < frame_align) {
            /* Stream stopped: flush silence through all DMA descriptors.
             * With auto_clear=false, old data replays unless we push zeros. */
            i2s_chan_handle_t tx = aes67_audio_get_tx_chan(node->audio);
            if (tx && node->config.output_mode == AES67_OUTPUT_I2S) {
                memset(pcm_buf, 0, buf_bytes);
                for (int d = 0; d < 4; d++) {
                    size_t written = 0;
                    i2s_channel_write(tx, pcm_buf, 192 * frame_align,
                                      &written, pdMS_TO_TICKS(10));
                }
            }
            continue;
        }
        /* Drain all remaining data immediately (non-blocking) */
        if (received < buf_bytes) {
            size_t extra = xStreamBufferReceive(sbuf, (uint8_t *)pcm_buf + received,
                                                buf_bytes - received, 0);
            received += extra;
        }

        /* Align to frame boundary */
        uint32_t frames = received / frame_align;
        uint32_t aligned_bytes = frames * frame_align;

        /* Route to output */
        switch (node->config.output_mode) {
        case AES67_OUTPUT_I2S: {
            i2s_chan_handle_t tx = aes67_audio_get_tx_chan(node->audio);
            if (tx) {
                size_t written = 0;
                i2s_channel_write(tx, pcm_buf, aligned_bytes,
                                  &written, portMAX_DELAY);
            }
            break;
        }
        case AES67_OUTPUT_CALLBACK:
            if (node->config.audio_cb) {
                node->config.audio_cb(pcm_buf, frames, ch,
                                      node->config.audio.word_length,
                                      node->config.audio.sample_rate,
                                      node->config.audio_cb_user_data);
            }
            break;
        case AES67_OUTPUT_STREAM_BUFFER:
            /* Data stays in stream buffer for app consumption */
            break;
        }

        total_frames += frames;
        write_count++;

        if ((write_count % 1000) == 0) {
            static int64_t last_stat_us = 0;
            static uint32_t last_stat_frames = 0;
            int64_t now = esp_timer_get_time();
            if (last_stat_us > 0) {
                int64_t delta_us = now - last_stat_us;
                uint32_t delta_frames = total_frames - last_stat_frames;
                if (delta_us > 0) {
                    ESP_LOGI(TAG, "PB: %lu fps, sb=%u",
                             (unsigned long)(delta_frames * 1000000ULL / delta_us),
                             (unsigned)xStreamBufferBytesAvailable(sbuf));
                }
            }
            last_stat_us = now;
            last_stat_frames = total_frames;
        }
    }

    heap_caps_free(pcm_buf);
    ESP_LOGI(TAG, "Playback task stopped");
    xSemaphoreGive(node->pb_done);
    vTaskDelete(NULL);
}

/*
 * PTP lock transitions: post to the application event loop (if one exists)
 * so applications can react, and log.
 */
static void node_ptp_lock_cb(aes67_ptp_lock_state_t state, void *user_data)
{
    aes67_event_t ev = (state == AES67_PTP_LOCKED) ? AES67_EVENT_PTP_LOCKED
                                                   : AES67_EVENT_PTP_UNLOCKED;
    esp_err_t err = esp_event_post(AES67_EVENT, ev, &state, sizeof(state), 0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGD(TAG, "event post failed: %s", esp_err_to_name(err));
    }
}

esp_err_t aes67_node_init(const aes67_config_t *config, aes67_node_handle_t *handle)
{
    if (!handle || !config || !config->net.eth_handle) {
        /* The Ethernet handle is required for PTP and the frame timer, so
         * a NULL config cannot work; say so instead of failing later. */
        return ESP_ERR_INVALID_ARG;
    }
    if (s_node) {
        ESP_LOGE(TAG, "only one node instance is supported");
        return ESP_ERR_INVALID_STATE;
    }

    struct aes67_node *node = calloc(1, sizeof(struct aes67_node));
    if (!node) {
        ESP_LOGE(TAG, "failed to allocate node context");
        return ESP_ERR_NO_MEM;
    }
    memcpy(&node->config, config, sizeof(aes67_config_t));

    /* Resolve the Ethernet netif once; everything below binds to this key. */
    aes67_net_set_interface(node->config.net.interface_name);
    if (!esp_netif_get_handle_from_ifkey(node->config.net.interface_name)) {
        ESP_LOGE(TAG, "no esp_netif with key \"%s\"; set config.net.interface_name "
                 "to the key the Ethernet netif was created with",
                 node->config.net.interface_name);
        free(node);
        return ESP_ERR_NOT_FOUND;
    }

    esp_err_t ret;

    /* 1. PTP clock subsystem */
    ret = aes67_ptp_init(node->config.net.eth_handle, &node->config.ptp,
                         node->config.net.interface_name, &node->ptp);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "PTP init failed: %s", esp_err_to_name(ret));
        goto fail_ptp;
    }

    /* 2. RTP packet engine */
    ret = aes67_rtp_engine_init(&node->config.net, node->config.use_psram, &node->rtp);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "RTP engine init failed: %s", esp_err_to_name(ret));
        goto fail_rtp;
    }

    /* 3. SAP discovery (optional) */
    if (node->config.sap_enabled) {
        ret = aes67_sap_init(&node->sap);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "SAP init failed: %s", esp_err_to_name(ret));
            goto fail_sap;
        }
        aes67_sap_set_timing(node->sap, node->config.sap_interval_s, node->config.sap_min_expiry_s);
    }

    /* 4. Audio I/O driver (I2S + DMA) */
    ret = aes67_audio_init(&node->config.audio, &node->config.i2s_pins,
                           node->config.use_psram, node->config.ring_buffer_ms,
                           &node->audio);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "audio init failed: %s", esp_err_to_name(ret));
        goto fail_audio;
    }

    /* 5. Session manager -- connects all subsystems */
    ret = aes67_session_init(node->rtp, node->ptp, node->sap,
                             node->audio, &node->config, &node->session);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "session manager init failed: %s", esp_err_to_name(ret));
        goto fail_session;
    }

    /* 6. PTP lock transitions reach the application through the event loop */
    aes67_ptp_register_lock_cb(node->ptp, node_ptp_lock_cb, node);

#if CONFIG_AES67_CLOCK_RECOVERY
    /* Audio clock recovery: the APLL follows PTP once both are up */
    ret = aes67_clock_init(node->audio, node->ptp, node->config.audio.sample_rate);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "clock recovery unavailable: %s (audio clock free-running)",
                 esp_err_to_name(ret));
    }
#endif

    /* 7. Initialize the hardware PTP frame timer for RTP TX: one tick
     * per packet's worth of frames, on the media clock's frame grid */
    uint32_t samples_per_pkt = aes67_frames_per_packet(node->config.audio.sample_rate,
                                                       node->config.audio.packet_time_us);
    aes67_rtp_engine_set_tic_frames(node->rtp, samples_per_pkt);

    node->tic_done = xSemaphoreCreateBinary();
    node->pb_done = xSemaphoreCreateBinary();
    if (!node->tic_done || !node->pb_done) {
        ESP_LOGE(TAG, "failed to create task semaphores");
        goto fail_cb;
    }

    ret = aes67_hw_timer_init(samples_per_pkt, node->config.audio.sample_rate,
                              hw_frame_isr, NULL);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "hw PTP timer init failed: %s (falling back to I2S timing)",
                 esp_err_to_name(ret));
        /* Non-fatal: I2S DMA callback still works as fallback */
    }

    s_node = node;
    node->running = false;
    *handle = node;

    ESP_LOGI(TAG, "node \"%s\" initialized (v%d.%d.%d)",
             node->config.node_name,
             AES67_VERSION_MAJOR, AES67_VERSION_MINOR, AES67_VERSION_PATCH);

    return ESP_OK;

fail_cb:
    if (node->tic_done) vSemaphoreDelete(node->tic_done);
    if (node->pb_done) vSemaphoreDelete(node->pb_done);
    aes67_session_destroy(node->session);
fail_session:
    aes67_audio_destroy(node->audio);
fail_audio:
    if (node->config.sap_enabled && node->sap) {
        aes67_sap_destroy(node->sap);
    }
fail_sap:
    aes67_rtp_engine_destroy(node->rtp);
fail_rtp:
    aes67_ptp_destroy(node->ptp);
fail_ptp:
    free(node);
    return ret;
}

esp_err_t aes67_node_start(aes67_node_handle_t handle)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }
    if (handle->running) {
        ESP_LOGW(TAG, "node already running");
        return ESP_OK;
    }

    esp_err_t ret;

    /* Start subsystems in dependency order */
    ret = aes67_ptp_start(handle->ptp);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "PTP start failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = aes67_rtp_engine_start(handle->rtp);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "RTP engine start failed: %s", esp_err_to_name(ret));
        aes67_ptp_stop(handle->ptp);
        return ret;
    }

    if (handle->config.sap_enabled && handle->sap) {
        ret = aes67_sap_start(handle->sap);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "SAP start failed: %s", esp_err_to_name(ret));
            aes67_rtp_engine_stop(handle->rtp);
            aes67_ptp_stop(handle->ptp);
            return ret;
        }
    }

    ret = aes67_audio_start(handle->audio);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "audio start failed: %s", esp_err_to_name(ret));
        if (handle->config.sap_enabled && handle->sap) {
            aes67_sap_stop(handle->sap);
        }
        aes67_rtp_engine_stop(handle->rtp);
        aes67_ptp_stop(handle->ptp);
        return ret;
    }

    if (aes67_clock_start() == ESP_OK) {
        ESP_LOGI(TAG, "audio clock steered to PTP");
    }

    /* mDNS advertisement (optional) */
    if (handle->config.mdns_enabled) {
        ret = aes67_mdns_init(handle->config.node_name, handle->config.net.rtp_port);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "mDNS init failed: %s (continuing without mDNS)",
                     esp_err_to_name(ret));
            /* Non-fatal: the node still works without mDNS */
        }
    }

    handle->running = true;

    /* Decoded sink audio: straight into the audio driver's playout buffer
     * in I2S mode (the DMA ISR reads it); otherwise the engine's own buffer,
     * which the playback task hands to the callback or the application. */
    aes67_rtp_engine_set_playout_buf(handle->rtp,
        handle->config.output_mode == AES67_OUTPUT_I2S
            ? aes67_audio_get_playout_buf(handle->audio) : NULL);

    /* Take PTP and RTP off the Ethernet input path before lwIP */
    if (handle->config.eth_rx_hook) {
        esp_netif_t *netif = esp_netif_get_handle_from_ifkey(handle->config.net.interface_name);
        ret = aes67_eth_rx_install(handle->config.net.eth_handle, netif, handle->rtp, handle->ptp);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Ethernet receive hook not installed: %s (socket path only)",
                     esp_err_to_name(ret));
        }
    }

    /* Playback task: only needed for callback/stream_buffer output modes.
     * For I2S mode, the DMA ISR reads directly from the stream buffer,
     * bypassing the playback task entirely (zero scheduling overhead). */
    if (handle->config.output_mode != AES67_OUTPUT_I2S) {
        xTaskCreatePinnedToCore(playback_task, "aes67_pb", 8192, handle,
                                21, &handle->pb_task, 0);
        aes67_rtp_engine_set_notify_task(handle->rtp, handle->pb_task);
        ESP_LOGI(TAG, "Playback task started (output_mode=%d)", handle->config.output_mode);
    } else {
        ESP_LOGI(TAG, "I2S output via DMA ISR (no playback task needed)");
    }

    /* Start the hardware PTP frame timer and audio frame task.
     * This task handles both TX and RX at PTP-aligned boundaries. */
    ret = aes67_hw_timer_start();
    if (ret == ESP_OK) {
        BaseType_t xret = xTaskCreatePinnedToCore(
            audio_frame_task, "aes67_tic", 8192, handle,
            21,     /* High priority, just below audio I/O (22) */
            &handle->tx_task,
            0       /* Pin to core 0 (PTP/network core) */
        );
        if (xret != pdPASS) {
            ESP_LOGW(TAG, "failed to create audio frame task");
            aes67_hw_timer_stop();
        } else {
            ESP_LOGI(TAG, "Audio frame processing driven by hardware PTP timer");
        }
    } else {
        ESP_LOGW(TAG, "hw timer start failed, using I2S DMA callback");
    }

    ESP_LOGI(TAG, "node started");

    return ESP_OK;
}

esp_err_t aes67_node_stop(aes67_node_handle_t handle)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!handle->running) {
        ESP_LOGW(TAG, "node not running");
        return ESP_OK;
    }

    /* Stop feeding the engine and daemon from the Ethernet path first */
    aes67_eth_rx_disable();

    /* Stop the frame timer and let the TIC and playback tasks leave their
     * loops before any subsystem they use is stopped. */
    aes67_hw_timer_stop();
    handle->running = false;
    if (handle->tx_task) {
        if (xSemaphoreTake(handle->tic_done, pdMS_TO_TICKS(500)) != pdTRUE) {
            ESP_LOGW(TAG, "audio frame task did not exit in time");
        }
        handle->tx_task = NULL;
    }
    if (handle->pb_task) {
        if (xSemaphoreTake(handle->pb_done, pdMS_TO_TICKS(1000)) != pdTRUE) {
            ESP_LOGW(TAG, "playback task did not exit in time");
        }
        handle->pb_task = NULL;
        aes67_rtp_engine_set_notify_task(handle->rtp, NULL);
    }

    /* Tear down in reverse order */
    if (handle->config.mdns_enabled) {
        aes67_mdns_deinit();
    }

    aes67_clock_stop();
    aes67_audio_stop(handle->audio);

    if (handle->config.sap_enabled && handle->sap) {
        aes67_sap_stop(handle->sap);
    }

    aes67_rtp_engine_stop(handle->rtp);
    aes67_ptp_stop(handle->ptp);

    ESP_LOGI(TAG, "node stopped");

    return ESP_OK;
}

esp_err_t aes67_node_destroy(aes67_node_handle_t handle)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Stop first if still running */
    if (handle->running) {
        aes67_node_stop(handle);
    }

    /* Destroy subsystems in reverse init order */
    aes67_session_destroy(handle->session);
    aes67_clock_deinit();
    aes67_audio_destroy(handle->audio);

    if (handle->config.sap_enabled && handle->sap) {
        aes67_sap_destroy(handle->sap);
    }

    aes67_rtp_engine_destroy(handle->rtp);
    aes67_ptp_destroy(handle->ptp);

    /* Frame timer (file static, single node) */
    aes67_hw_timer_deinit();
    vSemaphoreDelete(handle->tic_done);
    vSemaphoreDelete(handle->pb_done);
    s_node = NULL;

    ESP_LOGI(TAG, "node \"%s\" destroyed", handle->config.node_name);
    free(handle);

    return ESP_OK;
}

esp_err_t aes67_node_get_config(aes67_node_handle_t handle, aes67_config_t *config)
{
    if (!handle || !config) {
        return ESP_ERR_INVALID_ARG;
    }

    memcpy(config, &handle->config, sizeof(aes67_config_t));
    return ESP_OK;
}

esp_err_t aes67_node_get_session(aes67_node_handle_t handle,
                                  aes67_session_handle_t *session)
{
    if (!handle || !session) {
        return ESP_ERR_INVALID_ARG;
    }

    *session = handle->session;
    return ESP_OK;
}

esp_err_t aes67_node_get_audio(aes67_node_handle_t handle,
                               aes67_audio_handle_t *audio)
{
    if (!handle || !audio) {
        return ESP_ERR_INVALID_ARG;
    }
    *audio = handle->audio;
    return ESP_OK;
}

esp_err_t aes67_node_get_sap(aes67_node_handle_t handle,
                              aes67_sap_handle_t *sap)
{
    if (!handle || !sap) {
        return ESP_ERR_INVALID_ARG;
    }

    *sap = handle->sap;
    return ESP_OK;
}

esp_err_t aes67_node_get_ptp(aes67_node_handle_t handle,
                              aes67_ptp_handle_t *ptp)
{
    if (!handle || !ptp) {
        return ESP_ERR_INVALID_ARG;
    }

    *ptp = handle->ptp;
    return ESP_OK;
}

esp_err_t aes67_node_get_rtp(aes67_node_handle_t handle,
                              aes67_rtp_engine_handle_t *rtp)
{
    if (!handle || !rtp) {
        return ESP_ERR_INVALID_ARG;
    }

    *rtp = handle->rtp;
    return ESP_OK;
}

esp_err_t aes67_node_get_tic_stats(aes67_node_handle_t handle, aes67_node_tic_stats_t *stats)
{
    if (!handle || !stats) {
        return ESP_ERR_INVALID_ARG;
    }
    *stats = s_tic_stats;
    aes67_hw_timer_stats_t ts = {0};
    aes67_hw_timer_get_stats(&ts);
    stats->timer_fires = ts.fires;
    stats->timer_reanchors = ts.reanchors;
    stats->timer_rearms = ts.rearms;
    return ESP_OK;
}

esp_err_t aes67_node_get_clock_status(aes67_node_handle_t handle, aes67_clock_status_t *status)
{
    if (!handle || !status) {
        return ESP_ERR_INVALID_ARG;
    }
    return aes67_clock_get_status(status);
}

esp_err_t aes67_node_get_rx_stats(aes67_node_handle_t handle, aes67_node_rx_stats_t *stats)
{
    if (!handle || !stats) {
        return ESP_ERR_INVALID_ARG;
    }
    aes67_eth_rx_stats_t hook = {0};
    aes67_rtp_rx_stats_t rtp = {0};
    aes67_eth_rx_get_stats(&hook);
    aes67_rtp_engine_get_rx_stats(handle->rtp, &rtp);
    stats->frames_seen = hook.frames_seen;
    stats->frames_forwarded = hook.frames_forwarded;
    stats->ptp_consumed = hook.ptp_consumed;
    stats->rtp_consumed = rtp.rtp_consumed;
    stats->rtp_unmatched = rtp.rtp_unmatched;
    stats->rtp_malformed = rtp.rtp_malformed;
    stats->ssrc_rejected = rtp.ssrc_rejected;
    stats->playout_overflow = rtp.playout_overflow;
    return ESP_OK;
}
