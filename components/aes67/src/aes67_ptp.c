/*
 * SPDX-FileCopyrightText: 2026 DatanoiseTV
 *
 * SPDX-License-Identifier: Apache-2.0
 */
/*
 * PTP (IEEE 1588) clock synchronization implementation.
 *
 * Wraps the ptpd daemon and ESP32-P4 hardware timestamping to provide
 * a synchronized PTP clock for AES67 RTP timestamp generation.
 * Monitors lock state and notifies upper layers on transitions.
 */

#include "aes67_ptp.h"

#include <string.h>
#include <stdlib.h>
#include <time.h>

#include "esp_log.h"
#include "esp_err.h"
#include "esp_check.h"
#include "esp_eth.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "aes67_ptp";

/* Lock state machine: driven by a low-pass filtered offset with hysteresis.
 *   unlocked -> locking   when |filtered| < 10 ms
 *   locking  -> locked    when |filtered| < threshold for STABLE_COUNT polls
 *   locked   -> locking   when |filtered| > 5 x threshold
 *   any      -> unlocked  when |filtered| > 10 ms
 * The threshold is a Kconfig value because achievable accuracy depends on
 * the transport: a few microseconds with hardware timestamps, tens with
 * software timestamps. */
#define PTP_LOCKING_THRESHOLD_NS    (10 * 1000 * 1000)   /* 10 ms  */
#define PTP_LOCKED_THRESHOLD_NS     CONFIG_AES67_PTP_LOCK_THRESHOLD_NS
#define PTP_UNLOCK_HYSTERESIS       5
#define PTP_LOCKED_STABLE_COUNT     4
#define PTP_FILTER_SHIFT            2                     /* EMA alpha = 1/4 */
/* Do not claim to be grandmaster until this long has passed without an
 * Announce from anyone; three announce intervals. */
#define PTP_GM_GRACE_MS             (3 * CONFIG_NETUTILS_PTPD_ANNOUNCE_INTERVAL_MSEC)
#define PTP_MONITOR_INTERVAL_MS     500
#define PTP_MONITOR_STACK_SIZE      4096
#define PTP_MONITOR_PRIORITY        10  /* Above SAP, below RTP/audio */
#define PTP_STATUS_FAILS_BEFORE_UNLOCK 4   /* consecutive ptpd_status failures */

/* PTP daemon and clock APIs from local components */
#include "ptpd.h"
#include "esp_eth_time.h"
#include "esp_vfs_l2tap.h"

/* -------------------------------------------------------------------
 * Internal context
 * ------------------------------------------------------------------- */
struct aes67_ptp_ctx {
    esp_eth_handle_t eth_handle;
    char netif_key[16];             /* esp_netif interface key the daemon binds to */
    int status_failures;            /* consecutive ptpd_status failures */
    int ptpd_pid;
    aes67_ptp_config_t config;
    aes67_ptp_lock_state_t lock_state;
    aes67_ptp_lock_cb_t lock_cb;
    void *lock_cb_user_data;
    TaskHandle_t monitor_task;
    SemaphoreHandle_t monitor_done;   /* given by the monitor task on exit */
    bool running;

    /* Grandmaster identity from last status poll */
    uint8_t grandmaster_id[8];

    /* Own MAC address, used for generating grandmaster identity when acting as GM */
    uint8_t own_mac[6];
    bool is_grandmaster;            /* True when this node is acting as PTP GM */

    /* Rolling count of consecutive samples under the locked threshold */
    int stable_count;

    /* Cached status values */
    int32_t offset_ns;
    int32_t path_delay_ns;
    int32_t drift_ppb;
    int32_t filtered_offset_ns;
    int32_t jitter_ns;
    bool filter_primed;
    aes67_ptp_transport_t transport;
    uint8_t master_clock_class;
    uint8_t master_accuracy;
    TickType_t start_tick;  /* when the daemon was started, for the GM grace period */

    /* Stats logging counter (prints every N polls) */
    int stats_counter;
};

