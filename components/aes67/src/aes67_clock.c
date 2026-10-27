/*
 * SPDX-FileCopyrightText: 2026 Leon de Rooij (OSHLabs)
 *
 * SPDX-License-Identifier: Apache-2.0
 */
/*
 * Audio clock recovery: steer the APLL so the DAC's frame count tracks
 * PTP time. See aes67_clock.h for the picture.
 *
 * Measurement. The TX DMA ISR stores (frames played, PTP time) once per
 * descriptor; the controller reads the latest pair ten times a second.
 * With an anchor (f0, t0) taken at lock, the phase error in frames is
 *
 *     e = (frames - f0) - (t - t0) * fs / 1e9
 *
 * positive when the audio clock runs fast.
 *
 * Control. A PI loop produces a correction u in ppm (positive = speed the
 * audio clock up): u = -(Kp e + Ki integral(e)). With 1 ppm moving the
 * phase by 0.048 frames/s, Kp = 1.25 ppm/frame and Ki = 0.019 ppm/(frame s)
 * give a critically damped loop with a 33 s time constant. The correction
 * is clamped (CONFIG_AES67_CLOCK_MAX_CORRECTION_PPM) and slew-limited to
 * 2 ppm per update, so even the initial pull-in is inaudible. While PTP is
 * not locked the last correction is held.
 *
 * Actuation. The APLL output is xtal * (4 + sdm/65536) / ((o_div + 2) * 2)
 * with sdm the 24-bit sigma-delta word. Scaling the multiplier by
 * (1 + u/1e6) is a shift of the sdm word by (262144 + sdm) * u / 1e6, about
 * 0.6 steps per ppm at the 36.864 MHz the I2S driver sets up. The word is
 * written straight into the modulator (hal clk_ll_apll_set_config); the
 * driver's own setter additionally runs a PLL calibration cycle on every
 * write, which is neither needed for a sub-percent retune nor glitch free.
 */

#include "aes67_clock.h"

#include <math.h>
#include <string.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "hal/clk_tree_ll.h"
#include "esp_private/regi2c_ctrl.h"
#include "esp_eth_time.h"

static const char *TAG = "aes67_clock";

#define CLOCK_PERIOD_MS         100
#define CLOCK_KP                1.25        /* ppm per frame of phase error */
#define CLOCK_KI                0.019       /* ppm per frame-second */
#define CLOCK_SLEW_PPM          2.0         /* max change per update */
#define CLOCK_RATE_WINDOW       100         /* updates (10 s) the rate error is measured over */
#define CLOCK_LOCK_PHASE_MS     1           /* |phase error| for "locked" ... */
#define CLOCK_LOCK_RATE_PPM     2.0         /* ... and |rate error|; "locking" again at twice either */
#define CLOCK_REANCHOR_MS       100         /* a larger phase error is a step, not drift */
#define CLOCK_XTAL_HZ           40000000.0

#ifdef CONFIG_AES67_CLOCK_MAX_CORRECTION_PPM
#define CLOCK_MAX_PPM           ((double)CONFIG_AES67_CLOCK_MAX_CORRECTION_PPM)
#else
#define CLOCK_MAX_PPM           200.0
#endif

static struct {
    aes67_audio_handle_t    audio;
    aes67_ptp_handle_t      ptp;
    uint32_t                sample_rate;
    bool                    initialised;
    volatile bool           running;
    TaskHandle_t            task;
    SemaphoreHandle_t       done;
    portMUX_TYPE            lock;

    /* APLL nominal setting as the I2S driver programmed it */
    uint32_t                o_div;
    uint32_t                sdm_base;       /* 24-bit word */
    uint32_t                sdm_applied;
    int64_t                 write_us_max;   /* longest coefficient write */

    /* Controller state (task only) */
    bool                    anchored;
    uint64_t                f0, t0;
    double                  e_int;          /* integral of phase error, frame-seconds */
    double                  u_ppm;          /* applied correction */
    double                  e_hist[CLOCK_RATE_WINDOW];
    uint32_t                e_hist_n;       /* samples stored (saturates at window) */
    uint32_t                e_hist_i;
    uint64_t                last_snap_frames;

    aes67_clock_status_t    status;         /* under lock */
} s_clk;

