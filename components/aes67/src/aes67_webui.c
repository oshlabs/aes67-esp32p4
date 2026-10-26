/*
 * SPDX-FileCopyrightText: 2026 DatanoiseTV
 *
 * SPDX-License-Identifier: Apache-2.0
 */
/*
 * AES67 Web UI - HTTP server and WebSocket backend.
 *
 * Provides a REST API for device status and stream management,
 * serves an embedded gzipped SPA, and pushes real-time status
 * updates over WebSocket at 2Hz.
 *
 * All data reads use copy-out accessor functions from the node/session/
 * PTP/SAP subsystems. No direct struct access, no mutex contention
 * with audio paths.
 */

#include "aes67_webui.h"
#include "aes67.h"
#include "aes67_config.h"
#include "aes67_session.h"
#include "aes67_ptp.h"
#include "aes67_rtp.h"
#include "aes67_sap.h"
#include "aes67_sdp.h"
#include "aes67_net.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

static const char *TAG = "aes67_webui";

/* Embedded gzipped index.html from EMBED_FILES */
extern const uint8_t index_html_gz_start[] asm("_binary_index_html_gz_start");
extern const uint8_t index_html_gz_end[]   asm("_binary_index_html_gz_end");

/* JSON response buffer size for REST endpoints and WS pushes */
#define JSON_BUF_SIZE 2048

/* ---- Context ---- */

/* Copy-out buffers for the status builder. They are large (a sink embeds a
 * 1 KB SDP, a SAP remote another 1 KB) and must not live on the server
 * task's stack, so one set is allocated with the context. */
struct webui_scratch {
    aes67_source_t              src;
    aes67_sink_t                sink;
    aes67_sap_remote_source_t   remote;
    aes67_sdp_t                 sdp;
};

struct aes67_webui {
    aes67_node_handle_t     node;
    httpd_handle_t          httpd;
    esp_timer_handle_t      push_timer;   /* periodic status push trigger */
    struct webui_scratch   *scratch;
    char                   *json;         /* status frame build buffer */
    uint16_t                port;
    bool                    running;
};

/* ---- Codec name helper ---- */

static const char *codec_name(aes67_codec_t codec)
{
    switch (codec) {
    case AES67_CODEC_L16:   return "L16";
    case AES67_CODEC_L24:   return "L24";
    case AES67_CODEC_L32:   return "L32";
    case AES67_CODEC_AM824: return "AM824";
    default:                return "unknown";
    }
}

/* ---- PTP lock state string ---- */

static const char *ptp_lock_str(aes67_ptp_lock_state_t state)
{
    switch (state) {
    case AES67_PTP_LOCKED:   return "locked";
    case AES67_PTP_LOCKING:  return "locking";
    case AES67_PTP_UNLOCKED: return "unlocked";
    default:                 return "unknown";
    }
}

/* ---- GET / : serve gzipped SPA ---- */

static esp_err_t handler_index(httpd_req_t *req)
{
    size_t len = index_html_gz_end - index_html_gz_start;
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_set_hdr(req, "Connection", "close");
    return httpd_resp_send(req, (const char *)index_html_gz_start, len);
}

/* ---- GET /favicon.ico : empty response to prevent 404 socket waste ---- */

static esp_err_t handler_favicon(httpd_req_t *req)
{
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_set_hdr(req, "Connection", "close");
    return httpd_resp_send(req, NULL, 0);
}

/* ---- GET /api/info ---- */