/* -------------------------------------------------------------------
 * Lock state machine
 * ------------------------------------------------------------------- */

/*
 * Evaluate offset and advance the lock state machine.
 * Returns true if the state changed.
 */
static bool ptp_update_lock_state(struct aes67_ptp_ctx *ctx, int64_t offset_ns)
{
    /* Low-pass the raw offset and track how much it moves between polls */
    if (!ctx->filter_primed) {
        ctx->filtered_offset_ns = (int32_t)offset_ns;
        ctx->jitter_ns = 0;
        ctx->filter_primed = true;
    } else {
        int32_t dev = (int32_t)offset_ns - ctx->filtered_offset_ns;
        ctx->filtered_offset_ns += dev >> PTP_FILTER_SHIFT;
        int32_t adev = dev < 0 ? -dev : dev;
        ctx->jitter_ns += (adev - ctx->jitter_ns) >> 3;
    }
    int64_t abs_offset = ctx->filtered_offset_ns < 0 ? -ctx->filtered_offset_ns
                                                     : ctx->filtered_offset_ns;
    aes67_ptp_lock_state_t prev = ctx->lock_state;

    switch (ctx->lock_state) {
    case AES67_PTP_UNLOCKED:
        if (abs_offset < PTP_LOCKING_THRESHOLD_NS) {
            ctx->lock_state = AES67_PTP_LOCKING;
            ctx->stable_count = 0;
            ESP_LOGI(TAG, "PTP locking (offset %lld ns)", (long long)offset_ns);
        }
        break;

    case AES67_PTP_LOCKING:
        if (abs_offset >= PTP_LOCKING_THRESHOLD_NS) {
            /* Lost convergence, fall back */
            ctx->lock_state = AES67_PTP_UNLOCKED;
            ctx->stable_count = 0;
            ESP_LOGW(TAG, "PTP lost lock (offset %lld ns)", (long long)offset_ns);
        } else if (abs_offset < PTP_LOCKED_THRESHOLD_NS) {
            ctx->stable_count++;
            if (ctx->stable_count >= PTP_LOCKED_STABLE_COUNT) {
                ctx->lock_state = AES67_PTP_LOCKED;
                ESP_LOGI(TAG, "PTP locked (offset %lld ns after %d stable polls)",
                         (long long)offset_ns, ctx->stable_count);
            }
        } else {
            /* Still converging but not yet sub-microsecond */
            ctx->stable_count = 0;
        }
        break;

    case AES67_PTP_LOCKED:
        if (abs_offset >= PTP_LOCKING_THRESHOLD_NS) {
            ctx->lock_state = AES67_PTP_UNLOCKED;
            ctx->stable_count = 0;
            ESP_LOGW(TAG, "PTP lost lock completely (offset %lld ns)", (long long)offset_ns);
        } else if (abs_offset >= (int64_t)PTP_LOCKED_THRESHOLD_NS * PTP_UNLOCK_HYSTERESIS) {
            ctx->lock_state = AES67_PTP_LOCKING;
            ctx->stable_count = 0;
            ESP_LOGW(TAG, "PTP drifting (offset %lld ns, re-locking)", (long long)offset_ns);
        }
        break;
    }

    return (ctx->lock_state != prev);
}

/* -------------------------------------------------------------------
 * Monitor task
 * ------------------------------------------------------------------- */

