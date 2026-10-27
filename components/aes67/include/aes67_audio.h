/*
 * SPDX-FileCopyrightText: 2026 DatanoiseTV
 *
 * SPDX-License-Identifier: Apache-2.0
 */
/*
 * Audio I/O driver using I2S TDM on ESP32-P4.
 *
 * Manages I2S DMA transfers for audio capture and playback. Provides
 * the audio frame callback (TIC) that drives the RTP engine's timing.
 * DMA buffers are in internal SRAM; larger ring buffers use PSRAM.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "aes67_config.h"

#ifdef __cplusplus
extern "C" {
#endif


/* Opaque audio driver handle */
typedef struct aes67_audio_ctx *aes67_audio_handle_t;

/**
 * Initialize the I2S audio driver.
 *
 * @param audio_config  Audio parameters (sample rate, channels, format)
 * @param pins          I2S pin configuration
 * @param use_psram     If true, allocate ring buffers in PSRAM
 * @param ring_buf_ms   Ring buffer size in milliseconds
 * @param handle        Output handle
 */
esp_err_t aes67_audio_init(const aes67_audio_config_t *audio_config,
                           const aes67_i2s_pins_t *pins,
                           bool use_psram,
                           uint16_t ring_buf_ms,
                           aes67_audio_handle_t *handle);

/**
 * Start audio I/O (enables I2S DMA).
 */
esp_err_t aes67_audio_start(aes67_audio_handle_t handle);

/**
 * Stop audio I/O.
 */
esp_err_t aes67_audio_stop(aes67_audio_handle_t handle);

/**
 * Destroy audio driver and free all buffers.
 */
esp_err_t aes67_audio_destroy(aes67_audio_handle_t handle);

/**
 * Register the audio frame callback. The callback is invoked from a
 * high-priority task each time a DMA transfer completes (one frame period).
 */

/**
 * Write samples to the playback ring buffer (sink path).
 * Called from the RTP jitter buffer read path.
 *
 * @param handle        Audio driver handle
 * @param samples       Interleaved audio samples
 * @param frame_count   Number of frames to write
 * @return ESP_OK, or ESP_ERR_NO_MEM if buffer full
 */
esp_err_t aes67_audio_write_playback(aes67_audio_handle_t handle,
                                     const void *samples, uint32_t frame_count);

/**
 * Read samples from the capture ring buffer (source path).
 * Called by the RTP engine to get audio data for transmission.
 *
 * @param handle        Audio driver handle
 * @param samples       Output buffer for interleaved samples
 * @param frame_count   Number of frames to read
 * @return ESP_OK, or ESP_ERR_NOT_FOUND if insufficient data
 */
esp_err_t aes67_audio_read_capture(aes67_audio_handle_t handle,
                                   void *samples, uint32_t frame_count);

/**
 * Get current audio parameters.
 */
esp_err_t aes67_audio_get_config(aes67_audio_handle_t handle,
                                 aes67_audio_config_t *config);

/**
 * Get buffer fill levels (for monitoring).
 *
 * @param handle            Audio driver handle
 * @param capture_frames    Frames available in capture ring buffer
 * @param playback_frames   Frames available in playback ring buffer
 */
esp_err_t aes67_audio_get_buffer_levels(aes67_audio_handle_t handle,
                                        uint32_t *capture_frames,
                                        uint32_t *playback_frames);

/**
 * Write int32 samples directly to I2S via i2s_channel_write.
 * Blocks until DMA has room (used for DMA-paced playback).
 */
esp_err_t aes67_audio_direct_write(aes67_audio_handle_t handle,
                                   const int32_t *samples, uint32_t frame_count);

/**
 * Get the I2S TX channel handle (for DMA-paced playback).
 */
#include "driver/i2s_types.h"
i2s_chan_handle_t aes67_audio_get_tx_chan(aes67_audio_handle_t handle);

/**
 * Write int16 stereo samples to the staging ring buffer.
 * Called by the playback task; the DMA ISR reads from this ring.
 */
esp_err_t aes67_audio_staging_write(aes67_audio_handle_t handle,
                                     const int16_t *samples, uint32_t sample_count);

/**
 * Get DMA staging statistics.
 */
void aes67_audio_get_staging_stats(aes67_audio_handle_t handle,
                                    uint32_t *isr_count, uint32_t *underruns);

/**
 * The playout stream buffer the DMA ISR plays from: two-slot interleaved
 * int32 frames. Created by aes67_audio_init(); the node points the RTP
 * engine's decoded output at it in I2S mode.
 */
StreamBufferHandle_t aes67_audio_get_playout_buf(aes67_audio_handle_t handle);

/**
 * Log ISR statistics.
 */
void aes67_audio_log_isr_stats(void);


/**
 * Get the capture stream buffer for reading captured I2S audio.
 */
StreamBufferHandle_t aes67_audio_get_capture_buf(aes67_audio_handle_t handle);

/**
 * Give `task` one task notification when the next capture DMA chunk has
 * been written to the capture buffer (one-shot; NULL cancels). Lets the
 * frame task wait for the last chunk of a packet instead of a whole period.
 */
void aes67_audio_capture_notify_once(aes67_audio_handle_t handle, TaskHandle_t task);

/**
 * Reference clock the TX DMA ISR stamps its playout snapshots with
 * (nanoseconds; must be callable from an ISR). Set by the clock-recovery
 * loop before the audio is started; NULL disables the snapshots.
 */
typedef uint64_t (*aes67_audio_clock_fn_t)(void);
void aes67_audio_set_clock_source(aes67_audio_handle_t handle, aes67_audio_clock_fn_t now_ns);

/**
 * Latest playout snapshot: frames the DAC has clocked out since init and
 * the reference time when the TX DMA ISR recorded that count.
 * ESP_ERR_NOT_FOUND until the first DMA descriptor has completed.
 */
esp_err_t aes67_audio_get_playout_snapshot(aes67_audio_handle_t handle,
                                           uint64_t *frames_out, uint64_t *time_ns);

/** TX DMA ISR totals since init (never reset) */
typedef struct {
    uint32_t isr_fires;             /* descriptors played */
    uint32_t isr_missed;            /* descriptors played without a timely interrupt (stale data) */
    uint32_t dropouts;              /* times the playout buffer ran dry while a stream was playing */
} aes67_audio_dma_stats_t;
esp_err_t aes67_audio_get_dma_stats(aes67_audio_handle_t handle, aes67_audio_dma_stats_t *stats);

#ifdef __cplusplus
}
#endif