static esp_err_t handler_api_info(httpd_req_t *req)
{
    struct aes67_webui *ctx = (struct aes67_webui *)req->user_ctx;
    char *buf = malloc(JSON_BUF_SIZE);
    if (!buf) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    aes67_config_t cfg;
    aes67_node_get_config(ctx->node, &cfg);

    /* Uptime in seconds */
    int64_t uptime_us = esp_timer_get_time();
    uint32_t uptime_s = (uint32_t)(uptime_us / 1000000);

    /* Heap stats */
    uint32_t heap_free = esp_get_free_heap_size();
    uint32_t heap_min = esp_get_minimum_free_heap_size();

    /* Network info */
    uint32_t ip_u32 = 0;
    uint8_t mac[6] = {0};
    aes67_net_get_local_ip(&ip_u32);
    aes67_net_get_local_mac(mac);
    char ip_str[16];
    aes67_net_u32_to_ip(ip_u32, ip_str, sizeof(ip_str));

    int len = snprintf(buf, JSON_BUF_SIZE,
        "{"
        "\"name\":\"%s\","
        "\"version\":\"%d.%d.%d\","
        "\"ip\":\"%s\","
        "\"mac\":\"%02x:%02x:%02x:%02x:%02x:%02x\","
        "\"uptime\":%lu,"
        "\"heap_free\":%lu,"
        "\"heap_min\":%lu,"
        "\"sample_rate\":%lu,"
        "\"channels\":%u,"
        "\"codec\":\"%s\","
        "\"ptime_us\":%u"
        "}",
        cfg.node_name,
        AES67_VERSION_MAJOR, AES67_VERSION_MINOR, AES67_VERSION_PATCH,
        ip_str,
        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
        (unsigned long)uptime_s,
        (unsigned long)heap_free,
        (unsigned long)heap_min,
        (unsigned long)cfg.audio.sample_rate,
        cfg.audio.channels,
        codec_name(cfg.audio.codec),
        cfg.audio.packet_time_us);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, len);
    free(buf);
    return ESP_OK;
}

/* ---- GET /api/ptp ---- */

static esp_err_t handler_api_ptp(httpd_req_t *req)
{
    struct aes67_webui *ctx = (struct aes67_webui *)req->user_ctx;
    char *buf = malloc(JSON_BUF_SIZE);
    if (!buf) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    aes67_ptp_handle_t ptp = NULL;
    aes67_node_get_ptp(ctx->node, &ptp);

    aes67_ptp_status_t st = {0};
    if (ptp) {
        aes67_ptp_get_status(ptp, &st);
    }

    /* Format grandmaster ID as dash-separated hex */
    char gm_str[24];
    snprintf(gm_str, sizeof(gm_str),
             "%02X-%02X-%02X-%02X-%02X-%02X-%02X-%02X",
             st.grandmaster_id[0], st.grandmaster_id[1],
             st.grandmaster_id[2], st.grandmaster_id[3],
             st.grandmaster_id[4], st.grandmaster_id[5],
             st.grandmaster_id[6], st.grandmaster_id[7]);

    int len = snprintf(buf, JSON_BUF_SIZE,
        "{"
        "\"lock_state\":\"%s\","
        "\"grandmaster\":\"%s\","
        "\"domain\":%u,"
        "\"offset_ns\":%ld,"
        "\"jitter_ns\":%ld,"
        "\"path_delay_ns\":%ld"
        "}",
        ptp_lock_str(st.lock_state),
        gm_str,
        st.domain,
        (long)st.offset_ns,
        (long)st.jitter_ns,
        (long)st.path_delay_ns);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, len);
    free(buf);
    return ESP_OK;
}

/* ---- GET /api/sources ---- */

static esp_err_t handler_api_sources(httpd_req_t *req)
{
    struct aes67_webui *ctx = (struct aes67_webui *)req->user_ctx;
    char *buf = malloc(JSON_BUF_SIZE);
    if (!buf) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    aes67_session_handle_t sess = NULL;
    aes67_node_get_session(ctx->node, &sess);

    int pos = 0;
    buf[pos++] = '[';

    int count = sess ? aes67_session_get_source_count(sess) : 0;
    bool first = true;

    for (int i = 0; i < count && i < CONFIG_AES67_MAX_SOURCES; i++) {
        aes67_source_t src;
        if (aes67_session_get_source(sess, i, &src) != ESP_OK) {
            continue;
        }
        if (!src.enabled) {
            continue;
        }

        /* Get RTP stream stats if available */
        aes67_rtp_stream_status_t rtp_st = {0};
        if (src.rtp_stream) {
            aes67_rtp_stream_get_status(src.rtp_stream, &rtp_st);
        }

        if (!first) {
            buf[pos++] = ',';
        }
        first = false;

        pos += snprintf(buf + pos, JSON_BUF_SIZE - pos,
            "{"
            "\"id\":%u,"
            "\"name\":\"%s\","
            "\"channels\":%u,"
            "\"sample_rate\":%lu,"
            "\"codec\":\"%s\","
            "\"packets_sent\":%lu"
            "}",
            src.id, src.name,
            src.rtp_config.channels,
            (unsigned long)src.rtp_config.sample_rate,
            codec_name(src.rtp_config.codec),
            (unsigned long)rtp_st.packets_sent);

        if (pos >= JSON_BUF_SIZE - 128) {
            break;
        }
    }

    buf[pos++] = ']';
    buf[pos] = '\0';

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, pos);
    free(buf);
    return ESP_OK;
}

