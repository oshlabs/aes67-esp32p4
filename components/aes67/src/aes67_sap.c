/*
 * SPDX-FileCopyrightText: 2026 DatanoiseTV
 *
 * SPDX-License-Identifier: Apache-2.0
 */
/*
 * SAP (Session Announcement Protocol) - RFC 2974
 *
 * Handles periodic multicast announcement and discovery of AES67
 * audio sessions via SDP payloads over the SAP multicast address.
 */

#include "aes67_sap.h"
#include "aes67_sdp.h"
#include "aes67_net.h"

#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "aes67_sap";

/* SAP protocol constants */
#define SAP_HEADER_SIZE      8
#define SAP_CONTENT_TYPE     "application/sdp"
#define SAP_CONTENT_TYPE_LEN 16  /* includes null terminator */
#define SAP_ANNOUNCE_FLAG    0x20
#define SAP_DELETE_FLAG      0x24
#define SAP_FLAG_VERSION(f)  ((f) >> 5)
#define SAP_FLAG_IPV6        0x10
#define SAP_FLAG_DELETE      0x04
#define SAP_FLAG_ENCRYPTED   0x02
#define SAP_FLAG_COMPRESSED  0x01
/* Announce timing (RFC 2974: a session times out after ten times its
 * announce interval or an hour, whichever is larger; AES67 and RAVENNA
 * devices announce about every 30 s). A new source is announced at once
 * and twice more shortly after so it is found without waiting a period. */
#define SAP_TX_INTERVAL_S_DEFAULT   30
#define SAP_MIN_EXPIRY_S_DEFAULT    60
#define SAP_MAX_EXPIRY_MS           3600000
#define SAP_BURST_REPEATS           2
#define SAP_BURST_SPACING_MS        1500
#define SAP_RX_BUF_SIZE      1500
#define SAP_MAX_LOCAL         8
#define SAP_MAX_REMOTE        16

#define SAP_SELECT_TIMEOUT_S  1       /* bounds the stop latency and the expiry scan */
#define SAP_RX_TASK_STACK     12288  /* Needs room for callback -> add_source -> SDP log */
#define SAP_TX_TASK_STACK     4096
#define SAP_RX_TASK_PRIO      5   /* Background - discovery */
#define SAP_TX_TASK_PRIO      4   /* Background - periodic announce */

/* Locally announced source */
typedef struct {
    bool active;
    uint16_t msg_id;
    uint32_t origin_ip;
    uint32_t next_tx_tick;      /* when the TX task sends it next */
    uint8_t burst_left;         /* quick repeats still to send after creation */
    char sdp[AES67_SAP_MAX_SDP_LEN];
} sap_local_source_t;

/* Full SAP context */
struct aes67_sap_ctx {
    int sock;
    TaskHandle_t rx_task;
    TaskHandle_t tx_task;
    SemaphoreHandle_t rx_done;   /* given by each task when it exits */
    SemaphoreHandle_t tx_done;
    /* Guards local[] and remote[]. The RX task compacts remote[] with
     * memmove while announce/delete and the Web UI read the tables from
     * other tasks. Callbacks are invoked with the lock released, on a copy,
     * so they may call back into SAP. */
    SemaphoreHandle_t lock;
    aes67_sap_remote_source_t *event_copy;  /* RX task's copy for callbacks */
    bool running;

    aes67_sap_event_cb_t event_cb;
    void *user_data;
    uint32_t tx_interval_ms;
    uint32_t min_expiry_ms;

    sap_local_source_t local[SAP_MAX_LOCAL];
    int local_count;

    aes67_sap_remote_source_t remote[SAP_MAX_REMOTE];
    int remote_count;
};

/* Build a SAP packet (announce or delete) into buf.
 * Returns total packet length, or -1 on error. */
