/*
 * SPDX-FileCopyrightText: 2026 Leon de Rooij (OSHLabs)
 *
 * SPDX-License-Identifier: Apache-2.0
 */
/*
 * Ethernet receive hook.
 *
 * Installed on the Ethernet driver's input path, so every received frame
 * passes here before lwIP. PTP over UDP is handed to the PTP daemon with
 * the driver's hardware receive timestamp; RTP for an active sink is
 * decoded by the RTP engine straight into the playout buffer; everything
 * else is forwarded to lwIP through the netif. This is the receive path
 * the README's latency figures come from: no socket, no lwIP, no task hop
 * between the wire and the DMA ring.
 */

#include "aes67_eth_rx.h"
#include "aes67_ptp.h"
#include "aes67_rtp.h"

#include <stdlib.h>
#include "esp_log.h"
#include "esp_attr.h"
#include "esp_eth_driver.h"
#include "esp_netif.h"

static const char *TAG = "aes67_eth_rx";

static struct {
    esp_netif_t                *netif;
    aes67_rtp_engine_handle_t   rtp;
    aes67_ptp_handle_t          ptp;
    bool                        installed;
    volatile bool               enabled;
    aes67_eth_rx_stats_t        stats;
} s_hook;

static esp_err_t IRAM_ATTR eth_rx_hook(esp_eth_handle_t eth_handle, uint8_t *buffer,
                                       uint32_t length, void *priv, void *info)
{
    s_hook.stats.frames_seen++;

    if (s_hook.enabled) {
        if (s_hook.ptp && aes67_ptp_eth_input(buffer, length, info)) {
            s_hook.stats.ptp_consumed++;
            free(buffer);
            return ESP_OK;
        }
        if (s_hook.rtp && aes67_rtp_engine_rx_frame(s_hook.rtp, buffer, length)) {
            s_hook.stats.rtp_consumed++;
            free(buffer);
            return ESP_OK;
        }
    }

    s_hook.stats.frames_forwarded++;
    if (s_hook.netif) {
        return esp_netif_receive(s_hook.netif, buffer, length, NULL);
    }
    free(buffer);
    return ESP_OK;
}

esp_err_t aes67_eth_rx_install(esp_eth_handle_t eth_handle, esp_netif_t *netif,
                               aes67_rtp_engine_handle_t rtp, aes67_ptp_handle_t ptp)
{
    if (!eth_handle || !netif) {
        return ESP_ERR_INVALID_ARG;
    }
    s_hook.netif = netif;
    s_hook.rtp = rtp;
    s_hook.ptp = ptp;
    s_hook.enabled = true;
    if (!s_hook.installed) {
        esp_err_t err = esp_eth_update_input_path_info(eth_handle, eth_rx_hook, netif);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_eth_update_input_path_info failed: %s", esp_err_to_name(err));
            s_hook.enabled = false;
            return err;
        }
        s_hook.installed = true;
    }
    ESP_LOGI(TAG, "Ethernet receive hook installed (PTP + RTP before lwIP)");
    return ESP_OK;
}

void aes67_eth_rx_disable(void)
{
    /* The driver offers no way to read back the previous input path, so
     * the hook stays installed and simply forwards everything. */
    s_hook.enabled = false;
    s_hook.rtp = NULL;
    s_hook.ptp = NULL;
}

esp_err_t aes67_eth_rx_get_stats(aes67_eth_rx_stats_t *stats)
{
    if (!stats) {
        return ESP_ERR_INVALID_ARG;
    }
    *stats = s_hook.stats;
    return ESP_OK;
}
