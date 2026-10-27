/*
 * SPDX-FileCopyrightText: 2026 DatanoiseTV
 *
 * SPDX-License-Identifier: Apache-2.0
 */
/*
 * Hardware PTP-synchronized audio frame timer.
 *
 * Uses the ESP32-P4 EMAC's target time interrupt to fire a callback
 * at precise PTP-clock-aligned intervals. The target time mechanism
 * programs an absolute PTP timestamp; when the EMAC PTP counter
 * reaches that value, a hardware ISR fires. We use this to create
 * a periodic frame timer synchronized to the PTP clock.
 *
 * This eliminates software timing jitter from vTaskDelay or software
 * timers, giving nanosecond-precision audio frame boundaries that
 * track the PTP master clock.
 */

#include "aes67_hw_timer.h"
#include "esp_eth_time.h"
#include "esp_log.h"
#include "esp_attr.h"

#include <string.h>
#include <time.h>
#include <sys/time.h>

static const char *TAG = "aes67_hw_timer";

/* A target written less than this far ahead of the clock may already be
 * in the past by the time the register takes it; the compare then never
 * matches and the timer is dead until someone re-arms it. */
#define HW_TIMER_GUARD_NS   100000

static struct {
    uint32_t frames_per_period;
    uint32_t sample_rate;
    uint32_t frame_period_ns;       /* nominal, rounded */
    aes67_hw_timer_cb_t cb;
    void *user_data;
    struct timespec next_time;
    bool running;
    volatile uint32_t fires;        /* ISR invocations */
    volatile uint32_t reanchors;    /* targets re-anchored because they were too close */
    volatile uint32_t rearms;       /* re-armed from task context after a stall */
} s_timer;

/* The first frame boundary strictly after `now_ns` (plus the guard), as a
 * timespec: the grid is in frames of PTP time, converted back to
 * nanoseconds exactly (truncated), so it stays aligned with the seconds. */
static inline void next_boundary_after(uint64_t now_ns, struct timespec *out)
{
    uint64_t frames = aes67_frames_from_ns(now_ns, s_timer.sample_rate);
    uint64_t next = (frames / s_timer.frames_per_period + 1) * s_timer.frames_per_period;
    out->tv_sec = (time_t)(next / s_timer.sample_rate);
    out->tv_nsec = (long)(((next % s_timer.sample_rate) * 1000000000ULL) / s_timer.sample_rate);
}

static inline uint64_t timespec_ns(const struct timespec *ts)
{
    return (uint64_t)ts->tv_sec * 1000000000ULL + (uint64_t)ts->tv_nsec;
}

/* ISR callback from EMAC when PTP clock reaches target time.
 * Runs in ISR context - must be fast and in IRAM. */
IRAM_ATTR static bool hw_timer_isr(esp_eth_mediator_t *eth, void *user_args)
{
    if (!s_timer.running) {
        return false;
    }
    s_timer.fires++;

    /* Fire the user callback */
    if (s_timer.cb) {
        s_timer.cb(s_timer.user_data);
    }

    /* Schedule the next target: the frame boundary after this one */
    next_boundary_after(timespec_ns(&s_timer.next_time), &s_timer.next_time);

    /* The next target must lie safely in the future when it is written.
     * If we are late (ISR latency, a PTP step) or merely too close, skip
     * to the first frame boundary at least a guard interval ahead rather
     * than loop to catch up. The task reconstructs skipped ticks from
     * PTP time. */
    struct timespec now;
    esp_eth_clock_gettime(CLOCK_PTP_SYSTEM, &now);

    int64_t diff_ns = (int64_t)(timespec_ns(&s_timer.next_time) - timespec_ns(&now));

    if (diff_ns < HW_TIMER_GUARD_NS) {
        next_boundary_after(timespec_ns(&now) + HW_TIMER_GUARD_NS, &s_timer.next_time);
        s_timer.reanchors++;
    }

    esp_eth_clock_set_target_time(CLOCK_PTP_SYSTEM, &s_timer.next_time);

    return false;
}