/* The reference the DMA ISR stamps with: PTP time in nanoseconds */
static uint64_t IRAM_ATTR clock_now_ns(void)
{
    struct timespec ts;
    if (esp_eth_clock_gettime(CLOCK_PTP_SYSTEM, &ts) != 0) {
        return 0;
    }
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static void apll_read(uint32_t *o_div, uint32_t *sdm)
{
    uint32_t sdm0, sdm1, sdm2;
    ANALOG_CLOCK_ENABLE();
    clk_ll_apll_get_config(o_div, &sdm0, &sdm1, &sdm2);
    ANALOG_CLOCK_DISABLE();
    *sdm = (sdm2 << 16) | (sdm1 << 8) | sdm0;
}

static void apll_write(uint32_t o_div, uint32_t sdm)
{
    int64_t t0 = esp_timer_get_time();
    ANALOG_CLOCK_ENABLE();
    clk_ll_apll_set_config(o_div, sdm & 0xFF, (sdm >> 8) & 0xFF, (sdm >> 16) & 0x3F);
    ANALOG_CLOCK_DISABLE();
    int64_t us = esp_timer_get_time() - t0;
    if (us > s_clk.write_us_max) {
        s_clk.write_us_max = us;
    }
}

static double apll_hz(uint32_t o_div, uint32_t sdm)
{
    return CLOCK_XTAL_HZ * (4.0 + sdm / 65536.0) / ((o_div + 2) * 2.0);
}

/* Apply a correction in ppm; returns true when the modulator word changed.
 * The word moves only once the exact value is three quarters of a step
 * away from what is applied, so noise around a boundary does not dither. */
static bool apply_ppm(double ppm)
{
    double exact = s_clk.sdm_base + (262144.0 + s_clk.sdm_base) * ppm / 1e6;
    if (fabs(exact - (double)s_clk.sdm_applied) < 0.75) {
        return false;
    }
    long word = lround(exact);
    if (word < 0) {
        word = 0;
    } else if (word > 0x3FFFFF) {
        word = 0x3FFFFF;            /* sdm2 is six bits */
    }
    if ((uint32_t)word == s_clk.sdm_applied) {
        return false;
    }
    apll_write(s_clk.o_div, (uint32_t)word);
    s_clk.sdm_applied = (uint32_t)word;
    return true;
}

static void set_status(aes67_clock_state_t state, double e_frames, double rate_ppm,
                       const aes67_audio_dma_stats_t *dma)
{
    portENTER_CRITICAL(&s_clk.lock);
    s_clk.status.state = state;
    s_clk.status.correction_ppb = (int32_t)lround(s_clk.u_ppm * 1000.0);
    s_clk.status.phase_error_ns = (int32_t)lround(e_frames * 1e9 / s_clk.sample_rate);
    s_clk.status.rate_error_ppb = (int32_t)lround(rate_ppm * 1000.0);
    s_clk.status.dropouts = dma->dropouts;
    s_clk.status.missed = dma->isr_missed;
    portEXIT_CRITICAL(&s_clk.lock);
}

static const char *state_str(aes67_clock_state_t st)
{
    switch (st) {
    case AES67_CLOCK_OFF:       return "off";
    case AES67_CLOCK_FREE:      return "free";
    case AES67_CLOCK_LOCKING:   return "locking";
    case AES67_CLOCK_LOCKED:    return "locked";
    case AES67_CLOCK_HOLDOVER:  return "holdover";
    }
    return "?";
}

static void clock_task(void *arg)
{
    const double fs = s_clk.sample_rate;
    const double dt = CLOCK_PERIOD_MS / 1000.0;
    const double reanchor_frames = fs * CLOCK_REANCHOR_MS / 1000.0;
    const double lock_frames = fs * CLOCK_LOCK_PHASE_MS / 1000.0;
    aes67_clock_state_t state = AES67_CLOCK_FREE, logged_state = AES67_CLOCK_OFF;
    double e = 0.0, rate_ppm = 0.0;
    uint32_t iter = 0;

    s_clk.anchored = false;
    s_clk.e_hist_n = 0;
    s_clk.e_hist_i = 0;

    while (s_clk.running) {
        vTaskDelay(pdMS_TO_TICKS(CLOCK_PERIOD_MS));
        iter++;

        aes67_ptp_status_t ps = {0};
        aes67_ptp_get_status(s_clk.ptp, &ps);
        bool ptp_locked = ps.lock_state == AES67_PTP_LOCKED;

        aes67_audio_dma_stats_t dma = {0};
        aes67_audio_get_dma_stats(s_clk.audio, &dma);

        uint64_t frames, t;
        bool have_snap = aes67_audio_get_playout_snapshot(s_clk.audio, &frames, &t) == ESP_OK
                         && t != 0 && frames != s_clk.last_snap_frames;
        s_clk.last_snap_frames = frames;

        if (!ptp_locked || !have_snap) {
            /* Nothing to measure against: hold what we have */
            if (s_clk.anchored) {
                s_clk.anchored = false;
                s_clk.status.holdovers++;
                ESP_LOGW(TAG, "PTP lock lost, holding %.2f ppm", s_clk.u_ppm);
            }
            state = (s_clk.sdm_applied != s_clk.sdm_base) ? AES67_CLOCK_HOLDOVER : AES67_CLOCK_FREE;
            set_status(state, 0.0, 0.0, &dma);
            s_clk.e_hist_n = 0;
        } else {
            if (!s_clk.anchored) {
                s_clk.f0 = frames;
                s_clk.t0 = t;
                s_clk.anchored = true;
                s_clk.e_hist_n = 0;
                e = 0.0;
                ESP_LOGI(TAG, "anchored to PTP (correction %.2f ppm)", s_clk.u_ppm);
            } else {
                double expected = (double)(int64_t)(t - s_clk.t0) * fs / 1e9;
                e = (double)(int64_t)(frames - s_clk.f0) - expected;

                if (fabs(e) > reanchor_frames) {
                    /* A step in PTP time or an audio restart, not drift */
                    s_clk.f0 = frames;
                    s_clk.t0 = t;
                    s_clk.e_hist_n = 0;
                    s_clk.status.reanchors++;
                    ESP_LOGW(TAG, "phase step of %.1f ms, re-anchored",
                             e * 1000.0 / fs);
                    e = 0.0;
                } else {
                    /* PI, with the integrator frozen while clamped */
                    double u = -(CLOCK_KP * e + CLOCK_KI * s_clk.e_int);
                    bool clamped = (u > CLOCK_MAX_PPM && e < 0) || (u < -CLOCK_MAX_PPM && e > 0);
                    if (!clamped) {
                        s_clk.e_int += e * dt;
                    }
                    if (u > CLOCK_MAX_PPM) {
                        u = CLOCK_MAX_PPM;
                    } else if (u < -CLOCK_MAX_PPM) {
                        u = -CLOCK_MAX_PPM;
                    }
                    double step = u - s_clk.u_ppm;
                    if (step > CLOCK_SLEW_PPM) {
                        step = CLOCK_SLEW_PPM;
                    } else if (step < -CLOCK_SLEW_PPM) {
                        step = -CLOCK_SLEW_PPM;
                    }
                    s_clk.u_ppm += step;
                    if (apply_ppm(s_clk.u_ppm)) {
                        s_clk.status.writes++;
                    }
                }
            }

            /* Residual rate error over the last window */
            bool have_rate = s_clk.e_hist_n >= CLOCK_RATE_WINDOW;
            if (have_rate) {
                double e_old = s_clk.e_hist[s_clk.e_hist_i];
                rate_ppm = (e - e_old) / (CLOCK_RATE_WINDOW * dt) / fs * 1e6;
            } else {
                rate_ppm = 0.0;
            }
            s_clk.e_hist[s_clk.e_hist_i] = e;
            s_clk.e_hist_i = (s_clk.e_hist_i + 1) % CLOCK_RATE_WINDOW;
            if (s_clk.e_hist_n < CLOCK_RATE_WINDOW) {
                s_clk.e_hist_n++;
            }

            /* Lock with hysteresis so noise at the threshold does not flap */
            if (state == AES67_CLOCK_LOCKED) {
                if (!have_rate || fabs(e) > 2 * lock_frames ||
                    fabs(rate_ppm) > 2 * CLOCK_LOCK_RATE_PPM) {
                    state = AES67_CLOCK_LOCKING;
                }
            } else if (have_rate && fabs(e) < lock_frames &&
                       fabs(rate_ppm) < CLOCK_LOCK_RATE_PPM) {
                state = AES67_CLOCK_LOCKED;
            } else {
                state = AES67_CLOCK_LOCKING;
            }
            set_status(state, e, rate_ppm, &dma);
        }
        s_clk.status.updates = iter;

        if (state != logged_state) {
            ESP_LOGI(TAG, "%s (correction %.2f ppm, phase %.1f us)",
                     state_str(state), s_clk.u_ppm, e * 1e6 / fs);
            logged_state = state;
        }
        if ((iter % (60000 / CLOCK_PERIOD_MS)) == 0) {
            ESP_LOGI(TAG, "%s: correction %.2f ppm, phase %.1f us, rate %.2f ppm, "
                     "%lu writes (max %lld us), %lu re-anchors, %lu missed, %lu dropouts",
                     state_str(state), s_clk.u_ppm, e * 1e6 / fs, rate_ppm,
                     (unsigned long)s_clk.status.writes, (long long)s_clk.write_us_max,
                     (unsigned long)s_clk.status.reanchors,
                     (unsigned long)dma.isr_missed, (unsigned long)dma.dropouts);
        }
    }

    aes67_audio_dma_stats_t dma_end = {0};
    set_status(AES67_CLOCK_OFF, 0.0, 0.0, &dma_end);
    xSemaphoreGive(s_clk.done);
    vTaskDelete(NULL);
}

esp_err_t aes67_clock_init(aes67_audio_handle_t audio, aes67_ptp_handle_t ptp, uint32_t sample_rate)
{
    if (!audio || !ptp || sample_rate == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_clk.initialised) {
        return ESP_ERR_INVALID_STATE;
    }

    uint32_t o_div, sdm;
    apll_read(&o_div, &sdm);
    if (sdm == 0) {
        ESP_LOGE(TAG, "APLL not configured; is the I2S clock source APLL?");
        return ESP_ERR_INVALID_STATE;
    }

    memset(&s_clk, 0, sizeof(s_clk));
    portMUX_INITIALIZE(&s_clk.lock);
    s_clk.audio = audio;
    s_clk.ptp = ptp;
    s_clk.sample_rate = sample_rate;
    s_clk.o_div = o_div;
    s_clk.sdm_base = sdm;
    s_clk.sdm_applied = sdm;
    s_clk.done = xSemaphoreCreateBinary();
    if (!s_clk.done) {
        return ESP_ERR_NO_MEM;
    }
    s_clk.initialised = true;
    s_clk.status.state = AES67_CLOCK_OFF;

    aes67_audio_set_clock_source(audio, clock_now_ns);

    ESP_LOGI(TAG, "APLL %.3f MHz (o_div %lu, sdm 0x%06lx), %.3f ppm per step, range +/-%.0f ppm",
             apll_hz(o_div, sdm) / 1e6, (unsigned long)o_div, (unsigned long)sdm,
             1e6 / (262144.0 + sdm), CLOCK_MAX_PPM);
    return ESP_OK;
}

esp_err_t aes67_clock_start(void)
{
    if (!s_clk.initialised) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_clk.running) {
        return ESP_OK;
    }
    s_clk.running = true;
    /* On the other core than the I2S DMA interrupt (allocated on the core
     * that created the channels, core 0): the coefficient writes run in a
     * critical section and must not hold that interrupt off. */
    if (xTaskCreatePinnedToCore(clock_task, "aes67_clk", 4096, NULL, 5, &s_clk.task,
                                CONFIG_FREERTOS_NUMBER_OF_CORES > 1 ? 1 : 0) != pdPASS) {
        s_clk.running = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t aes67_clock_stop(void)
{
    if (!s_clk.initialised || !s_clk.running) {
        return ESP_OK;
    }
    s_clk.running = false;
    if (xSemaphoreTake(s_clk.done, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGW(TAG, "clock task did not exit in time");
    }
    s_clk.task = NULL;
    /* Back to the nominal setting; the next start pulls in again */
    if (s_clk.sdm_applied != s_clk.sdm_base) {
        apll_write(s_clk.o_div, s_clk.sdm_base);
        s_clk.sdm_applied = s_clk.sdm_base;
    }
    s_clk.u_ppm = 0.0;
    s_clk.e_int = 0.0;
    return ESP_OK;
}

void aes67_clock_deinit(void)
{
    if (!s_clk.initialised) {
        return;
    }
    aes67_clock_stop();
    aes67_audio_set_clock_source(s_clk.audio, NULL);
    vSemaphoreDelete(s_clk.done);
    memset(&s_clk, 0, sizeof(s_clk));
}

esp_err_t aes67_clock_get_status(aes67_clock_status_t *status)
{
    if (!status) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_clk.initialised) {
        memset(status, 0, sizeof(*status));
        status->state = AES67_CLOCK_OFF;
        return ESP_OK;
    }
    portENTER_CRITICAL(&s_clk.lock);
    *status = s_clk.status;
    portEXIT_CRITICAL(&s_clk.lock);
    return ESP_OK;
}