/* ---- GET /api/sinks ---- */

static esp_err_t handler_api_sinks(httpd_req_t *req)
{
    struct aes67_webui *ctx = (struct aes67_webui *)req->user_ctx;
    char *buf = malloc(JSON_BUF_SIZE);
    if (!buf) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    aes67_session_handle_t sess = NULL;
    aes67_node_get_session(ctx->node, &sess);

    int pos = 0;
    buf[pos++] = '[';

    int count = sess ? aes67_session_get_sink_count(sess) : 0;
    bool first = true;

    for (int i = 0; i < count && i < CONFIG_AES67_MAX_SINKS; i++) {
        aes67_sink_t sink;
        if (aes67_session_get_sink(sess, i, &sink) != ESP_OK) {
            continue;
        }
        if (!sink.enabled) {
            continue;
        }

        aes67_rtp_stream_status_t rtp_st = {0};
        if (sink.rtp_stream) {
            aes67_rtp_stream_get_status(sink.rtp_stream, &rtp_st);
        }

        if (!first) {
            buf[pos++] = ',';
        }
        first = false;

        pos += snprintf(buf + pos, JSON_BUF_SIZE - pos,
            "{"
            "\"id\":%u,"
            "\"name\":\"%s\","
            "\"channels\":%u,"
            "\"sample_rate\":%lu,"
            "\"codec\":\"%s\","
            "\"packets_received\":%lu,"
            "\"packets_lost\":%lu,"
            "\"jitter_us\":%ld,"
            "\"receiving\":%s"
            "}",
            sink.id, sink.name,
            sink.rtp_config.channels,
            (unsigned long)sink.rtp_config.sample_rate,
            codec_name(sink.rtp_config.codec),
            (unsigned long)rtp_st.packets_received,
            (unsigned long)rtp_st.packets_lost,
            (long)rtp_st.jitter_us,
            (rtp_st.status_flags & AES67_RTP_STATUS_RECEIVING) ? "true" : "false");

        if (pos >= JSON_BUF_SIZE - 128) {
            break;
        }
    }

    buf[pos++] = ']';
    buf[pos] = '\0';

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, pos);
    free(buf);
    return ESP_OK;
}

/* ---- GET /api/sap/remotes ---- */

static esp_err_t handler_api_sap_remotes(httpd_req_t *req)
{
    struct aes67_webui *ctx = (struct aes67_webui *)req->user_ctx;
    char *buf = malloc(JSON_BUF_SIZE);
    if (!buf) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    aes67_sap_handle_t sap = NULL;
    aes67_node_get_sap(ctx->node, &sap);

    int pos = 0;
    buf[pos++] = '[';

    int count = sap ? aes67_sap_get_remote_count(sap) : 0;
    bool first = true;

    for (int i = 0; i < count; i++) {
        aes67_sap_remote_source_t remote;
        if (aes67_sap_get_remote(sap, i, &remote) != ESP_OK) {
            continue;
        }

        if (!first) {
            buf[pos++] = ',';
        }
        first = false;

        char origin_str[16];
        aes67_net_u32_to_ip(remote.origin_ip, origin_str, sizeof(origin_str));

        pos += snprintf(buf + pos, JSON_BUF_SIZE - pos,
            "{"
            "\"name\":\"%s\","
            "\"origin\":\"%s\","
            "\"msg_id\":%u"
            "}",
            remote.name,
            origin_str,
            remote.msg_id);

        if (pos >= JSON_BUF_SIZE - 128) {
            break;
        }
    }

    buf[pos++] = ']';
    buf[pos] = '\0';

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, pos);
    free(buf);
    return ESP_OK;
}

/* ---- POST /api/sinks : subscribe to remote source ---- */