static int build_sap_packet(uint8_t *buf, size_t buf_len,
                            uint8_t flags, uint16_t msg_id,
                            uint32_t origin_ip, const char *sdp)
{
    size_t sdp_len = strlen(sdp);
    size_t total = SAP_HEADER_SIZE + SAP_CONTENT_TYPE_LEN + sdp_len;
    if (total > buf_len) {
        return -1;
    }

    /* SAP header per RFC 2974 */
    buf[0] = flags;
    buf[1] = 0;  /* auth_len = 0 */
    buf[2] = (uint8_t)(msg_id >> 8);
    buf[3] = (uint8_t)(msg_id & 0xFF);

    /* Originating source IP. The origin_ip value from lwIP is already
     * in network byte order, so copy the 4 bytes directly. */
    memcpy(buf + 4, &origin_ip, 4);

    /* Content type string with null terminator */
    memcpy(buf + SAP_HEADER_SIZE, SAP_CONTENT_TYPE, SAP_CONTENT_TYPE_LEN);

    /* SDP payload */
    memcpy(buf + SAP_HEADER_SIZE + SAP_CONTENT_TYPE_LEN, sdp, sdp_len);

    return (int)total;
}

/* Send a SAP packet to the multicast group */
static esp_err_t send_sap_packet(int sock, const uint8_t *pkt, int pkt_len)
{
    struct sockaddr_in dest = {
        .sin_family = AF_INET,
        .sin_port = htons(AES67_SAP_PORT),
    };
    inet_aton(AES67_SAP_MCAST_ADDR, &dest.sin_addr);

    int sent = sendto(sock, pkt, pkt_len, 0,
                      (struct sockaddr *)&dest, sizeof(dest));
    if (sent < 0) {
        ESP_LOGE(TAG, "SAP sendto %s:%d failed: errno %d",
                 AES67_SAP_MCAST_ADDR, AES67_SAP_PORT, errno);
        return ESP_FAIL;
    }
    ESP_LOGD(TAG, "SAP sent %d bytes to %s:%d",
             sent, AES67_SAP_MCAST_ADDR, AES67_SAP_PORT);
    return ESP_OK;
}

/* Find a remote source by origin_ip + msg_id. Returns index or -1. */
static int find_remote(struct aes67_sap_ctx *ctx, uint32_t origin_ip,
                       uint16_t msg_id)
{
    for (int i = 0; i < ctx->remote_count; i++) {
        if (ctx->remote[i].origin_ip == origin_ip &&
            ctx->remote[i].msg_id == msg_id) {
            return i;
        }
    }
    return -1;
}

/* Remove a remote source by index, compacting the array */
static void remove_remote(struct aes67_sap_ctx *ctx, int idx)
{
    if (idx < 0 || idx >= ctx->remote_count) return;

    if (idx < ctx->remote_count - 1) {
        memmove(&ctx->remote[idx], &ctx->remote[idx + 1],
                sizeof(aes67_sap_remote_source_t) * (ctx->remote_count - 1 - idx));
    }
    ctx->remote_count--;
}

/* Extract session name from SDP text for the remote source name field */
static void extract_session_name(const char *sdp, char *name, size_t name_len)
{
    const char *s_line = strstr(sdp, "s=");
    if (!s_line) {
        strncpy(name, "(unknown)", name_len - 1);
        name[name_len - 1] = '\0';
        return;
    }

    s_line += 2;
    const char *end = s_line;
    while (*end && *end != '\r' && *end != '\n') {
        end++;
    }

    /* Keep printable bytes only (control characters and DEL dropped),
     * cap the length, and never leave the name empty. */
    size_t n = 0;
    for (const char *c = s_line; c < end && n < name_len - 1; c++) {
        unsigned char u = (unsigned char)*c;
        if (u >= 0x20 && u != 0x7F) {
            name[n++] = *c;
        }
    }
    name[n] = '\0';
    if (n == 0) {
        strncpy(name, "(unnamed)", name_len - 1);
        name[name_len - 1] = '\0';
    }
}

