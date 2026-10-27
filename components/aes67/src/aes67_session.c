/*
 * SPDX-FileCopyrightText: 2026 DatanoiseTV
 *
 * SPDX-License-Identifier: Apache-2.0
 */
/*
 * AES67 session manager implementation.
 *
 * Manages the lifecycle of source and sink streams, including SDP
 * generation, SAP announcements, and RTP stream creation/teardown.
 */

#include <string.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_event.h"
#include "aes67.h"

#include "aes67_session.h"
#include "aes67_rtp.h"
#include "aes67_sdp.h"
#include "aes67_sap.h"
#include "aes67_ptp.h"
#include "aes67_audio.h"
#include "aes67_net.h"
#include "aes67_config.h"

static const char *TAG = "aes67_session";

/* Session manager internal state */
struct aes67_session_mgr {
    aes67_rtp_engine_handle_t rtp;
    aes67_ptp_handle_t ptp;
    aes67_sap_handle_t sap;
    aes67_audio_handle_t audio;
    aes67_config_t config;

    aes67_source_t sources[CONFIG_AES67_MAX_SOURCES];
    aes67_sink_t sinks[CONFIG_AES67_MAX_SINKS];
    uint8_t source_count;
    uint8_t sink_count;

    uint32_t next_mcast_ip;       /* auto-incrementing multicast address */
    uint32_t session_id_counter;

    SemaphoreHandle_t lock;
};

/* Reference clock attributes from the current PTP state: the grandmaster
 * identity and domain when locked, "traceable" when that grandmaster is
 * itself locked to a primary reference (clockClass 6) or in holdover from
 * one (7). Nothing is claimed while not locked. */
static void fill_refclk(struct aes67_session_mgr *mgr, aes67_sdp_t *sdp)
{
    sdp->has_ptp_refclk = false;
    sdp->ptp_traceable = false;
    sdp->ptp_domain = mgr->config.ptp.domain;
    memset(sdp->ptp_grandmaster_id, 0, sizeof(sdp->ptp_grandmaster_id));

    aes67_ptp_status_t st;
    if (aes67_ptp_get_status(mgr->ptp, &st) != ESP_OK || st.lock_state != AES67_PTP_LOCKED) {
        return;
    }
    bool gm_valid = false;
    for (int i = 0; i < 8; i++) {
        if (st.grandmaster_id[i] != 0) { gm_valid = true; break; }
    }
    if (!gm_valid) {
        return;
    }
    memcpy(sdp->ptp_grandmaster_id, st.grandmaster_id, 8);
    sdp->has_ptp_refclk = true;
    sdp->ptp_domain = st.domain;
    sdp->ptp_traceable = st.master_clock_class <= 7;
}

/* CRC-16 of the SDP text, the SAP message id hash (Linux daemon convention) */
static uint16_t sdp_msg_id(const char *sdp_text, int len)
{
    uint16_t crc = 0xFFFF;
    for (int i = 0; i < len; i++) {
        crc ^= (uint8_t)sdp_text[i];
        for (int b = 0; b < 8; b++) {
            if (crc & 1) crc = (crc >> 1) ^ 0xA001;
            else crc >>= 1;
        }
    }
    return crc;
}

/* Generate a deterministic SSRC from MAC address and stream ID */
static uint32_t generate_ssrc(uint8_t stream_id)
{
    uint8_t mac[6];
    if (aes67_net_get_local_mac(mac) != ESP_OK) {
        /* Fallback: use stream ID only */
        return (uint32_t)stream_id | 0xAE670000;
    }

    /* FNV-1a 32-bit hash of MAC + stream ID */
    uint32_t hash = 2166136261u;
    for (int i = 0; i < 6; i++) {
        hash ^= mac[i];
        hash *= 16777619u;
    }
    hash ^= stream_id;
    hash *= 16777619u;

    /* Avoid reserved SSRC value 0 */
    if (hash == 0) {
        hash = 1;
    }
    return hash;
}