static esp_err_t handler_post_sink(httpd_req_t *req)
{
    struct aes67_webui *ctx = (struct aes67_webui *)req->user_ctx;

    /* Read request body (max 2KB for SDP) */
    int content_len = req->content_len;
    if (content_len <= 0 || content_len > JSON_BUF_SIZE) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid body length");
        return ESP_FAIL;
    }

    char *body = malloc(content_len + 1);
    if (!body) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    int received = httpd_req_recv(req, body, content_len);
    if (received != content_len) {
        free(body);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Incomplete body");
        return ESP_FAIL;
    }
    body[content_len] = '\0';

    /*
     * Minimal JSON parsing: extract "name" and "sdp" fields.
     * Expected format: {"name":"...","sdp":"v=0\r\n..."}
     */
    const char *name_key = "\"name\":\"";
    const char *sdp_key = "\"sdp\":\"";

    char name[64] = {0};
    char *sdp = NULL;

    /* Extract name */
    char *p = strstr(body, name_key);
    if (p) {
        p += strlen(name_key);
        char *end = strchr(p, '"');
        if (end) {
            size_t nlen = end - p;
            if (nlen >= sizeof(name)) nlen = sizeof(name) - 1;
            memcpy(name, p, nlen);
            name[nlen] = '\0';
        }
    }

    /* Extract SDP */
    p = strstr(body, sdp_key);
    if (p) {
        p += strlen(sdp_key);
        /* Find the closing quote (skip escaped quotes) */
        char *end = p;
        while (*end && !(*end == '"' && *(end - 1) != '\\')) {
            end++;
        }
        if (*end == '"') {
            size_t slen = end - p;
            sdp = malloc(slen + 1);
            if (sdp) {
                memcpy(sdp, p, slen);
                sdp[slen] = '\0';
                /* Unescape \r\n sequences */
                char *r = sdp, *w = sdp;
                while (*r) {
                    if (r[0] == '\\' && r[1] == 'r') {
                        *w++ = '\r';
                        r += 2;
                    } else if (r[0] == '\\' && r[1] == 'n') {
                        *w++ = '\n';
                        r += 2;
                    } else {
                        *w++ = *r++;
                    }
                }
                *w = '\0';
            }
        }
    }

    free(body);

    /* If no SDP provided, try to find it from SAP remotes by name */
    if (!sdp || !sdp[0]) {
        free(sdp);
        sdp = NULL;
        aes67_sap_handle_t sap = NULL;
        aes67_node_get_sap(ctx->node, &sap);
        if (sap && name[0]) {
            int rc = aes67_sap_get_remote_count(sap);
            for (int i = 0; i < rc; i++) {
                aes67_sap_remote_source_t remote;
                if (aes67_sap_get_remote(sap, i, &remote) == ESP_OK &&
                    strcmp(remote.name, name) == 0) {
                    sdp = strdup(remote.sdp);
                    break;
                }
            }
        }
    }

    if (!name[0] || !sdp || !sdp[0]) {
        free(sdp);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "Missing 'name' or 'sdp' field, and name not found in SAP");
        return ESP_FAIL;
    }

    /* Add the sink via session manager */
    aes67_session_handle_t sess = NULL;
    aes67_node_get_session(ctx->node, &sess);
    if (!sess) {
        free(sdp);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    uint8_t id = 0;
    esp_err_t ret = aes67_session_add_sink(sess, name, sdp, &id);
    free(sdp);

    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "add_sink failed: %s", esp_err_to_name(ret));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "Failed to add sink");
        return ESP_FAIL;
    }

    /* Respond with the assigned ID */
    char resp[32];
    int len = snprintf(resp, sizeof(resp), "{\"id\":%u}", id);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, len);

    ESP_LOGI(TAG, "Sink '%s' added with id=%u", name, id);
    return ESP_OK;
}

/* ---- DELETE /api/sinks/N : remove sink by ID ---- */

static esp_err_t handler_delete_sink(httpd_req_t *req)
{
    struct aes67_webui *ctx = (struct aes67_webui *)req->user_ctx;

    /* Parse sink ID from URI: /api/sinks/<id> */
    const char *uri = req->uri;
    const char *id_str = strrchr(uri, '/');
    if (!id_str || !*(id_str + 1)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing sink ID");
        return ESP_FAIL;
    }
    id_str++; /* skip the '/' */

    int id = atoi(id_str);
    if (id < 0 || id > 255) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid sink ID");
        return ESP_FAIL;
    }

    aes67_session_handle_t sess = NULL;
    aes67_node_get_session(ctx->node, &sess);
    if (!sess) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    esp_err_t ret = aes67_session_remove_sink(sess, (uint8_t)id);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "remove_sink(%d) failed: %s", id, esp_err_to_name(ret));
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Sink not found");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true}", 11);

    ESP_LOGI(TAG, "Sink id=%d removed", id);
    return ESP_OK;
}