/* Handle a received SAP packet */
static void handle_sap_rx(struct aes67_sap_ctx *ctx, const uint8_t *data,
                          int len)
{
    /* Header per RFC 2974: flags, auth length (32-bit words), message id
     * hash, origin (IPv4 or IPv6), optional authentication data, then an
     * optional null-terminated payload type; the payload follows. */
    if (len < SAP_HEADER_SIZE + 4) {
        return;
    }
    uint8_t flags = data[0];
    if (SAP_FLAG_VERSION(flags) != 1) {
        ESP_LOGD(TAG, "SAP version %u ignored", SAP_FLAG_VERSION(flags));
        return;
    }
    if (flags & (SAP_FLAG_ENCRYPTED | SAP_FLAG_COMPRESSED)) {
        ESP_LOGD(TAG, "encrypted or compressed SAP payload ignored");
        return;
    }
    uint32_t auth_len = (uint32_t)data[1] * 4;
    uint16_t msg_id = ((uint16_t)data[2] << 8) | data[3];
    if (flags & SAP_FLAG_IPV6) {
        ESP_LOGD(TAG, "SAP with IPv6 origin ignored");
        return;
    }
    uint32_t origin_ip;
    memcpy(&origin_ip, data + 4, 4); /* Already in network byte order */

    uint32_t off = SAP_HEADER_SIZE + auth_len;
    if (off + 4 > (uint32_t)len) {
        return;
    }
    const char *payload = (const char *)(data + off);
    uint32_t avail = (uint32_t)len - off;
    if (memcmp(payload, "v=0", 3) != 0) {
        /* A payload type precedes the SDP; only application/sdp is understood */
        size_t tl = strnlen(payload, avail);
        if (tl == avail || strcmp(payload, SAP_CONTENT_TYPE) != 0) {
            ESP_LOGD(TAG, "SAP payload type not application/sdp, ignoring");
            return;
        }
        payload += tl + 1;
        avail -= tl + 1;
        if (avail < 4 || memcmp(payload, "v=0", 3) != 0) {
            return;
        }
    }
    const char *sdp = payload;
    int sdp_len = (int)avail;

    bool is_delete = (flags & SAP_FLAG_DELETE) != 0;

    /* Event to deliver after the lock is released, if any */
    aes67_sap_remote_source_t *event = ctx->event_copy;
    bool have_event = false, event_is_announce = false;

    xSemaphoreTake(ctx->lock, portMAX_DELAY);
    if (is_delete) {
        /* Remove this source */
        int idx = find_remote(ctx, origin_ip, msg_id);
        if (idx >= 0) {
            *event = ctx->remote[idx];
            remove_remote(ctx, idx);
            have_event = true;
            ESP_LOGI(TAG, "Source gone: \"%s\" (%d sources remaining)",
                     event->name, ctx->remote_count);
        }
    } else {
        /* Announce: add or update */
        int idx = find_remote(ctx, origin_ip, msg_id);
        if (idx >= 0) {
            /* Update existing entry: arrival time and the interval it
             * announces at (its expiry follows from that) */
            uint32_t now = xTaskGetTickCount();
            uint32_t gap_ms = pdTICKS_TO_MS(now - ctx->remote[idx].last_seen);
            if (gap_ms >= 500) {
                ctx->remote[idx].interval_ms = gap_ms;
            }
            ctx->remote[idx].last_seen = now;
            /* Update SDP in case it changed */
            if (sdp_len < (int)sizeof(ctx->remote[idx].sdp)) {
                memcpy(ctx->remote[idx].sdp, sdp, sdp_len);
                ctx->remote[idx].sdp[sdp_len] = '\0';
                extract_session_name(ctx->remote[idx].sdp,
                                     ctx->remote[idx].name,
                                     sizeof(ctx->remote[idx].name));
            }
        } else {
            /* New source */
            if (ctx->remote_count >= SAP_MAX_REMOTE) {
                xSemaphoreGive(ctx->lock);
                ESP_LOGW(TAG, "Remote source table full, ignoring new source");
                return;
            }

            aes67_sap_remote_source_t *src = &ctx->remote[ctx->remote_count];
            memset(src, 0, sizeof(*src));
            src->origin_ip = origin_ip;
            src->msg_id = msg_id;
            src->last_seen = xTaskGetTickCount();

            if (sdp_len < (int)sizeof(src->sdp)) {
                memcpy(src->sdp, sdp, sdp_len);
                src->sdp[sdp_len] = '\0';
            }
            extract_session_name(src->sdp, src->name, sizeof(src->name));

            ctx->remote_count++;

            /* Parse SDP for additional info to display */
            char ip_str[16] = {0};
            struct in_addr addr_tmp = { .s_addr = origin_ip };
            const char *ip = inet_ntoa(addr_tmp);
            if (ip) {
                strncpy(ip_str, ip, sizeof(ip_str) - 1);
            }

            ESP_LOGI(TAG, "Discovered: \"%s\" from %s (%d sources on network)",
                     src->name, ip_str, ctx->remote_count);
            *event = *src;
            have_event = true;
            event_is_announce = true;
        }
    }
    xSemaphoreGive(ctx->lock);

    if (have_event && ctx->event_cb) {
        ctx->event_cb(event_is_announce, event, ctx->user_data);
    }
}