static void ptp_monitor_task(void *arg)
{
    struct aes67_ptp_ctx *ctx = (struct aes67_ptp_ctx *)arg;
    struct ptpd_status_s status;

    ESP_LOGI(TAG, "PTP monitor task started");

    while (ctx->running) {
        vTaskDelay(pdMS_TO_TICKS(PTP_MONITOR_INTERVAL_MS));

        if (!ctx->running) {
            break;
        }

        memset(&status, 0, sizeof(status));
        if (ctx->ptpd_pid < 0) {
            continue;
        }
        int ret = ptpd_status(ctx->ptpd_pid, &status);
        if (ret != 0) {
            /* Daemon gone or wedged: after a few misses report UNLOCKED
             * once and keep polling quietly. */
            if (++ctx->status_failures == PTP_STATUS_FAILS_BEFORE_UNLOCK) {
                ESP_LOGW(TAG, "ptpd_status failing (%d); reporting unlocked", ret);
                aes67_ptp_lock_state_t prev = ctx->lock_state;
                ctx->lock_state = AES67_PTP_UNLOCKED;
                ctx->stable_count = 0;
                if (prev != AES67_PTP_UNLOCKED && ctx->lock_cb) {
                    ctx->lock_cb(ctx->lock_state, ctx->lock_cb_user_data);
                }
            }
            continue;
        }
        ctx->status_failures = 0;

        /* Cache latest offset and path delay.
         * last_delta_ns is the measured clock error from the most recent sync. */
        ctx->offset_ns = (int32_t)status.last_delta_ns;
        ctx->path_delay_ns = (int32_t)status.path_delay_ns;
        ctx->drift_ppb = (int32_t)status.drift_ppb;
        ctx->transport = (aes67_ptp_transport_t)status.source_transport;
        ctx->master_clock_class = (uint8_t)status.clock_source_info.clockclass;
        ctx->master_accuracy = (uint8_t)status.clock_source_info.accuracy;

        /* Extract grandmaster identity from status struct */
        memcpy(ctx->grandmaster_id, status.clock_source_info.gm_id, 8);

        /* Detect if we are the grandmaster. When acting as GM, the ptpd daemon
         * will report no valid clock source, or the GM ID will match our own
         * EUI-64 (MAC with FF:FE inserted in the middle). */
        /* No master yet: before the grace period has run out we are simply
         * unlocked, not grandmaster (the clock may still be at the epoch). */
        bool is_self_gm = !status.clock_source_valid;
        if (is_self_gm &&
            (xTaskGetTickCount() - ctx->start_tick) < pdMS_TO_TICKS(PTP_GM_GRACE_MS)) {
            if (ctx->lock_state != AES67_PTP_UNLOCKED) {
                ctx->lock_state = AES67_PTP_UNLOCKED;
                ctx->stable_count = 0;
                if (ctx->lock_cb) ctx->lock_cb(ctx->lock_state, ctx->lock_cb_user_data);
            }
            continue;
        }
        if (!is_self_gm) {
            is_self_gm = (
            ctx->grandmaster_id[0] == ctx->own_mac[0] &&
            ctx->grandmaster_id[1] == ctx->own_mac[1] &&
            ctx->grandmaster_id[2] == ctx->own_mac[2] &&
            ctx->grandmaster_id[3] == 0xFF &&
            ctx->grandmaster_id[4] == 0xFE &&
            ctx->grandmaster_id[5] == ctx->own_mac[3] &&
            ctx->grandmaster_id[6] == ctx->own_mac[4] &&
            ctx->grandmaster_id[7] == ctx->own_mac[5]);
        }

        if (is_self_gm) {

            if (!ctx->is_grandmaster) {
                ctx->is_grandmaster = true;
                ESP_LOGI(TAG, "This node is PTP grandmaster "
                         "(GM ID: %02X-%02X-%02X-%02X-%02X-%02X-%02X-%02X)",
                         ctx->own_mac[0], ctx->own_mac[1], ctx->own_mac[2],
                         0xFF, 0xFE,
                         ctx->own_mac[3], ctx->own_mac[4], ctx->own_mac[5]);
            }

            /* When acting as grandmaster, we are always "locked" to our own clock */
            if (ctx->lock_state != AES67_PTP_LOCKED) {
                aes67_ptp_lock_state_t prev = ctx->lock_state;
                ctx->lock_state = AES67_PTP_LOCKED;
                ctx->offset_ns = 0;
                ctx->path_delay_ns = 0;
                if (prev != AES67_PTP_LOCKED && ctx->lock_cb) {
                    ctx->lock_cb(ctx->lock_state, ctx->lock_cb_user_data);
                }
            }
        } else {
            if (ctx->is_grandmaster) {
                ctx->is_grandmaster = false;
                ESP_LOGI(TAG, "External grandmaster detected, switching to slave mode "
                         "(GM ID: %02X-%02X-%02X-%02X-%02X-%02X-%02X-%02X)",
                         ctx->grandmaster_id[0], ctx->grandmaster_id[1],
                         ctx->grandmaster_id[2], ctx->grandmaster_id[3],
                         ctx->grandmaster_id[4], ctx->grandmaster_id[5],
                         ctx->grandmaster_id[6], ctx->grandmaster_id[7]);
            }

            /* Run lock state machine for slave mode */
            bool changed = ptp_update_lock_state(ctx, status.last_delta_ns);
            if (changed && ctx->lock_cb) {
                ctx->lock_cb(ctx->lock_state, ctx->lock_cb_user_data);
            }
        }

        /* Periodic stats log every 20 polls (~10 seconds) */
        ctx->stats_counter++;
        if (ctx->stats_counter >= 20) {
            ctx->stats_counter = 0;

            const char *role = ctx->is_grandmaster ? "master" : "slave";
            const char *lock = (ctx->lock_state == AES67_PTP_LOCKED) ? "locked" :
                               (ctx->lock_state == AES67_PTP_LOCKING) ? "locking" :
                               "unlocked";

            if (ctx->is_grandmaster) {
                ESP_LOGI(TAG, "[%s/%s] GM ID: %02X-%02X-%02X-%02X-%02X-%02X-%02X-%02X",
                         role, lock,
                         ctx->own_mac[0], ctx->own_mac[1], ctx->own_mac[2],
                         0xFF, 0xFE,
                         ctx->own_mac[3], ctx->own_mac[4], ctx->own_mac[5]);
            } else {
                ESP_LOGI(TAG, "[%s/%s/%s] offset: %+ld ns (filtered %+ld, jitter %ld), delay: %ld ns, "
                         "drift: %+ld ppb, GM: %02X-%02X-%02X-%02X-%02X-%02X-%02X-%02X",
                         role, lock,
                         ctx->transport == AES67_PTP_TRANSPORT_L2 ? "l2" :
                         ctx->transport == AES67_PTP_TRANSPORT_UDP ? "udp" : "-",
                         (long)ctx->offset_ns, (long)ctx->filtered_offset_ns, (long)ctx->jitter_ns,
                         (long)ctx->path_delay_ns, (long)ctx->drift_ppb,
                         ctx->grandmaster_id[0], ctx->grandmaster_id[1],
                         ctx->grandmaster_id[2], ctx->grandmaster_id[3],
                         ctx->grandmaster_id[4], ctx->grandmaster_id[5],
                         ctx->grandmaster_id[6], ctx->grandmaster_id[7]);
            }
        }
    }

    ESP_LOGI(TAG, "PTP monitor task exiting");
        xSemaphoreGive(ctx->monitor_done);
    vTaskDelete(NULL);
}

