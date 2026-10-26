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
#include <stdarg.h>
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

/* One buffer serves every REST response and the WebSocket status frame.
 * Sized for the maxima: 16 SAP remotes and 8 streams at about 200 bytes
 * each plus headers is under 5 KB; names are escaped later, which can
 * grow an entry, hence the margin. Overflow is handled, not assumed away. */
#define JSON_BUF_SIZE 8192

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

/* ---- Bounded JSON builder ----
 *
 * Every JSON document is appended into a jbuf. A write that does not fit
 * sets the overflow flag and leaves the buffer unchanged, so the length
 * never runs past the capacity (plain "pos += snprintf" would). Array
 * entries are written between jb_mark() and jb_rollback(): on overflow
 * the partial entry is removed and the array stays well formed. */

typedef struct {
    char   *buf;
    size_t  cap;
    size_t  len;
    bool    overflow;
} jbuf_t;

static void jb_init(jbuf_t *jb, char *buf, size_t cap)
{
    jb->buf = buf;
    jb->cap = cap;
    jb->len = 0;
    jb->overflow = false;
    if (cap) buf[0] = '\0';
}

static bool jb_printf(jbuf_t *jb, const char *fmt, ...)
{
    if (jb->overflow) return false;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(jb->buf + jb->len, jb->cap - jb->len, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= jb->cap - jb->len) {
        jb->buf[jb->len] = '\0';
        jb->overflow = true;
        return false;
    }
    jb->len += (size_t)n;
    return true;
}

static size_t jb_mark(const jbuf_t *jb)
{
    return jb->len;
}

/* Undo everything written since mark and clear the overflow flag so the
 * caller can still close the document. */
static void jb_rollback(jbuf_t *jb, size_t mark)
{
    jb->len = mark;
    jb->buf[mark] = '\0';
    jb->overflow = false;
}

/* ---- JSON fragments shared by REST and the WebSocket frame ---- */

static void json_ptp_object(jbuf_t *jb, const aes67_ptp_status_t *st)
{
    /* We are GM if locked with zero offset and zero path delay
     * (a slave always has non-zero offset/delay from the master) */
    bool is_gm = (st->lock_state == AES67_PTP_LOCKED &&
                  st->offset_ns == 0 && st->path_delay_ns == 0);
    jb_printf(jb,
        "{\"lock_state\":\"%s\","
        "\"grandmaster\":\"%02X-%02X-%02X-%02X-%02X-%02X-%02X-%02X\","
        "\"is_gm\":%s,\"domain\":%u,"
        "\"offset_ns\":%ld,\"jitter_ns\":%ld,\"path_delay_ns\":%ld}",
        ptp_lock_str(st->lock_state),
        st->grandmaster_id[0], st->grandmaster_id[1],
        st->grandmaster_id[2], st->grandmaster_id[3],
        st->grandmaster_id[4], st->grandmaster_id[5],
        st->grandmaster_id[6], st->grandmaster_id[7],
        is_gm ? "true" : "false",
        st->domain,
        (long)st->offset_ns, (long)st->jitter_ns, (long)st->path_delay_ns);
}

static void json_info_object(jbuf_t *jb, struct aes67_webui *ctx)
{
    aes67_config_t cfg;
    aes67_node_get_config(ctx->node, &cfg);

    uint32_t ip_u32 = 0;
    uint8_t mac[6] = {0};
    aes67_net_get_local_ip(&ip_u32);
    aes67_net_get_local_mac(mac);
    char ip_str[16];
    aes67_net_u32_to_ip(ip_u32, ip_str, sizeof(ip_str));

    jb_printf(jb,
        "{\"name\":\"%s\",\"version\":\"%d.%d.%d\",\"ip\":\"%s\","
        "\"mac\":\"%02x:%02x:%02x:%02x:%02x:%02x\","
        "\"uptime\":%lu,\"heap_free\":%lu,\"heap_min\":%lu,"
        "\"sample_rate\":%lu,\"channels\":%u,\"codec\":\"%s\",\"ptime_us\":%u}",
        cfg.node_name,
        AES67_VERSION_MAJOR, AES67_VERSION_MINOR, AES67_VERSION_PATCH,
        ip_str,
        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
        (unsigned long)(esp_timer_get_time() / 1000000),
        (unsigned long)esp_get_free_heap_size(),
        (unsigned long)esp_get_minimum_free_heap_size(),
        (unsigned long)cfg.audio.sample_rate,
        cfg.audio.channels,
        codec_name(cfg.audio.codec),
        cfg.audio.packet_time_us);
}

/* Append "[entry,entry,...]" of the enabled sources. Returns false if the
 * array had to be cut short. */