/* ---- WebSocket ---- */

/*
 * Frames from the browser. The page never sends anything today; the server
 * handles ping/close itself. Reading the frame keeps the session healthy.
 */
static esp_err_t handler_ws(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        /* Not reached on ESP-IDF 5.x: the server answers the handshake and
         * does not call the handler for it. Clients are found when pushing. */
        return ESP_OK;
    }

    httpd_ws_frame_t ws_pkt;
    memset(&ws_pkt, 0, sizeof(ws_pkt));
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;
    uint8_t frame_buf[128];
    ws_pkt.payload = frame_buf;
    esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, sizeof(frame_buf));
    if (ret != ESP_OK) {
        ESP_LOGD(TAG, "WS recv failed: %s", esp_err_to_name(ret));
    }
    return ESP_OK;
}

/* Called by the server for every closed session, WebSocket or not. */
static void webui_close_fn(httpd_handle_t hd, int fd)
{
    (void)hd;
    ESP_LOGD(TAG, "session fd=%d closed", fd);
    close(fd);
}

/* ---- Status push ---- */

/*
 * Build the status frame. Runs in the server task (see ws_push_work), so
 * the heavy copy-outs use ctx->scratch instead of the stack.
 */
static int build_status_frame(struct aes67_webui *ctx, char *buf, size_t cap)
{
    struct webui_scratch *sc = ctx->scratch;

    int64_t uptime_us = esp_timer_get_time();
    uint32_t uptime_s = (uint32_t)(uptime_us / 1000000);
    uint32_t heap_free = esp_get_free_heap_size();

    aes67_ptp_handle_t ptp = NULL;
    aes67_node_get_ptp(ctx->node, &ptp);
    aes67_ptp_status_t ptp_st = {0};
    if (ptp) {
        aes67_ptp_get_status(ptp, &ptp_st);
    }

    char gm_str[24];
    snprintf(gm_str, sizeof(gm_str),
             "%02X-%02X-%02X-%02X-%02X-%02X-%02X-%02X",
             ptp_st.grandmaster_id[0], ptp_st.grandmaster_id[1],
             ptp_st.grandmaster_id[2], ptp_st.grandmaster_id[3],
             ptp_st.grandmaster_id[4], ptp_st.grandmaster_id[5],
             ptp_st.grandmaster_id[6], ptp_st.grandmaster_id[7]);

    /* We are GM if locked with zero offset and zero path delay
     * (a slave always has non-zero offset/delay from the master) */
    bool is_gm = (ptp_st.lock_state == AES67_PTP_LOCKED &&
                  ptp_st.offset_ns == 0 && ptp_st.path_delay_ns == 0);

    int pos = snprintf(buf, cap,
        "{"
        "\"t\":\"status\","
        "\"uptime\":%lu,"
        "\"heap_free\":%lu,"
        "\"ptp\":{\"lock\":\"%s\",\"gm\":\"%s\",\"is_gm\":%s,\"offset\":%ld,\"jitter\":%ld,\"delay\":%ld},",
        (unsigned long)uptime_s,
        (unsigned long)heap_free,
        ptp_lock_str(ptp_st.lock_state),
        gm_str,
        is_gm ? "true" : "false",
        (long)ptp_st.offset_ns,
        (long)ptp_st.jitter_ns,
        (long)ptp_st.path_delay_ns);

    aes67_session_handle_t sess = NULL;
    aes67_node_get_session(ctx->node, &sess);

    pos += snprintf(buf + pos, cap - pos, "\"sources\":[");
    if (sess) {
        bool first = true;
        int src_count = aes67_session_get_source_count(sess);
        for (int i = 0; i < src_count && i < CONFIG_AES67_MAX_SOURCES; i++) {
            aes67_source_t *src = &sc->src;
            if (aes67_session_get_source(sess, i, src) != ESP_OK || !src->enabled) {
                continue;
            }
            aes67_rtp_stream_status_t rtp_st = {0};
            if (src->rtp_stream) {
                aes67_rtp_stream_get_status(src->rtp_stream, &rtp_st);
            }
            if (!first) buf[pos++] = ',';
            first = false;
            pos += snprintf(buf + pos, cap - pos,
                "{\"id\":%u,\"name\":\"%s\",\"codec\":\"%s\",\"rate\":%lu,"
                "\"ch\":%u,\"ptime\":%u,\"tx\":%lu}",
                src->id, src->name, aes67_codec_to_str(src->rtp_config.codec),
                (unsigned long)src->rtp_config.sample_rate,
                src->rtp_config.channels,
                src->rtp_config.packet_time_us,
                (unsigned long)rtp_st.packets_sent);
            if (pos >= (int)cap - 256) break;
        }
    }
    pos += snprintf(buf + pos, cap - pos, "],");

    pos += snprintf(buf + pos, cap - pos, "\"sinks\":[");
    if (sess) {
        bool first = true;
        int sink_count = aes67_session_get_sink_count(sess);
        for (int i = 0; i < sink_count && i < CONFIG_AES67_MAX_SINKS; i++) {
            aes67_sink_t *sink = &sc->sink;
            if (aes67_session_get_sink(sess, i, sink) != ESP_OK || !sink->enabled) {
                continue;
            }
            aes67_rtp_stream_status_t rtp_st = {0};
            if (sink->rtp_stream) {
                aes67_rtp_stream_get_status(sink->rtp_stream, &rtp_st);
            }
            if (!first) buf[pos++] = ',';
            first = false;
            /* Use Ethernet hook counters for sink stats since the hook
             * intercepts RTP at L2 before the RTP engine sees packets */
            extern uint32_t s_hook_pkt_count;
            extern uint32_t s_hook_seq_lost;
            bool active = s_hook_pkt_count > 0;
            pos += snprintf(buf + pos, cap - pos,
                "{\"id\":%u,\"name\":\"%s\",\"codec\":\"%s\",\"rate\":%lu,"
                "\"ch\":%u,\"ptime\":%u,\"rx\":%lu,\"lost\":%lu,"
                "\"jitter\":%ld,\"active\":%s}",
                sink->id, sink->name, aes67_codec_to_str(sink->rtp_config.codec),
                (unsigned long)sink->rtp_config.sample_rate,
                sink->rtp_config.channels,
                sink->rtp_config.packet_time_us,
                (unsigned long)s_hook_pkt_count,
                (unsigned long)s_hook_seq_lost,
                (long)rtp_st.jitter_us,
                active ? "true" : "false");
            if (pos >= (int)cap - 256) break;
        }
    }
    pos += snprintf(buf + pos, cap - pos, "],");

    aes67_sap_handle_t sap = NULL;
    aes67_node_get_sap(ctx->node, &sap);

    pos += snprintf(buf + pos, cap - pos, "\"sap_remotes\":[");
    if (sap) {
        bool first = true;
        int rcount = aes67_sap_get_remote_count(sap);
        for (int i = 0; i < rcount; i++) {
            aes67_sap_remote_source_t *remote = &sc->remote;
            if (aes67_sap_get_remote(sap, i, remote) != ESP_OK) {
                continue;
            }
            char origin_str[16];
            aes67_net_u32_to_ip(remote->origin_ip, origin_str, sizeof(origin_str));
            if (!first) buf[pos++] = ',';
            first = false;
            const char *codec_str = "?";
            uint32_t rate = 0;
            uint8_t ch = 0;
            uint16_t ptime = 0;
            memset(&sc->sdp, 0, sizeof(sc->sdp));
            if (aes67_sdp_parse(remote->sdp, &sc->sdp) == ESP_OK) {
                codec_str = aes67_codec_to_str(sc->sdp.codec);
                rate = sc->sdp.sample_rate;
                ch = sc->sdp.channels;
                ptime = sc->sdp.ptime_us;
            }
            pos += snprintf(buf + pos, cap - pos,
                "{\"name\":\"%s\",\"origin\":\"%s\",\"codec\":\"%s\","
                "\"rate\":%lu,\"ch\":%u,\"ptime\":%u}",
                remote->name, origin_str, codec_str,
                (unsigned long)rate, ch, ptime);
            if (pos >= (int)cap - 128) break;
        }
    }
    pos += snprintf(buf + pos, cap - pos, "]");

    /* Ethernet hook stats (defined in main.c) */
    extern uint32_t s_hook_pkt_count;
    extern uint32_t s_hook_seq_lost;
    extern uint32_t s_hook_total_frames;
    pos += snprintf(buf + pos, cap - pos,
        ",\"eth\":{\"rtp\":%lu,\"total\":%lu,\"lost\":%lu}",
        (unsigned long)s_hook_pkt_count,
        (unsigned long)s_hook_total_frames,
        (unsigned long)s_hook_seq_lost);

    buf[pos++] = '}';
    buf[pos] = '\0';
    return pos;
}