/* A sink started or stopped receiving (engine timer task): post the event
 * with the sink id, which applications and the web page key on */
static void sink_rx_state_cb(aes67_rtp_stream_handle_t stream, bool active, void *user_data)
{
    struct aes67_session_mgr *mgr = user_data;
    if (xSemaphoreTake(mgr->lock, pdMS_TO_TICKS(20)) != pdTRUE) {
        return;                     /* busy; the watcher reports again next time */
    }
    int found = -1;
    for (int i = 0; i < CONFIG_AES67_MAX_SINKS; i++) {
        if (mgr->sinks[i].enabled && mgr->sinks[i].rtp_stream == stream) {
            found = i;
            break;
        }
    }
    xSemaphoreGive(mgr->lock);
    if (found < 0) {
        return;
    }
    uint8_t id = (uint8_t)found;
    ESP_LOGI(TAG, "sink %u %s", id, active ? "receiving" : "receive timeout");
    esp_event_post(AES67_EVENT, active ? AES67_EVENT_SINK_RX_ACTIVE : AES67_EVENT_SINK_RX_TIMEOUT,
                   &id, sizeof(id), 0);
}

esp_err_t aes67_session_init(aes67_rtp_engine_handle_t rtp,
                             aes67_ptp_handle_t ptp,
                             aes67_sap_handle_t sap,
                             aes67_audio_handle_t audio,
                             const aes67_config_t *config,
                             aes67_session_handle_t *handle)
{
    if (!rtp || !ptp || !config || !handle) {
        return ESP_ERR_INVALID_ARG;
    }

    struct aes67_session_mgr *mgr = calloc(1, sizeof(*mgr));
    if (!mgr) {
        return ESP_ERR_NO_MEM;
    }

    mgr->rtp = rtp;
    mgr->ptp = ptp;
    mgr->sap = sap;
    mgr->audio = audio;
    memcpy(&mgr->config, config, sizeof(aes67_config_t));

    mgr->source_count = 0;
    mgr->sink_count = 0;
    mgr->session_id_counter = 1;

    /* Parse the multicast base address for auto-incrementing */
    mgr->next_mcast_ip = aes67_net_ip_to_u32(config->net.rtp_mcast_base);

    mgr->lock = xSemaphoreCreateMutex();
    if (!mgr->lock) {
        free(mgr);
        return ESP_ERR_NO_MEM;
    }

    /* Clear all slots */
    memset(mgr->sources, 0, sizeof(mgr->sources));
    memset(mgr->sinks, 0, sizeof(mgr->sinks));

    aes67_rtp_engine_set_rx_state_cb(mgr->rtp, sink_rx_state_cb, mgr);
    *handle = mgr;
    ESP_LOGI(TAG, "session manager initialized");
    return ESP_OK;
}