static bool json_sources_array(jbuf_t *jb, struct aes67_webui *ctx)
{
    aes67_session_handle_t sess = NULL;
    aes67_node_get_session(ctx->node, &sess);
    aes67_source_t *src = &ctx->scratch->src;
    bool complete = true, first = true;

    jb_printf(jb, "[");
    int count = sess ? aes67_session_get_source_count(sess) : 0;
    for (int i = 0; i < count && i < CONFIG_AES67_MAX_SOURCES; i++) {
        if (aes67_session_get_source(sess, i, src) != ESP_OK || !src->enabled) {
            continue;
        }
        aes67_rtp_stream_status_t rtp_st = {0};
        if (src->rtp_stream) {
            aes67_rtp_stream_get_status(src->rtp_stream, &rtp_st);
        }
        size_t mark = jb_mark(jb);
        bool ok = jb_printf(jb,
            "%s{\"id\":%u,\"name\":\"%s\",\"codec\":\"%s\",\"sample_rate\":%lu,"
            "\"channels\":%u,\"ptime_us\":%u,\"packets_sent\":%lu}",
            first ? "" : ",",
            src->id, src->name, codec_name(src->rtp_config.codec),
            (unsigned long)src->rtp_config.sample_rate,
            src->rtp_config.channels,
            src->rtp_config.packet_time_us,
            (unsigned long)rtp_st.packets_sent);
        if (!ok) {
            jb_rollback(jb, mark);
            complete = false;
            break;
        }
        first = false;
    }
    jb_printf(jb, "]");
    return complete;
}

static bool json_sinks_array(jbuf_t *jb, struct aes67_webui *ctx)
{
    aes67_session_handle_t sess = NULL;
    aes67_node_get_session(ctx->node, &sess);
    aes67_sink_t *sink = &ctx->scratch->sink;
    bool complete = true, first = true;

    jb_printf(jb, "[");
    int count = sess ? aes67_session_get_sink_count(sess) : 0;
    for (int i = 0; i < count && i < CONFIG_AES67_MAX_SINKS; i++) {
        if (aes67_session_get_sink(sess, i, sink) != ESP_OK || !sink->enabled) {
            continue;
        }
        aes67_rtp_stream_status_t rtp_st = {0};
        if (sink->rtp_stream) {
            aes67_rtp_stream_get_status(sink->rtp_stream, &rtp_st);
        }
        /* The Ethernet hook intercepts RTP at L2 before the engine sees
         * packets, so its counters are the ones that move (defined in the
         * example's main.c for now). */
        extern uint32_t s_hook_pkt_count;
        extern uint32_t s_hook_seq_lost;
        size_t mark = jb_mark(jb);
        bool ok = jb_printf(jb,
            "%s{\"id\":%u,\"name\":\"%s\",\"codec\":\"%s\",\"sample_rate\":%lu,"
            "\"channels\":%u,\"ptime_us\":%u,\"packets_received\":%lu,"
            "\"packets_lost\":%lu,\"jitter_us\":%ld,\"receiving\":%s}",
            first ? "" : ",",
            sink->id, sink->name, codec_name(sink->rtp_config.codec),
            (unsigned long)sink->rtp_config.sample_rate,
            sink->rtp_config.channels,
            sink->rtp_config.packet_time_us,
            (unsigned long)s_hook_pkt_count,
            (unsigned long)s_hook_seq_lost,
            (long)rtp_st.jitter_us,
            s_hook_pkt_count > 0 ? "true" : "false");
        if (!ok) {
            jb_rollback(jb, mark);
            complete = false;
            break;
        }
        first = false;
    }
    jb_printf(jb, "]");
    return complete;
}

static bool json_remotes_array(jbuf_t *jb, struct aes67_webui *ctx)
{
    aes67_sap_handle_t sap = NULL;
    aes67_node_get_sap(ctx->node, &sap);
    aes67_sap_remote_source_t *remote = &ctx->scratch->remote;
    aes67_sdp_t *sdp = &ctx->scratch->sdp;
    bool complete = true, first = true;

    jb_printf(jb, "[");
    int count = sap ? aes67_sap_get_remote_count(sap) : 0;
    for (int i = 0; i < count; i++) {
        if (aes67_sap_get_remote(sap, i, remote) != ESP_OK) {
            continue;
        }
        char origin_str[16];
        aes67_net_u32_to_ip(remote->origin_ip, origin_str, sizeof(origin_str));
        const char *codec_str = "?";
        uint32_t rate = 0;
        uint8_t ch = 0;
        uint16_t ptime = 0;
        memset(sdp, 0, sizeof(*sdp));
        if (aes67_sdp_parse(remote->sdp, sdp) == ESP_OK) {
            codec_str = codec_name(sdp->codec);
            rate = sdp->sample_rate;
            ch = sdp->channels;
            ptime = sdp->ptime_us;
        }
        size_t mark = jb_mark(jb);
        bool ok = jb_printf(jb,
            "%s{\"name\":\"%s\",\"origin\":\"%s\",\"msg_id\":%u,"
            "\"codec\":\"%s\",\"sample_rate\":%lu,\"channels\":%u,\"ptime_us\":%u}",
            first ? "" : ",",
            remote->name, origin_str, remote->msg_id,
            codec_str, (unsigned long)rate, ch, ptime);
        if (!ok) {
            jb_rollback(jb, mark);
            complete = false;
            break;
        }
        first = false;
    }
    jb_printf(jb, "]");
    return complete;
}

