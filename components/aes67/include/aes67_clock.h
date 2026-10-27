/*
 * SPDX-FileCopyrightText: 2026 Leon de Rooij (OSHLabs)
 *
 * SPDX-License-Identifier: Apache-2.0
 */
/*
 * Audio clock recovery.
 *
 * The I2S master clock comes from the APLL, a free-running oscillator with
 * its own crystal error; PTP time comes from the network. Left alone the
 * two drift apart by tens of ppm, so a receiver's playout buffer slowly
 * fills or drains and a sender's packets carry audio that is not quite
 * 48000 frames per second of PTP time. This loop measures how far the DAC
 * has run against PTP (the TX DMA ISR records frames played and PTP time
 * every descriptor) and steers the APLL's sigma-delta coefficients to keep
 * the phase error at zero: a PI controller, updated ten times a second,
 * with the correction clamped and slew-limited so a correction is never
 * audible. Capture shares the master clock and inherits the lock.
 *
 * Single instance; owned by the node.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "aes67_audio.h"
#include "aes67_ptp.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AES67_CLOCK_OFF = 0,        /* not steering (disabled, or audio not running) */
    AES67_CLOCK_FREE,           /* PTP not locked, APLL at its nominal setting */
    AES67_CLOCK_LOCKING,        /* steering, phase error still settling */
    AES67_CLOCK_LOCKED,         /* phase error and rate error within bounds */
    AES67_CLOCK_HOLDOVER,       /* PTP lock lost; last correction held */
} aes67_clock_state_t;

typedef struct {
    aes67_clock_state_t state;
    int32_t correction_ppb;     /* applied APLL correction (+ = audio clock sped up) */
    int32_t phase_error_ns;     /* DAC ahead (+) or behind (-) of PTP time */
    int32_t rate_error_ppb;     /* residual rate error, filtered */
    uint32_t updates;           /* controller iterations */
    uint32_t writes;            /* APLL coefficient writes */
    uint32_t reanchors;         /* phase reference reset (PTP step, audio restart) */
    uint32_t holdovers;         /* times PTP lock was lost while steering */
    uint32_t dropouts;          /* times the playout buffer ran dry while a stream was playing */
    uint32_t missed;            /* DMA descriptors played without a timely interrupt */
} aes67_clock_status_t;

/**
 * Read the APLL's nominal coefficients and register the PTP clock as the
 * audio driver's reference. Call after audio and PTP init, before start.
 */
esp_err_t aes67_clock_init(aes67_audio_handle_t audio, aes67_ptp_handle_t ptp, uint32_t sample_rate);

/** Start the control task (audio must be running). */
esp_err_t aes67_clock_start(void);

/** Stop the control task and return the APLL to its nominal coefficients. */
esp_err_t aes67_clock_stop(void);

void aes67_clock_deinit(void);

esp_err_t aes67_clock_get_status(aes67_clock_status_t *status);

#ifdef __cplusplus
}
#endif