esp_err_t aes67_session_add_source(aes67_session_handle_t handle,
                                   const char *name,
                                   uint8_t channels,
                                   aes67_codec_t codec,
                                   uint16_t packet_time_us,
                                   uint8_t *id)
{
    if (!handle || !name || !id) {
        return ESP_ERR_INVALID_ARG;
    }

    struct aes67_session_mgr *mgr = handle;

    if (xSemaphoreTake(mgr->lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    /* Find a free slot */
    int slot = -1;
    for (int i = 0; i < CONFIG_AES67_MAX_SOURCES; i++) {
        if (!mgr->sources[i].enabled) {
            slot = i;
            break;
        }
    }

    if (slot < 0) {
        xSemaphoreGive(mgr->lock);
        ESP_LOGE(TAG, "no free source slots");
        return ESP_ERR_NO_MEM;
    }

    /* Use defaults where not specified */
    if (channels == 0) {
        channels = mgr->config.audio.channels;
    }
    if ((int)codec < 0) {
        codec = mgr->config.audio.codec;
    }

    uint8_t word_length = aes67_codec_word_length(codec);
    uint32_t sample_rate = mgr->config.audio.sample_rate;
    uint16_t packet_time = packet_time_us ? packet_time_us : mgr->config.audio.packet_time_us;
    if (packet_time < 125 || packet_time > 4000) {
        xSemaphoreGive(mgr->lock);
        ESP_LOGE(TAG, "packet time %u us out of range (125..4000)", packet_time);
        return ESP_ERR_INVALID_ARG;
    }
    uint16_t port = mgr->config.net.rtp_port;

    /* Compute multicast IP: base address + source index (network order).
     * The base IP is already in network byte order from aes67_net_ip_to_u32,
     * so we increment the host-order representation. */
    uint32_t mcast_ip = ntohl(mgr->next_mcast_ip) + (uint32_t)slot;
    mcast_ip = htonl(mcast_ip);

    uint32_t ssrc = generate_ssrc((uint8_t)slot);

    /* Build RTP stream config */
    aes67_rtp_stream_config_t rtp_cfg;
    memset(&rtp_cfg, 0, sizeof(rtp_cfg));
    rtp_cfg.direction = AES67_STREAM_SOURCE;
    rtp_cfg.dest_ip = mcast_ip;
    rtp_cfg.dest_port = port;
    rtp_cfg.src_port = port;
    rtp_cfg.payload_type = 98;
    rtp_cfg.ssrc = ssrc;
    rtp_cfg.sample_rate = sample_rate;
    rtp_cfg.channels = channels;
    rtp_cfg.word_length = word_length;
    rtp_cfg.codec = codec;
    rtp_cfg.packet_time_us = packet_time;
    rtp_cfg.playout_delay_us = 0;
    rtp_cfg.dscp = mgr->config.net.dscp;
    rtp_cfg.ttl = mgr->config.net.ttl;

    /* Default 1:1 channel routing */
    for (int i = 0; i < channels && i < CONFIG_AES67_MAX_CHANNELS_PER_STREAM; i++) {
        rtp_cfg.routing[i] = (uint8_t)i;
    }

    /* Build SDP */
    aes67_sdp_t sdp;
    memset(&sdp, 0, sizeof(sdp));

    uint32_t local_ip = 0;
    aes67_net_get_local_ip(&local_ip);

    strncpy(sdp.origin_username, "-", sizeof(sdp.origin_username) - 1);
    sdp.session_id = mgr->session_id_counter++;
    sdp.session_version = 1;
    sdp.origin_ip = local_ip;

    strncpy(sdp.session_name, name, sizeof(sdp.session_name) - 1);
    sdp.connection_ip = mcast_ip;
    sdp.ttl = mgr->config.net.ttl;
    sdp.port = port;
    sdp.payload_type = 98;
    sdp.codec = codec;
    sdp.sample_rate = sample_rate;
    sdp.channels = channels;
    sdp.word_length = word_length;
    sdp.ptime_us = packet_time;

    /* Reference clock as PTP stands now; aes67_session_refresh_sdp()
     * regenerates and re-announces when the lock state changes */
    sdp.is_source = true;  /* This is a TX source: a=sendonly */
    fill_refclk(mgr, &sdp);

    /* Media clock offset */
    sdp.has_mediaclk = true;
    sdp.mediaclk_offset = 0;

    /* Generate SDP text into a temporary buffer */
    char sdp_text[AES67_SDP_MAX_LEN];
    int sdp_len = aes67_sdp_generate(&sdp, sdp_text, sizeof(sdp_text));
    if (sdp_len < 0) {
        xSemaphoreGive(mgr->lock);
        ESP_LOGE(TAG, "failed to generate SDP for source '%s'", name);
        return ESP_FAIL;
    }

    /* Add the RTP stream */
    aes67_rtp_stream_handle_t rtp_stream = NULL;
    esp_err_t err = aes67_rtp_stream_add(mgr->rtp, &rtp_cfg, &rtp_stream);
    if (err != ESP_OK) {
        xSemaphoreGive(mgr->lock);
        ESP_LOGE(TAG, "failed to add RTP stream for source '%s': %s",
                 name, esp_err_to_name(err));
        return err;
    }

    /* The SAP message id: a hash of the first SDP text, kept for the
     * source's lifetime so receivers see updates as the same session */
    uint16_t msg_crc = sdp_msg_id(sdp_text, sdp_len);

    /* SAP announcement if enabled */
    if (mgr->config.sap_enabled && mgr->sap) {
        err = aes67_sap_announce(mgr->sap, msg_crc,
                                 local_ip, sdp_text);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "SAP announce failed for source '%s': %s",
                     name, esp_err_to_name(err));
            /* Non-fatal: stream still works without SAP */
        }
    }

    /* Store in source slot */
    aes67_source_t *src = &mgr->sources[slot];
    src->id = (uint8_t)slot;
    strncpy(src->name, name, sizeof(src->name) - 1);
    src->name[sizeof(src->name) - 1] = '\0';
    memcpy(&src->rtp_config, &rtp_cfg, sizeof(rtp_cfg));
    memcpy(&src->sdp, &sdp, sizeof(sdp));
    src->rtp_stream = rtp_stream;
    src->sap_msg_id = msg_crc;
    src->enabled = true;

    mgr->source_count++;
    *id = (uint8_t)slot;

    xSemaphoreGive(mgr->lock);

    ESP_LOGI(TAG, "source '%s' added (id=%d, channels=%d, ssrc=0x%08lx)",
             name, slot, channels, (unsigned long)ssrc);
    esp_event_post(AES67_EVENT, AES67_EVENT_SOURCE_ADDED, id, sizeof(*id), 0);
    ESP_LOGI(TAG, "SDP:\n%s", sdp_text);
    return ESP_OK;
}