/* -------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------- */

esp_err_t aes67_ptp_init(esp_eth_handle_t eth_handle, const aes67_ptp_config_t *config,
                         const char *netif_key, aes67_ptp_handle_t *handle)
{
    ESP_RETURN_ON_FALSE(netif_key && netif_key[0], ESP_ERR_INVALID_ARG, TAG, "netif key required");
    ESP_RETURN_ON_FALSE(eth_handle != NULL, ESP_ERR_INVALID_ARG, TAG, "eth_handle is NULL");
    ESP_RETURN_ON_FALSE(config != NULL, ESP_ERR_INVALID_ARG, TAG, "config is NULL");
    ESP_RETURN_ON_FALSE(handle != NULL, ESP_ERR_INVALID_ARG, TAG, "handle is NULL");

    struct aes67_ptp_ctx *ctx = calloc(1, sizeof(struct aes67_ptp_ctx));
    ESP_RETURN_ON_FALSE(ctx != NULL, ESP_ERR_NO_MEM, TAG, "failed to allocate PTP context");

    ctx->eth_handle = eth_handle;
    ctx->config = *config;
    strlcpy(ctx->netif_key, netif_key, sizeof(ctx->netif_key));
    ctx->monitor_done = xSemaphoreCreateBinary();
    if (!ctx->monitor_done) {
        free(ctx);
        return ESP_ERR_NO_MEM;
    }
    ctx->lock_state = AES67_PTP_UNLOCKED;
    ctx->ptpd_pid = -1;
    ctx->running = false;
    ctx->is_grandmaster = false;

    /* Read the MAC address for grandmaster identity detection.
     * The EUI-64 GM ID is derived by inserting FF:FE into the MAC. */
    esp_eth_ioctl(eth_handle, ETH_CMD_G_MAC_ADDR, ctx->own_mac);

    /* Register the L2 TAP VFS interface. The ptpd component opens
     * /dev/net/tap for Layer 2 PTP packet transport with hardware
     * timestamping. This must happen before ptpd_start(). */
    esp_err_t ret = esp_vfs_l2tap_intf_register(NULL);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        /* ESP_ERR_INVALID_STATE means already registered, which is fine */
        ESP_LOGE(TAG, "esp_vfs_l2tap_intf_register failed: %s", esp_err_to_name(ret));
        free(ctx);
        return ret;
    }

    /* Initialize the hardware PTP clock tied to the EMAC */
    esp_eth_clock_cfg_t clk_cfg = {
        .eth_hndl = eth_handle,
    };
    ret = esp_eth_clock_init(CLOCK_PTP_SYSTEM, &clk_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_eth_clock_init failed: %s", esp_err_to_name(ret));
        free(ctx);
        return ret;
    }

    ESP_LOGI(TAG, "PTP initialized (domain %u, own ID: %02X-%02X-%02X-FF-FE-%02X-%02X-%02X)",
             config->domain,
             ctx->own_mac[0], ctx->own_mac[1], ctx->own_mac[2],
             ctx->own_mac[3], ctx->own_mac[4], ctx->own_mac[5]);
    *handle = ctx;
    return ESP_OK;
}