/*
 * Runs in the server task via httpd_queue_work: find the WebSocket clients,
 * build one status frame if there is at least one, send it to all of them.
 * Socket access stays on the server's thread, as the httpd API requires.
 */
static void ws_push_work(void *arg)
{
    struct aes67_webui *ctx = (struct aes67_webui *)arg;
    if (!ctx->running || !ctx->httpd) {
        return;
    }

    int fds[CONFIG_LWIP_MAX_SOCKETS];
    size_t nfds = CONFIG_LWIP_MAX_SOCKETS;
    if (httpd_get_client_list(ctx->httpd, &nfds, fds) != ESP_OK) {
        return;
    }

    int clients = 0;
    for (size_t i = 0; i < nfds; i++) {
        if (httpd_ws_get_fd_info(ctx->httpd, fds[i]) == HTTPD_WS_CLIENT_WEBSOCKET) {
            fds[clients++] = fds[i];
        }
    }
    if (clients == 0) {
        return;
    }

    int len = build_status_frame(ctx, ctx->json, JSON_BUF_SIZE);
    if (len <= 0) {
        return;
    }

    httpd_ws_frame_t ws_pkt;
    memset(&ws_pkt, 0, sizeof(ws_pkt));
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;
    ws_pkt.payload = (uint8_t *)ctx->json;
    ws_pkt.len = len;

    for (int i = 0; i < clients; i++) {
        esp_err_t ret = httpd_ws_send_frame_async(ctx->httpd, fds[i], &ws_pkt);
        if (ret != ESP_OK) {
            ESP_LOGD(TAG, "WS send to fd=%d failed: %s", fds[i], esp_err_to_name(ret));
        }
    }
}