/* Expiry of a remote source: ten announce intervals, at least the
 * configured minimum, at most an hour */
static uint32_t remote_expiry_ms(const struct aes67_sap_ctx *ctx,
                                 const aes67_sap_remote_source_t *src)
{
    uint32_t ms = src->interval_ms ? src->interval_ms * 10 : ctx->min_expiry_ms;
    if (ms < ctx->min_expiry_ms) {
        ms = ctx->min_expiry_ms;
    }
    if (ms > SAP_MAX_EXPIRY_MS) {
        ms = SAP_MAX_EXPIRY_MS;
    }
    return ms;
}

/* Check for timed-out remote sources */
static void expire_remote_sources(struct aes67_sap_ctx *ctx)
{
    aes67_sap_remote_source_t *expired = ctx->event_copy;
    uint32_t expired_after_ms = 0;

    /* Remove one expired entry per pass under the lock, deliver the event
     * with the lock released, repeat until none is left. */
    for (;;) {
        bool found = false;
        xSemaphoreTake(ctx->lock, portMAX_DELAY);
        uint32_t now = xTaskGetTickCount();
        for (int i = ctx->remote_count - 1; i >= 0; i--) {
            uint32_t expiry_ms = remote_expiry_ms(ctx, &ctx->remote[i]);
            if (pdTICKS_TO_MS(now - ctx->remote[i].last_seen) >= expiry_ms) {
                *expired = ctx->remote[i];
                expired_after_ms = expiry_ms;
                remove_remote(ctx, i);
                found = true;
                break;
            }
        }
        xSemaphoreGive(ctx->lock);
        if (!found) {
            return;
        }
        ESP_LOGI(TAG, "Source timeout: \"%s\" (no announcement for %lu s)",
                 expired->name, (unsigned long)(expired_after_ms / 1000));
        if (ctx->event_cb) {
            ctx->event_cb(false, expired, ctx->user_data);
        }
    }
}