esp_err_t aes67_ptp_start(aes67_ptp_handle_t handle)
{
    ESP_RETURN_ON_FALSE(handle != NULL, ESP_ERR_INVALID_ARG, TAG, "handle is NULL");
    ESP_RETURN_ON_FALSE(!handle->running, ESP_ERR_INVALID_STATE, TAG, "PTP already running");

    /* Start the ptpd daemon on the configured netif key; the call returns
     * only after the daemon has opened its tap and sockets, or failed. */
    int pid = ptpd_start(handle->netif_key);
    if (pid < 0) {
        ESP_LOGE(TAG, "ptpd_start on \"%s\" failed (%d)", handle->netif_key, pid);
        return ESP_FAIL;
    }
    handle->status_failures = 0;

    handle->ptpd_pid = pid;
    handle->running = true;
    handle->lock_state = AES67_PTP_UNLOCKED;
    handle->stable_count = 0;
    handle->filter_primed = false;
    handle->is_grandmaster = false;
    handle->transport = AES67_PTP_TRANSPORT_NONE;
    handle->start_tick = xTaskGetTickCount();

    ESP_LOGI(TAG, "ptpd started (pid %d)", pid);

    /* Spawn monitor task to track lock state */
    BaseType_t xret = xTaskCreate(
        ptp_monitor_task,
        "ptp_monitor",
        PTP_MONITOR_STACK_SIZE,
        handle,
        PTP_MONITOR_PRIORITY,
        &handle->monitor_task
    );
    if (xret != pdPASS) {
        ESP_LOGE(TAG, "failed to create PTP monitor task");
        ptpd_stop(handle->ptpd_pid);
        handle->running = false;
        handle->ptpd_pid = -1;
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

esp_err_t aes67_ptp_stop(aes67_ptp_handle_t handle)
{
    ESP_RETURN_ON_FALSE(handle != NULL, ESP_ERR_INVALID_ARG, TAG, "handle is NULL");
    ESP_RETURN_ON_FALSE(handle->running, ESP_ERR_INVALID_STATE, TAG, "PTP not running");

    /* Signal the monitor task to stop, then wait for it to exit */
    handle->running = false;

    /* The monitor wakes every PTP_MONITOR_INTERVAL_MS; wait for it to
     * signal that it has left its loop. */
    if (handle->monitor_task &&
        xSemaphoreTake(handle->monitor_done, pdMS_TO_TICKS(PTP_MONITOR_INTERVAL_MS * 3)) != pdTRUE) {
        ESP_LOGW(TAG, "PTP monitor task did not exit in time");
    }
    handle->monitor_task = NULL;

    /* Stop the ptpd daemon */
    int ret = ptpd_stop(handle->ptpd_pid);
    if (ret != 0) {
        ESP_LOGW(TAG, "ptpd_stop returned %d", ret);
    }
    handle->ptpd_pid = -1;

    /* Reset state */
    aes67_ptp_lock_state_t prev_state = handle->lock_state;
    handle->lock_state = AES67_PTP_UNLOCKED;
    handle->stable_count = 0;
    handle->offset_ns = 0;
    handle->path_delay_ns = 0;

    /* Notify if state changed */
    if (prev_state != AES67_PTP_UNLOCKED && handle->lock_cb) {
        handle->lock_cb(AES67_PTP_UNLOCKED, handle->lock_cb_user_data);
    }

    ESP_LOGI(TAG, "PTP stopped");
    return ESP_OK;
}

esp_err_t aes67_ptp_destroy(aes67_ptp_handle_t handle)
{
    ESP_RETURN_ON_FALSE(handle != NULL, ESP_ERR_INVALID_ARG, TAG, "handle is NULL");

    /* Stop first if still running */
    if (handle->running) {
        esp_err_t ret = aes67_ptp_stop(handle);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "aes67_ptp_stop returned %s during destroy", esp_err_to_name(ret));
        }
    }

    vSemaphoreDelete(handle->monitor_done);
    free(handle);
    ESP_LOGI(TAG, "PTP context destroyed");
    return ESP_OK;
}