/* Send a jbuf as the JSON response; 500 if it overflowed. */
static esp_err_t send_json(httpd_req_t *req, const jbuf_t *jb)
{
    if (jb->overflow) {
        ESP_LOGW(TAG, "JSON response for %s exceeds %u bytes", req->uri, (unsigned)jb->cap);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, jb->buf, jb->len);
}

/* ---- GET /api/info ---- */

static esp_err_t handler_api_info(httpd_req_t *req)
{
    struct aes67_webui *ctx = (struct aes67_webui *)req->user_ctx;
    jbuf_t jb;
    jb_init(&jb, ctx->json, JSON_BUF_SIZE);
    json_info_object(&jb, ctx);
    return send_json(req, &jb);
}

/* ---- GET /api/ptp ---- */

static esp_err_t handler_api_ptp(httpd_req_t *req)
{
    struct aes67_webui *ctx = (struct aes67_webui *)req->user_ctx;
    aes67_ptp_handle_t ptp = NULL;
    aes67_node_get_ptp(ctx->node, &ptp);
    aes67_ptp_status_t st = {0};
    if (ptp) {
        aes67_ptp_get_status(ptp, &st);
    }
    jbuf_t jb;
    jb_init(&jb, ctx->json, JSON_BUF_SIZE);
    json_ptp_object(&jb, &st);
    return send_json(req, &jb);
}

/* ---- GET /api/sources, /api/sinks, /api/sap/remotes ---- */

static esp_err_t handler_api_sources(httpd_req_t *req)
{
    struct aes67_webui *ctx = (struct aes67_webui *)req->user_ctx;
    jbuf_t jb;
    jb_init(&jb, ctx->json, JSON_BUF_SIZE);
    json_sources_array(&jb, ctx);
    return send_json(req, &jb);
}

static esp_err_t handler_api_sinks(httpd_req_t *req)
{
    struct aes67_webui *ctx = (struct aes67_webui *)req->user_ctx;
    jbuf_t jb;
    jb_init(&jb, ctx->json, JSON_BUF_SIZE);
    json_sinks_array(&jb, ctx);
    return send_json(req, &jb);
}

static esp_err_t handler_api_sap_remotes(httpd_req_t *req)
{
    struct aes67_webui *ctx = (struct aes67_webui *)req->user_ctx;
    jbuf_t jb;
    jb_init(&jb, ctx->json, JSON_BUF_SIZE);
    json_remotes_array(&jb, ctx);
    return send_json(req, &jb);
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
 * Build the status frame from the shared fragments. Runs in the server
 * task (see ws_push_work). Returns the length, or 0 if even the fixed part
 * did not fit. Arrays cut short are reported as "truncated":true.
 */
static int build_status_frame(struct aes67_webui *ctx, char *buf, size_t cap)
{
    jbuf_t jb;
    jb_init(&jb, buf, cap);

    aes67_ptp_handle_t ptp = NULL;
    aes67_node_get_ptp(ctx->node, &ptp);
    aes67_ptp_status_t ptp_st = {0};
    if (ptp) {
        aes67_ptp_get_status(ptp, &ptp_st);
    }

    jb_printf(&jb, "{\"t\":\"status\",\"uptime\":%lu,\"heap_free\":%lu,\"ptp\":",
              (unsigned long)(esp_timer_get_time() / 1000000),
              (unsigned long)esp_get_free_heap_size());
    json_ptp_object(&jb, &ptp_st);

    bool complete = true;
    jb_printf(&jb, ",\"sources\":");
    complete &= json_sources_array(&jb, ctx);
    jb_printf(&jb, ",\"sinks\":");
    complete &= json_sinks_array(&jb, ctx);
    jb_printf(&jb, ",\"sap_remotes\":");
    complete &= json_remotes_array(&jb, ctx);

    /* Ethernet hook stats (defined in the example's main.c for now) */
    extern uint32_t s_hook_pkt_count;
    extern uint32_t s_hook_seq_lost;
    extern uint32_t s_hook_total_frames;
    jb_printf(&jb, ",\"eth\":{\"rtp\":%lu,\"total\":%lu,\"lost\":%lu},\"truncated\":%s}",
              (unsigned long)s_hook_pkt_count,
              (unsigned long)s_hook_total_frames,
              (unsigned long)s_hook_seq_lost,
              complete ? "false" : "true");

    if (jb.overflow) {
        ESP_LOGW(TAG, "status frame exceeds %u bytes", (unsigned)cap);
        return 0;
    }
    return (int)jb.len;
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
    ctx->json = heap_caps_malloc(JSON_BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ctx->json) {
        ctx->json = malloc(JSON_BUF_SIZE);
    }
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
