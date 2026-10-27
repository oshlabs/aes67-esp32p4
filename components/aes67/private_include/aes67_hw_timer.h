/*
 * SPDX-FileCopyrightText: 2026 DatanoiseTV
 *
 * SPDX-License-Identifier: Apache-2.0
 */
/*
 * Hardware PTP-synchronized audio frame timer.
 *
 * Uses the EMAC target time interrupt to fire a callback at precise
 * PTP-synchronized intervals, replacing software-based timing with
 * hardware-precision audio frame boundaries.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Callback fired at each audio frame boundary from ISR context.
 * Must be IRAM_ATTR and as brief as possible - typically just
 * gives a semaphore or sets a flag. */
typedef void (*aes67_hw_timer_cb_t)(void *user_data);

/**
 * Initialize the hardware PTP frame timer.
 *
 * The timer runs on the media clock's frame grid: boundaries are at
 * multiples of `frames_per_period` frames of PTP time (frame k is at
 * k / sample_rate seconds since the epoch). For 16 frames at 48 kHz the
 * targets alternate 333333 and 333334 ns, and never drift from the
 * second boundaries as a rounded nanosecond period would.
 *
 * @param frames_per_period  Frames per tick (the packet's frame count)
 * @param sample_rate        Media clock rate in Hz
 * @param cb                 Callback to fire at each frame boundary
 * @param user_data          User data passed to callback
 */
esp_err_t aes67_hw_timer_init(uint32_t frames_per_period, uint32_t sample_rate,
                               aes67_hw_timer_cb_t cb, void *user_data);

/** PTP time in media clock frames (frame k is at k / rate seconds) */
static inline uint64_t aes67_frames_from_ns(uint64_t ns, uint32_t sample_rate)
{
    return (ns / 1000000000ULL) * sample_rate +
           ((ns % 1000000000ULL) * sample_rate) / 1000000000ULL;
}

/**
 * Start the hardware frame timer. Programs the first target time
 * aligned to the next PTP-time frame boundary.
 */
esp_err_t aes67_hw_timer_start(void);

/**
 * Re-anchor the timer to the next frame boundary from the current PTP
 * time. Call when ticks stop arriving (e.g. after a backward PTP step).
 */
esp_err_t aes67_hw_timer_rearm(void);

/**
 * Stop the hardware frame timer.
 */
esp_err_t aes67_hw_timer_stop(void);

/**
 * Unregister the ISR and forget the configuration (inverse of init).
 */
esp_err_t aes67_hw_timer_deinit(void);

typedef struct {
    uint32_t fires;         /* ISR invocations since init */
    uint32_t reanchors;     /* next target skipped ahead because it was too close */
    uint32_t rearms;        /* re-armed from task context after a stall */
} aes67_hw_timer_stats_t;

void aes67_hw_timer_get_stats(aes67_hw_timer_stats_t *stats);
/** Nominal period in nanoseconds (rounded; for wait bounds, not for the grid) */
uint32_t aes67_hw_timer_period_ns(void);
uint32_t aes67_hw_timer_frames_per_period(void);

#ifdef __cplusplus
}
#endif