esp_err_t aes67_ptp_get_status(aes67_ptp_handle_t handle, aes67_ptp_status_t *status)
{
    ESP_RETURN_ON_FALSE(handle != NULL, ESP_ERR_INVALID_ARG, TAG, "handle is NULL");
    ESP_RETURN_ON_FALSE(status != NULL, ESP_ERR_INVALID_ARG, TAG, "status is NULL");

    status->lock_state = handle->lock_state;
    status->domain = handle->config.domain;
    status->offset_ns = handle->offset_ns;
    status->path_delay_ns = handle->path_delay_ns;
    status->jitter_ns = handle->jitter_ns;
    status->drift_ppb = handle->drift_ppb;
    status->filtered_offset_ns = handle->filtered_offset_ns;
    status->is_grandmaster = handle->is_grandmaster;
    status->transport = handle->is_grandmaster ? AES67_PTP_TRANSPORT_NONE : handle->transport;
    status->master_clock_class = handle->master_clock_class;
    status->master_accuracy = handle->master_accuracy;

    /* When acting as grandmaster, report our own EUI-64 as the GM ID */
    if (handle->is_grandmaster) {
        status->grandmaster_id[0] = handle->own_mac[0];
        status->grandmaster_id[1] = handle->own_mac[1];
        status->grandmaster_id[2] = handle->own_mac[2];
        status->grandmaster_id[3] = 0xFF;
        status->grandmaster_id[4] = 0xFE;
        status->grandmaster_id[5] = handle->own_mac[3];
        status->grandmaster_id[6] = handle->own_mac[4];
        status->grandmaster_id[7] = handle->own_mac[5];
    } else {
        memcpy(status->grandmaster_id, handle->grandmaster_id, 8);
    }

    return ESP_OK;
}