/* RX task: listens for incoming SAP packets */
static void sap_rx_task(void *arg)
{
    struct aes67_sap_ctx *ctx = (struct aes67_sap_ctx *)arg;

    /* Allocate RX buffer on heap to avoid stack overflow. select() and
     * the newlib printf internals consume significant stack space. */
    uint8_t *rx_buf = malloc(SAP_RX_BUF_SIZE);
    if (!rx_buf) {
        ESP_LOGE(TAG, "Failed to allocate SAP RX buffer");
            xSemaphoreGive(ctx->rx_done);
    vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "SAP RX task started");

    while (ctx->running) {
        /* Use a timeout so we can periodically check for expired sources */
        struct timeval tv = {
            .tv_sec = SAP_SELECT_TIMEOUT_S,
            .tv_usec = 0,
        };
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(ctx->sock, &rfds);

        int ret = select(ctx->sock + 1, &rfds, NULL, NULL, &tv);
        if (ret < 0) {
            if (errno == EINTR) continue;
            ESP_LOGE(TAG, "SAP select() failed: errno %d", errno);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        if (ret > 0 && FD_ISSET(ctx->sock, &rfds)) {
            struct sockaddr_in from;
            socklen_t from_len = sizeof(from);
            int len = recvfrom(ctx->sock, rx_buf, SAP_RX_BUF_SIZE, 0,
                               (struct sockaddr *)&from, &from_len);
            if (len > 0) {
                handle_sap_rx(ctx, rx_buf, len);
            }
        }

        expire_remote_sources(ctx);
    }

    free(rx_buf);
    ESP_LOGI(TAG, "SAP RX task stopped");
        xSemaphoreGive(ctx->rx_done);
    vTaskDelete(NULL);
}

/* TX task: periodically re-announces local sources */
static void sap_tx_task(void *arg)
{
    struct aes67_sap_ctx *ctx = (struct aes67_sap_ctx *)arg;
    uint8_t tx_buf[SAP_RX_BUF_SIZE];

    ESP_LOGI(TAG, "SAP TX task started");

    while (ctx->running) {
        /* Announce every local source that is due: the quick repeats after
         * creation first, then once per interval */
        uint32_t now = xTaskGetTickCount();
        for (int i = 0; i < SAP_MAX_LOCAL; i++) {
            xSemaphoreTake(ctx->lock, portMAX_DELAY);
            int pkt_len = -1;
            sap_local_source_t *l = &ctx->local[i];
            if (l->active && (int32_t)(now - l->next_tx_tick) >= 0) {
                pkt_len = build_sap_packet(tx_buf, sizeof(tx_buf),
                                           SAP_ANNOUNCE_FLAG, l->msg_id,
                                           l->origin_ip, l->sdp);
                if (l->burst_left) {
                    l->burst_left--;
                }
                l->next_tx_tick = now + pdMS_TO_TICKS(l->burst_left ? SAP_BURST_SPACING_MS
                                                                    : ctx->tx_interval_ms);
            }
            xSemaphoreGive(ctx->lock);
            if (pkt_len > 0) {
                send_sap_packet(ctx->sock, tx_buf, pkt_len);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    ESP_LOGI(TAG, "SAP TX task stopped");
        xSemaphoreGive(ctx->tx_done);
    vTaskDelete(NULL);
}

esp_err_t aes67_sap_init(aes67_sap_handle_t *handle)
{
    if (!handle) return ESP_ERR_INVALID_ARG;

    struct aes67_sap_ctx *ctx = calloc(1, sizeof(struct aes67_sap_ctx));
    if (!ctx) {
        ESP_LOGE(TAG, "Failed to allocate SAP context");
        return ESP_ERR_NO_MEM;
    }

    ctx->sock = -1;
    ctx->running = false;
    ctx->event_cb = NULL;
    ctx->user_data = NULL;
    ctx->local_count = 0;
    ctx->remote_count = 0;
    ctx->tx_interval_ms = SAP_TX_INTERVAL_S_DEFAULT * 1000;
    ctx->min_expiry_ms = SAP_MIN_EXPIRY_S_DEFAULT * 1000;
    ctx->lock = xSemaphoreCreateMutex();
    ctx->rx_done = xSemaphoreCreateBinary();
    ctx->tx_done = xSemaphoreCreateBinary();
    ctx->event_copy = malloc(sizeof(aes67_sap_remote_source_t));
    if (!ctx->lock || !ctx->rx_done || !ctx->tx_done || !ctx->event_copy) {
        if (ctx->lock) vSemaphoreDelete(ctx->lock);
        if (ctx->rx_done) vSemaphoreDelete(ctx->rx_done);
        if (ctx->tx_done) vSemaphoreDelete(ctx->tx_done);
        free(ctx->event_copy);
        free(ctx);
        return ESP_ERR_NO_MEM;
    }

    *handle = ctx;
    ESP_LOGI(TAG, "SAP subsystem initialized");
    return ESP_OK;
}

esp_err_t aes67_sap_set_timing(aes67_sap_handle_t handle, uint16_t interval_s, uint16_t min_expiry_s)
{
    if (!handle) return ESP_ERR_INVALID_ARG;
    handle->tx_interval_ms = (interval_s ? interval_s : SAP_TX_INTERVAL_S_DEFAULT) * 1000UL;
    handle->min_expiry_ms = (min_expiry_s ? min_expiry_s : SAP_MIN_EXPIRY_S_DEFAULT) * 1000UL;
    return ESP_OK;
}

esp_err_t aes67_sap_start(aes67_sap_handle_t handle)
{
    if (!handle) return ESP_ERR_INVALID_ARG;
    if (handle->running) return ESP_ERR_INVALID_STATE;

    /* Create UDP socket for SAP */
    handle->sock = aes67_net_create_udp_socket(AES67_SAP_PORT, true);
    if (handle->sock < 0) {
        ESP_LOGE(TAG, "Failed to create SAP socket");
        return ESP_FAIL;
    }

    /* Get local IP first so we can bind multicast to the correct interface */
    uint32_t local_ip = 0;
    aes67_net_get_local_ip(&local_ip);
    char local_ip_str[16];
    aes67_net_u32_to_ip(local_ip, local_ip_str, sizeof(local_ip_str));

    /* Join the SAP multicast group on the local interface (not INADDR_ANY) */
    esp_err_t err = aes67_net_join_multicast(handle->sock,
                                              AES67_SAP_MCAST_ADDR,
                                              local_ip_str);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to join SAP multicast group");
        close(handle->sock);
        handle->sock = -1;
        return err;
    }

    /* RFC 2974 requires SAP packets to have TTL=255 */
    aes67_net_set_multicast_ttl(handle->sock, 255);

    /* Disable multicast loopback so we don't receive our own announcements */
    int loop = 0;
    setsockopt(handle->sock, IPPROTO_IP, IP_MULTICAST_LOOP,
               &loop, sizeof(loop));

    /* Set the outgoing multicast interface to our local IP so packets
     * go out via Ethernet, not a random interface */
    struct in_addr mcast_if = { .s_addr = local_ip };
    setsockopt(handle->sock, IPPROTO_IP, IP_MULTICAST_IF,
               &mcast_if, sizeof(mcast_if));

    handle->running = true;

    /* Start RX task */
    BaseType_t ret = xTaskCreate(sap_rx_task, "sap_rx", SAP_RX_TASK_STACK,
                                 handle, SAP_RX_TASK_PRIO, &handle->rx_task);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create SAP RX task");
        handle->running = false;
        close(handle->sock);
        handle->sock = -1;
        return ESP_ERR_NO_MEM;
    }

    /* Start TX task */
    ret = xTaskCreate(sap_tx_task, "sap_tx", SAP_TX_TASK_STACK,
                      handle, SAP_TX_TASK_PRIO, &handle->tx_task);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create SAP TX task");
        handle->running = false;
        xSemaphoreTake(handle->rx_done, pdMS_TO_TICKS(2000));
        handle->rx_task = NULL;
        close(handle->sock);
        handle->sock = -1;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "SAP started on port %d", AES67_SAP_PORT);
    return ESP_OK;
}

esp_err_t aes67_sap_stop(aes67_sap_handle_t handle)
{
    if (!handle) return ESP_ERR_INVALID_ARG;
    if (!handle->running) return ESP_OK;

    ESP_LOGI(TAG, "Stopping SAP...");
    handle->running = false;

    /* The RX task wakes on its select timeout, the TX task every 100 ms;
     * wait for each to signal that it has left its loop. */
    if (handle->rx_task &&
        xSemaphoreTake(handle->rx_done, pdMS_TO_TICKS(SAP_SELECT_TIMEOUT_S * 1000 + 500)) != pdTRUE) {
        ESP_LOGW(TAG, "SAP RX task did not exit in time");
    }
    if (handle->tx_task &&
        xSemaphoreTake(handle->tx_done, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGW(TAG, "SAP TX task did not exit in time");
    }

    if (handle->sock >= 0) {
        aes67_net_leave_multicast(handle->sock, AES67_SAP_MCAST_ADDR, NULL);
        close(handle->sock);
        handle->sock = -1;
    }

    handle->rx_task = NULL;
    handle->tx_task = NULL;

    ESP_LOGI(TAG, "SAP stopped");
    return ESP_OK;
}

esp_err_t aes67_sap_destroy(aes67_sap_handle_t handle)
{
    if (!handle) return ESP_ERR_INVALID_ARG;

    if (handle->running) {
        aes67_sap_stop(handle);
    }

    vSemaphoreDelete(handle->lock);
    vSemaphoreDelete(handle->rx_done);
    vSemaphoreDelete(handle->tx_done);
    free(handle->event_copy);
    free(handle);
    ESP_LOGI(TAG, "SAP context destroyed");
    return ESP_OK;
}

esp_err_t aes67_sap_announce(aes67_sap_handle_t handle,
                             uint16_t msg_id, uint32_t origin_ip,
                             const char *sdp)
{
    if (!handle || !sdp) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(handle->lock, portMAX_DELAY);
    int slot = -1;
    bool updated = false;
    for (int i = 0; i < SAP_MAX_LOCAL; i++) {
        if (handle->local[i].active && handle->local[i].msg_id == msg_id) {
            slot = i;
            updated = true;
            break;
        }
    }
    if (slot < 0) {
        for (int i = 0; i < SAP_MAX_LOCAL; i++) {
            if (!handle->local[i].active) {
                slot = i;
                break;
            }
        }
    }
    if (slot < 0) {
        xSemaphoreGive(handle->lock);
        ESP_LOGE(TAG, "Local source table full (max %d)", SAP_MAX_LOCAL);
        return ESP_ERR_NO_MEM;
    }
    handle->local[slot].active = true;
    handle->local[slot].msg_id = msg_id;
    handle->local[slot].origin_ip = origin_ip;
    if (!updated) {
        /* Sent at once below; the TX task repeats it twice quickly, then
         * once per interval */
        handle->local[slot].burst_left = SAP_BURST_REPEATS;
        handle->local[slot].next_tx_tick = xTaskGetTickCount() + pdMS_TO_TICKS(SAP_BURST_SPACING_MS);
    }
    strncpy(handle->local[slot].sdp, sdp, AES67_SAP_MAX_SDP_LEN - 1);
    handle->local[slot].sdp[AES67_SAP_MAX_SDP_LEN - 1] = '\0';
    if (!updated) {
        handle->local_count++;
    }
    xSemaphoreGive(handle->lock);

    ESP_LOGI(TAG, "%s local source announcement (id=0x%04X)",
             updated ? "Updated" : "Added", msg_id);

    /* Send an immediate announcement if already running */
    if (!updated && handle->running && handle->sock >= 0) {
        uint8_t pkt[SAP_RX_BUF_SIZE];
        int pkt_len = build_sap_packet(pkt, sizeof(pkt),
                                       SAP_ANNOUNCE_FLAG,
                                       msg_id, origin_ip, sdp);
        if (pkt_len > 0) {
            send_sap_packet(handle->sock, pkt, pkt_len);
        }
    }
    return ESP_OK;
}

esp_err_t aes67_sap_delete(aes67_sap_handle_t handle,
                           uint16_t msg_id, uint32_t origin_ip,
                           const char *sdp)
{
    if (!handle || !sdp) return ESP_ERR_INVALID_ARG;

    /* Send a deletion packet */
    if (handle->running && handle->sock >= 0) {
        uint8_t pkt[SAP_RX_BUF_SIZE];
        int pkt_len = build_sap_packet(pkt, sizeof(pkt),
                                       SAP_DELETE_FLAG,
                                       msg_id, origin_ip, sdp);
        if (pkt_len > 0) {
            send_sap_packet(handle->sock, pkt, pkt_len);
        }
    }

    /* Remove from local source table */
    bool found = false;
    xSemaphoreTake(handle->lock, portMAX_DELAY);
    for (int i = 0; i < SAP_MAX_LOCAL; i++) {
        if (handle->local[i].active && handle->local[i].msg_id == msg_id) {
            handle->local[i].active = false;
            handle->local_count--;
            found = true;
            break;
        }
    }
    xSemaphoreGive(handle->lock);
    if (found) {
        ESP_LOGI(TAG, "Removed local source (id=0x%04X)", msg_id);
        return ESP_OK;
    }
    ESP_LOGW(TAG, "Local source id=0x%04X not found for deletion", msg_id);
    return ESP_ERR_NOT_FOUND;
}

esp_err_t aes67_sap_register_cb(aes67_sap_handle_t handle,
                                aes67_sap_event_cb_t cb, void *user_data)
{
    if (!handle) return ESP_ERR_INVALID_ARG;
    handle->event_cb = cb;
    handle->user_data = user_data;
    return ESP_OK;
}

int aes67_sap_get_remote_count(aes67_sap_handle_t handle)
{
    if (!handle) return 0;
    xSemaphoreTake(handle->lock, portMAX_DELAY);
    int n = handle->remote_count;
    xSemaphoreGive(handle->lock);
    return n;
}

esp_err_t aes67_sap_get_remote(aes67_sap_handle_t handle, int index,
                               aes67_sap_remote_source_t *source)
{
    if (!handle || !source) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(handle->lock, portMAX_DELAY);
    if (index < 0 || index >= handle->remote_count) {
        xSemaphoreGive(handle->lock);
        return ESP_ERR_NOT_FOUND;
    }
    *source = handle->remote[index];
    xSemaphoreGive(handle->lock);
    return ESP_OK;
}