/* esp_timer callback: hand the push to the server task. */
static void push_timer_cb(void *arg)
{
    struct aes67_webui *ctx = (struct aes67_webui *)arg;
    if (ctx->running && ctx->httpd) {
        httpd_queue_work(ctx->httpd, ws_push_work, ctx);
    }
}

/* ---- Public API ---- */

esp_err_t aes67_webui_init(aes67_node_handle_t node, uint16_t port,
                            aes67_webui_handle_t *handle)
{
    if (!node || !handle) {
        return ESP_ERR_INVALID_ARG;
    }

    struct aes67_webui *ctx = calloc(1, sizeof(struct aes67_webui));
    if (!ctx) {
        ESP_LOGW(TAG, "Failed to allocate webui context");
        return ESP_ERR_NO_MEM;
    }

    ctx->node = node;
    ctx->port = port;
    ctx->httpd = NULL;
    ctx->running = false;
    ctx->scratch = calloc(1, sizeof(struct webui_scratch));
    ctx->json = malloc(JSON_BUF_SIZE);
    if (!ctx->scratch || !ctx->json) {
        free(ctx->scratch);
        free(ctx->json);
        free(ctx);
        return ESP_ERR_NO_MEM;
    }

    *handle = ctx;
    return ESP_OK;
}

esp_err_t aes67_webui_start(aes67_webui_handle_t handle)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }
    if (handle->running) {
        ESP_LOGW(TAG, "Web UI already running");
        return ESP_OK;
    }

    /* Configure and start the HTTP server */
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = handle->port;
    config.max_uri_handlers = 16;
    config.stack_size = 8192;
    config.lru_purge_enable = true;
    config.max_open_sockets = 7;
    config.keep_alive_enable = false;  /* Close HTTP conns immediately, free sockets for WS */
    config.core_id = 1;  /* Run HTTP server on core 1 (audio on core 0) */
    config.close_fn = webui_close_fn;

    esp_err_t ret = httpd_start(&handle->httpd, &config);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "httpd_start failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Register URI handlers -- all use the webui context as user_ctx */

    /* Serve the SPA */
    const httpd_uri_t uri_index = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = handler_index,
        .user_ctx = handle,
    };
    httpd_register_uri_handler(handle->httpd, &uri_index);

    /* Favicon -- prevent browser 404 and socket waste */
    const httpd_uri_t uri_favicon = {
        .uri = "/favicon.ico",
        .method = HTTP_GET,
        .handler = handler_favicon,
        .user_ctx = handle,
    };
    httpd_register_uri_handler(handle->httpd, &uri_favicon);

    /* WebSocket endpoint */
    const httpd_uri_t uri_ws = {
        .uri = "/ws",
        .method = HTTP_GET,
        .handler = handler_ws,
        .user_ctx = handle,
        .is_websocket = true,
    };
    httpd_register_uri_handler(handle->httpd, &uri_ws);

    /* REST: device info */
    const httpd_uri_t uri_info = {
        .uri = "/api/info",
        .method = HTTP_GET,
        .handler = handler_api_info,
        .user_ctx = handle,
    };
    httpd_register_uri_handler(handle->httpd, &uri_info);

    /* REST: PTP status */
    const httpd_uri_t uri_ptp = {
        .uri = "/api/ptp",
        .method = HTTP_GET,
        .handler = handler_api_ptp,
        .user_ctx = handle,
    };
    httpd_register_uri_handler(handle->httpd, &uri_ptp);

    /* REST: source streams */
    const httpd_uri_t uri_sources = {
        .uri = "/api/sources",
        .method = HTTP_GET,
        .handler = handler_api_sources,
        .user_ctx = handle,
    };
    httpd_register_uri_handler(handle->httpd, &uri_sources);

    /* REST: sink streams (GET) */
    const httpd_uri_t uri_sinks_get = {
        .uri = "/api/sinks",
        .method = HTTP_GET,
        .handler = handler_api_sinks,
        .user_ctx = handle,
    };
    httpd_register_uri_handler(handle->httpd, &uri_sinks_get);

    /* REST: SAP discovered remotes */
    const httpd_uri_t uri_sap_remotes = {
        .uri = "/api/sap/remotes",
        .method = HTTP_GET,
        .handler = handler_api_sap_remotes,
        .user_ctx = handle,
    };
    httpd_register_uri_handler(handle->httpd, &uri_sap_remotes);

    /* REST: add sink (POST) */
    const httpd_uri_t uri_sinks_post = {
        .uri = "/api/sinks",
        .method = HTTP_POST,
        .handler = handler_post_sink,
        .user_ctx = handle,
    };
    httpd_register_uri_handler(handle->httpd, &uri_sinks_post);

    /* REST: remove sink (DELETE /api/sinks/N) */
    const httpd_uri_t uri_sinks_delete = {
        .uri = "/api/sinks/*",
        .method = HTTP_DELETE,
        .handler = handler_delete_sink,
        .user_ctx = handle,
    };
    httpd_register_uri_handler(handle->httpd, &uri_sinks_delete);

    /* Periodic status push: the timer only queues work onto the server
     * task, which builds and sends the frame (see ws_push_work). */
    handle->running = true;
    const esp_timer_create_args_t targs = {
        .callback = push_timer_cb,
        .arg = handle,
        .name = "aes67_ws",
    };
    if (esp_timer_create(&targs, &handle->push_timer) == ESP_OK) {
        esp_timer_start_periodic(handle->push_timer,
                                 (uint64_t)CONFIG_AES67_WEBUI_PUSH_INTERVAL_MS * 1000);
    } else {
        ESP_LOGW(TAG, "Failed to create status push timer");
        handle->push_timer = NULL;
    }

    ESP_LOGI(TAG, "Web UI started on port %u", handle->port);
    return ESP_OK;
}

esp_err_t aes67_webui_stop(aes67_webui_handle_t handle)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!handle->running) {
        return ESP_OK;
    }

    handle->running = false;

    if (handle->push_timer) {
        esp_timer_stop(handle->push_timer);
        esp_timer_delete(handle->push_timer);
        handle->push_timer = NULL;
    }

    /* httpd_stop drains queued work before returning, so no push can run
     * on a stopped server. */
    if (handle->httpd) {
        httpd_stop(handle->httpd);
        handle->httpd = NULL;
    }

    ESP_LOGI(TAG, "Web UI stopped");
    return ESP_OK;
}