esp_err_t aes67_session_add_sink(aes67_session_handle_t handle,
                                 const char *name,
                                 const char *sdp_text,
                                 uint8_t *id)
{
    if (!handle || !name || !sdp_text || !id) {
        return ESP_ERR_INVALID_ARG;
    }

    struct aes67_session_mgr *mgr = handle;

    /* Parse the remote SDP */
    aes67_sdp_t sdp;
    memset(&sdp, 0, sizeof(sdp));
    esp_err_t err = aes67_sdp_parse(sdp_text, &sdp);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to parse SDP for sink '%s'", name);
        return err;
    }

    if (xSemaphoreTake(mgr->lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    /* Find a free slot */
    int slot = -1;
    for (int i = 0; i < CONFIG_AES67_MAX_SINKS; i++) {
        if (!mgr->sinks[i].enabled) {
            slot = i;
            break;
        }
    }

    if (slot < 0) {
        xSemaphoreGive(mgr->lock);
        ESP_LOGE(TAG, "no free sink slots");
        return ESP_ERR_NO_MEM;
    }

    /* Build RTP stream config from parsed SDP */
    aes67_rtp_stream_config_t rtp_cfg;
    memset(&rtp_cfg, 0, sizeof(rtp_cfg));
    rtp_cfg.direction = AES67_STREAM_SINK;
    rtp_cfg.dest_ip = sdp.connection_ip;
    rtp_cfg.dest_port = sdp.port;
    rtp_cfg.src_port = sdp.port;
    rtp_cfg.payload_type = sdp.payload_type;
    rtp_cfg.ssrc = 0;  /* Will match any SSRC from remote source */
    rtp_cfg.sample_rate = sdp.sample_rate;
    rtp_cfg.channels = sdp.channels;
    rtp_cfg.word_length = sdp.word_length;
    rtp_cfg.codec = sdp.codec;
    rtp_cfg.packet_time_us = sdp.ptime_us;
    if (sdp.framecount && sdp.sample_rate) {
        /* The SDP names the frame count: derive the packet time from it so
         * the stream parameters reproduce exactly that count */
        rtp_cfg.packet_time_us = (uint16_t)(((uint64_t)sdp.framecount * 1000000ULL +
                                             sdp.sample_rate / 2) / sdp.sample_rate);
    }
    rtp_cfg.playout_delay_us = (uint32_t)mgr->config.jitter_buffer_ms * 1000;
    rtp_cfg.dscp = mgr->config.net.dscp;
    rtp_cfg.ttl = mgr->config.net.ttl;

    /* Default 1:1 channel routing */
    for (int i = 0; i < sdp.channels && i < CONFIG_AES67_MAX_CHANNELS_PER_STREAM; i++) {
        rtp_cfg.routing[i] = (uint8_t)i;
    }

    /* Add the RTP stream */
    aes67_rtp_stream_handle_t rtp_stream = NULL;
    err = aes67_rtp_stream_add(mgr->rtp, &rtp_cfg, &rtp_stream);
    if (err != ESP_OK) {
        xSemaphoreGive(mgr->lock);
        ESP_LOGE(TAG, "failed to add RTP stream for sink '%s': %s",
                 name, esp_err_to_name(err));
        return err;
    }

    /* Store in sink slot */
    aes67_sink_t *snk = &mgr->sinks[slot];
    snk->id = (uint8_t)slot;
    strncpy(snk->name, name, sizeof(snk->name) - 1);
    snk->name[sizeof(snk->name) - 1] = '\0';
    memcpy(&snk->rtp_config, &rtp_cfg, sizeof(rtp_cfg));
    memcpy(&snk->sdp, &sdp, sizeof(sdp));
    snk->rtp_stream = rtp_stream;
    snk->enabled = true;

    /* Store original SDP text for reference */
    strncpy(snk->remote_sdp, sdp_text, AES67_SDP_MAX_LEN - 1);
    snk->remote_sdp[AES67_SDP_MAX_LEN - 1] = '\0';

    mgr->sink_count++;
    *id = (uint8_t)slot;

    xSemaphoreGive(mgr->lock);

    char group_str[16];
    aes67_net_u32_to_ip(sdp.connection_ip, group_str, sizeof(group_str));
    ESP_LOGI(TAG, "sink '%s' added (id=%d, %s/%luHz/%dch, wl=%d, ptime=%uus, "
             "mcast=%s:%u, pt=%u)",
             name, slot,
             aes67_codec_to_str(sdp.codec),
             (unsigned long)sdp.sample_rate, sdp.channels, sdp.word_length,
             rtp_cfg.packet_time_us,
             group_str, sdp.port, sdp.payload_type);
    esp_event_post(AES67_EVENT, AES67_EVENT_SINK_ADDED, id, sizeof(*id), 0);
    ESP_LOGI(TAG, "sink SDP:\n%s", sdp_text);
    return ESP_OK;
}

esp_err_t aes67_session_remove_source(aes67_session_handle_t handle, uint8_t id)
{
    if (!handle || id >= CONFIG_AES67_MAX_SOURCES) {
        return ESP_ERR_INVALID_ARG;
    }

    struct aes67_session_mgr *mgr = handle;

    if (xSemaphoreTake(mgr->lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    aes67_source_t *src = &mgr->sources[id];
    if (!src->enabled) {
        xSemaphoreGive(mgr->lock);
        return ESP_ERR_NOT_FOUND;
    }

    /* Send SAP deletion if enabled */
    if (mgr->config.sap_enabled && mgr->sap) {
        uint32_t local_ip = 0;
        aes67_net_get_local_ip(&local_ip);

        char sdp_text[AES67_SDP_MAX_LEN];
        int len = aes67_sdp_generate(&src->sdp, sdp_text, sizeof(sdp_text));
        if (len > 0) {
            esp_err_t err = aes67_sap_delete(mgr->sap,
                                             src->sap_msg_id,
                                             local_ip, sdp_text);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "SAP delete failed for source %d: %s",
                         id, esp_err_to_name(err));
            }
        }
    }

    /* Remove RTP stream */
    if (src->rtp_stream) {
        aes67_rtp_stream_remove(mgr->rtp, src->rtp_stream);
    }

    /* Clear slot */
    char name_copy[64];
    strlcpy(name_copy, src->name, sizeof(name_copy));
    memset(src, 0, sizeof(*src));
    mgr->source_count--;

    xSemaphoreGive(mgr->lock);

    ESP_LOGI(TAG, "source '%s' removed (id=%d)", name_copy, id);
    esp_event_post(AES67_EVENT, AES67_EVENT_SOURCE_REMOVED, &id, sizeof(id), 0);
    return ESP_OK;
}

esp_err_t aes67_session_remove_sink(aes67_session_handle_t handle, uint8_t id)
{
    if (!handle || id >= CONFIG_AES67_MAX_SINKS) {
        return ESP_ERR_INVALID_ARG;
    }

    struct aes67_session_mgr *mgr = handle;

    if (xSemaphoreTake(mgr->lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    aes67_sink_t *snk = &mgr->sinks[id];
    if (!snk->enabled) {
        xSemaphoreGive(mgr->lock);
        return ESP_ERR_NOT_FOUND;
    }

    /* Remove RTP stream */
    if (snk->rtp_stream) {
        aes67_rtp_stream_remove(mgr->rtp, snk->rtp_stream);
    }

    /* Clear slot */
    char name_copy[64];
    strlcpy(name_copy, snk->name, sizeof(name_copy));
    memset(snk, 0, sizeof(*snk));
    mgr->sink_count--;

    xSemaphoreGive(mgr->lock);

    ESP_LOGI(TAG, "sink '%s' removed (id=%d)", name_copy, id);
    esp_event_post(AES67_EVENT, AES67_EVENT_SINK_REMOVED, &id, sizeof(id), 0);
    return ESP_OK;
}

esp_err_t aes67_session_get_source_status(aes67_session_handle_t handle, uint8_t id,
                                          aes67_rtp_stream_status_t *status)
{
    if (!handle || !status || id >= CONFIG_AES67_MAX_SOURCES) {
        return ESP_ERR_INVALID_ARG;
    }
    struct aes67_session_mgr *mgr = handle;
    if (xSemaphoreTake(mgr->lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t ret = ESP_ERR_NOT_FOUND;
    if (mgr->sources[id].enabled && mgr->sources[id].rtp_stream) {
        ret = aes67_rtp_stream_get_status(mgr->sources[id].rtp_stream, status);
    }
    xSemaphoreGive(mgr->lock);
    return ret;
}

esp_err_t aes67_session_get_sink_status(aes67_session_handle_t handle, uint8_t id,
                                        aes67_rtp_stream_status_t *status)
{
    if (!handle || !status || id >= CONFIG_AES67_MAX_SINKS) {
        return ESP_ERR_INVALID_ARG;
    }
    struct aes67_session_mgr *mgr = handle;
    if (xSemaphoreTake(mgr->lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t ret = ESP_ERR_NOT_FOUND;
    if (mgr->sinks[id].enabled && mgr->sinks[id].rtp_stream) {
        ret = aes67_rtp_stream_get_status(mgr->sinks[id].rtp_stream, status);
    }
    xSemaphoreGive(mgr->lock);
    return ret;
}

esp_err_t aes67_session_get_source(aes67_session_handle_t handle, uint8_t id,
                                   aes67_source_t *source)
{
    if (!handle || !source || id >= CONFIG_AES67_MAX_SOURCES) {
        return ESP_ERR_INVALID_ARG;
    }

    struct aes67_session_mgr *mgr = handle;

    if (xSemaphoreTake(mgr->lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    if (!mgr->sources[id].enabled) {
        xSemaphoreGive(mgr->lock);
        return ESP_ERR_NOT_FOUND;
    }

    memcpy(source, &mgr->sources[id], sizeof(aes67_source_t));
    xSemaphoreGive(mgr->lock);
    return ESP_OK;
}

esp_err_t aes67_session_get_sink(aes67_session_handle_t handle, uint8_t id,
                                 aes67_sink_t *sink)
{
    if (!handle || !sink || id >= CONFIG_AES67_MAX_SINKS) {
        return ESP_ERR_INVALID_ARG;
    }

    struct aes67_session_mgr *mgr = handle;

    if (xSemaphoreTake(mgr->lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    if (!mgr->sinks[id].enabled) {
        xSemaphoreGive(mgr->lock);
        return ESP_ERR_NOT_FOUND;
    }

    memcpy(sink, &mgr->sinks[id], sizeof(aes67_sink_t));
    xSemaphoreGive(mgr->lock);
    return ESP_OK;
}

int aes67_session_get_source_count(aes67_session_handle_t handle)
{
    if (!handle) {
        return 0;
    }
    struct aes67_session_mgr *mgr = handle;

    if (xSemaphoreTake(mgr->lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        return 0;
    }
    int count = mgr->source_count;
    xSemaphoreGive(mgr->lock);
    return count;
}

int aes67_session_get_sink_count(aes67_session_handle_t handle)
{
    if (!handle) {
        return 0;
    }
    struct aes67_session_mgr *mgr = handle;

    if (xSemaphoreTake(mgr->lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        return 0;
    }
    int count = mgr->sink_count;
    xSemaphoreGive(mgr->lock);
    return count;
}

esp_err_t aes67_session_refresh_sdp(aes67_session_handle_t handle)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }
    struct aes67_session_mgr *mgr = handle;
    if (!mgr->config.sap_enabled || !mgr->sap) {
        return ESP_OK;
    }
    char *sdp_text = malloc(AES67_SDP_MAX_LEN);
    if (!sdp_text) {
        return ESP_ERR_NO_MEM;
    }
    if (xSemaphoreTake(mgr->lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        free(sdp_text);
        return ESP_ERR_TIMEOUT;
    }
    uint32_t local_ip = 0;
    aes67_net_get_local_ip(&local_ip);
    int refreshed = 0;
    for (int i = 0; i < CONFIG_AES67_MAX_SOURCES; i++) {
        aes67_source_t *src = &mgr->sources[i];
        if (!src->enabled) {
            continue;
        }
        aes67_sdp_t before = src->sdp;
        fill_refclk(mgr, &src->sdp);
        if (before.has_ptp_refclk == src->sdp.has_ptp_refclk &&
            before.ptp_traceable == src->sdp.ptp_traceable &&
            before.ptp_domain == src->sdp.ptp_domain &&
            memcmp(before.ptp_grandmaster_id, src->sdp.ptp_grandmaster_id, 8) == 0) {
            continue;                       /* nothing changed for this source */
        }
        src->sdp.session_version++;         /* o= version: the description changed */
        int len = aes67_sdp_generate(&src->sdp, sdp_text, AES67_SDP_MAX_LEN);
        if (len > 0) {
            aes67_sap_announce(mgr->sap, src->sap_msg_id, local_ip, sdp_text);
            refreshed++;
        }
    }
    xSemaphoreGive(mgr->lock);
    free(sdp_text);
    if (refreshed) {
        ESP_LOGI(TAG, "%d source SDP%s re-announced with the current PTP reference",
                 refreshed, refreshed == 1 ? "" : "s");
    }
    return ESP_OK;
}

esp_err_t aes67_session_destroy(aes67_session_handle_t handle)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }

    struct aes67_session_mgr *mgr = handle;

    /* Remove all active sources */
    for (int i = 0; i < CONFIG_AES67_MAX_SOURCES; i++) {
        if (mgr->sources[i].enabled) {
            aes67_session_remove_source(handle, (uint8_t)i);
        }
    }

    /* Remove all active sinks */
    for (int i = 0; i < CONFIG_AES67_MAX_SINKS; i++) {
        if (mgr->sinks[i].enabled) {
            aes67_session_remove_sink(handle, (uint8_t)i);
        }
    }

    if (mgr->lock) {
        vSemaphoreDelete(mgr->lock);
    }

    free(mgr);
    ESP_LOGI(TAG, "session manager destroyed");
    return ESP_OK;
}