esp_err_t aes67_ptp_get_time_ns(aes67_ptp_handle_t handle, uint64_t *time_ns)
{
    ESP_RETURN_ON_FALSE(handle != NULL, ESP_ERR_INVALID_ARG, TAG, "handle is NULL");
    ESP_RETURN_ON_FALSE(time_ns != NULL, ESP_ERR_INVALID_ARG, TAG, "time_ns is NULL");

    struct timespec ts;
    int ret = esp_eth_clock_gettime(CLOCK_PTP_SYSTEM, &ts);
    if (ret != 0) {
        ESP_LOGE(TAG, "esp_eth_clock_gettime failed (%d)", ret);
        return ESP_FAIL;
    }

    *time_ns = (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
    return ESP_OK;
}

esp_err_t aes67_ptp_get_time(aes67_ptp_handle_t handle, uint32_t *sec, uint32_t *nsec)
{
    ESP_RETURN_ON_FALSE(handle != NULL, ESP_ERR_INVALID_ARG, TAG, "handle is NULL");
    ESP_RETURN_ON_FALSE(sec != NULL && nsec != NULL, ESP_ERR_INVALID_ARG, TAG, "output is NULL");

    struct timespec ts;
    int ret = esp_eth_clock_gettime(CLOCK_PTP_SYSTEM, &ts);
    if (ret != 0) {
        ESP_LOGE(TAG, "esp_eth_clock_gettime failed (%d)", ret);
        return ESP_FAIL;
    }

    *sec = (uint32_t)ts.tv_sec;
    *nsec = (uint32_t)ts.tv_nsec;
    return ESP_OK;
}

uint32_t aes67_ptp_time_to_rtp_ts(uint64_t ptp_time_ns, uint32_t sample_rate)
{
    /*
     * RTP timestamp = (ptp_time_ns * sample_rate) / 1_000_000_000
     *
     * To avoid 128-bit overflow we split the computation:
     *   seconds part: (ptp_time_ns / 1e9) * sample_rate
     *   sub-second part: (ptp_time_ns % 1e9) * sample_rate / 1e9
     *
     * The result is taken modulo 2^32 (implicit uint32_t wrap).
     */
    uint64_t sec = ptp_time_ns / 1000000000ULL;
    uint64_t nsec = ptp_time_ns % 1000000000ULL;

    uint64_t ts = (sec * sample_rate) + (nsec * sample_rate / 1000000000ULL);
    return (uint32_t)ts;
}

esp_err_t aes67_ptp_register_lock_cb(aes67_ptp_handle_t handle,
                                     aes67_ptp_lock_cb_t cb, void *user_data)
{
    ESP_RETURN_ON_FALSE(handle != NULL, ESP_ERR_INVALID_ARG, TAG, "handle is NULL");

    handle->lock_cb = cb;
    handle->lock_cb_user_data = user_data;
    return ESP_OK;
}

esp_err_t aes67_ptp_get_grandmaster_str(aes67_ptp_handle_t handle, char *buf, size_t len)
{
    ESP_RETURN_ON_FALSE(handle != NULL, ESP_ERR_INVALID_ARG, TAG, "handle is NULL");
    ESP_RETURN_ON_FALSE(buf != NULL, ESP_ERR_INVALID_ARG, TAG, "buf is NULL");
    ESP_RETURN_ON_FALSE(len >= 24, ESP_ERR_INVALID_ARG, TAG,
                        "buf too small (need 24 bytes for \"XX-XX-XX-XX-XX-XX-XX-XX\")");

    const uint8_t *gm = handle->grandmaster_id;
    snprintf(buf, len, "%02X-%02X-%02X-%02X-%02X-%02X-%02X-%02X",
             gm[0], gm[1], gm[2], gm[3], gm[4], gm[5], gm[6], gm[7]);

    return ESP_OK;
}

bool aes67_ptp_eth_input(const uint8_t *frame, uint32_t len, const void *info)
{
    return ptpd_hook_input(frame, len, info) == 1;
}