esp_err_t aes67_hw_timer_init(uint32_t frames_per_period, uint32_t sample_rate,
                               aes67_hw_timer_cb_t cb, void *user_data)
{
    if (!cb || frames_per_period == 0 || sample_rate == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(&s_timer, 0, sizeof(s_timer));
    s_timer.frames_per_period = frames_per_period;
    s_timer.sample_rate = sample_rate;
    s_timer.frame_period_ns = (uint32_t)(((uint64_t)frames_per_period * 1000000000ULL +
                                          sample_rate / 2) / sample_rate);
    s_timer.cb = cb;
    s_timer.user_data = user_data;
    s_timer.running = false;

    /* Register the ISR callback with the EMAC PTP target time mechanism */
    int ret = esp_eth_clock_register_target_cb(CLOCK_PTP_SYSTEM, hw_timer_isr);
    if (ret != 0) {
        ESP_LOGE(TAG, "failed to register target time callback: %d", ret);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "initialized (%lu frames at %lu Hz, %lu ns)",
             (unsigned long)frames_per_period, (unsigned long)sample_rate,
             (unsigned long)s_timer.frame_period_ns);
    return ESP_OK;
}

esp_err_t aes67_hw_timer_start(void)
{
    /* Get current PTP time and align to the next frame boundary */
    struct timespec now;
    if (esp_eth_clock_gettime(CLOCK_PTP_SYSTEM, &now) != 0) {
        ESP_LOGE(TAG, "failed to get PTP time");
        return ESP_FAIL;
    }

    /* Align to the next frame boundary at least a guard interval ahead */
    next_boundary_after(timespec_ns(&now) + HW_TIMER_GUARD_NS, &s_timer.next_time);
    s_timer.running = true;

    /* Program the first target time */
    esp_eth_clock_set_target_time(CLOCK_PTP_SYSTEM, &s_timer.next_time);

    ESP_LOGI(TAG, "started (first target: %lld.%09ld)",
             (long long)s_timer.next_time.tv_sec,
             s_timer.next_time.tv_nsec);
    return ESP_OK;
}

esp_err_t aes67_hw_timer_rearm(void)
{
    if (!s_timer.running) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Re-anchor to the next frame boundary from the current PTP time.
     * Used when the TIC stops receiving ticks, e.g. after a backward PTP
     * step left the programmed target far in the future. */
    struct timespec now;
    if (esp_eth_clock_gettime(CLOCK_PTP_SYSTEM, &now) != 0) {
        return ESP_FAIL;
    }
    next_boundary_after(timespec_ns(&now) + HW_TIMER_GUARD_NS, &s_timer.next_time);
    esp_eth_clock_set_target_time(CLOCK_PTP_SYSTEM, &s_timer.next_time);
    s_timer.rearms++;

    ESP_LOGW(TAG, "re-armed (target: %lld.%09ld)",
             (long long)s_timer.next_time.tv_sec, s_timer.next_time.tv_nsec);
    return ESP_OK;
}

esp_err_t aes67_hw_timer_stop(void)
{
    s_timer.running = false;
    ESP_LOGI(TAG, "stopped");
    return ESP_OK;
}

esp_err_t aes67_hw_timer_deinit(void)
{
    s_timer.running = false;
    /* Unhook the ISR from the EMAC target-time interrupt */
    esp_eth_clock_register_target_cb(CLOCK_PTP_SYSTEM, NULL);
    memset(&s_timer, 0, sizeof(s_timer));
    return ESP_OK;
}

void aes67_hw_timer_get_stats(aes67_hw_timer_stats_t *stats)
{
    if (!stats) return;
    stats->fires = s_timer.fires;
    stats->reanchors = s_timer.reanchors;
    stats->rearms = s_timer.rearms;
}

uint32_t aes67_hw_timer_period_ns(void)
{
    return s_timer.frame_period_ns;
}

uint32_t aes67_hw_timer_frames_per_period(void)
{
    return s_timer.frames_per_period;
}
