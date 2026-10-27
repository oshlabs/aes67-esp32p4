/*
 * SPDX-FileCopyrightText: 2026 DatanoiseTV
 *
 * SPDX-License-Identifier: Apache-2.0
 */
/*
 * mDNS service advertisement for AES67/RAVENNA.
 *
 * Advertises this node as an AES67 device using DNS-SD/mDNS,
 * making it discoverable by other AES67 devices on the network.
 */

#include "aes67_mdns.h"
#include "aes67_sdp.h"

#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "mdns.h"

static const char *TAG = "aes67_mdns";

esp_err_t aes67_mdns_init(const aes67_config_t *config)
{
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mdns_init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = mdns_hostname_set(config->node_name);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mdns_hostname_set failed: %s", esp_err_to_name(err));
        return err;
    }

    err = mdns_instance_name_set(config->node_name);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mdns_instance_name_set failed: %s", esp_err_to_name(err));
    }

    /* The device record: what this node streams by default. Individual
     * streams are described by their SAP announcements. */
    char rate_str[12], ch_str[4], ptime_str[8];
    snprintf(rate_str, sizeof(rate_str), "%lu", (unsigned long)config->audio.sample_rate);
    snprintf(ch_str, sizeof(ch_str), "%u", config->audio.channels);
    snprintf(ptime_str, sizeof(ptime_str), "%u", config->audio.packet_time_us);
    mdns_txt_item_t txt_items[] = {
        { "proto", "AES67" },
        { "rate",  rate_str },
        { "ch",    ch_str },
        { "codec", aes67_codec_to_str(config->audio.codec) },
        { "ptime_us", ptime_str },
    };

    err = mdns_service_add(config->node_name, "_ravenna", "_udp",
                           config->net.rtp_port, txt_items,
                           sizeof(txt_items) / sizeof(txt_items[0]));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mdns_service_add _ravenna._udp failed: %s",
                 esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "mDNS: _ravenna._udp \"%s\" on port %u", config->node_name,
             config->net.rtp_port);
    return ESP_OK;
}

void aes67_mdns_deinit(void)
{
    mdns_service_remove_all();
    mdns_free();
    ESP_LOGI(TAG, "mDNS services removed");
}
