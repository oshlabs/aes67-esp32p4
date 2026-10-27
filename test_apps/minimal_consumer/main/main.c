/*
 * SPDX-FileCopyrightText: 2026 Leon de Rooij (OSHLabs)
 *
 * SPDX-License-Identifier: Apache-2.0
 */
/*
 * The smallest application that uses the component: bring up the node
 * and its web UI on an Ethernet interface the board code provides. This
 * is a build and link check for the component's public API; eth_init()
 * is a placeholder, since Ethernet wiring is board specific (see the
 * example's main.c for the ESP32-P4 EMAC + IP101 bring-up).
 */

#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "aes67.h"
#include "aes67_webui.h"

static const char *TAG = "consumer";

/* Board specific: create the Ethernet driver and attach it to a netif
 * registered under the key AES67_CONFIG_DEFAULT() expects ("ETH_DEF"). */
static esp_eth_handle_t eth_init(void)
{
    return NULL;
}

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    aes67_config_t cfg = AES67_CONFIG_DEFAULT();
    cfg.net.eth_handle = eth_init();
    if (!cfg.net.eth_handle) {
        ESP_LOGW(TAG, "no Ethernet driver on this board; nothing to run");
        return;
    }

    aes67_node_handle_t node = NULL;
    ESP_ERROR_CHECK(aes67_node_init(&cfg, &node));
    ESP_ERROR_CHECK(aes67_node_start(node));

    aes67_session_handle_t session = NULL;
    ESP_ERROR_CHECK(aes67_node_get_session(node, &session));
    uint8_t id = 0;
    ESP_ERROR_CHECK(aes67_session_add_source(session, "Consumer", 2, AES67_CODEC_L24, 0, &id));

    aes67_webui_handle_t webui = NULL;
    ESP_ERROR_CHECK(aes67_webui_init(node, 80, &webui));
    ESP_ERROR_CHECK(aes67_webui_start(webui));

    aes67_clock_status_t clock;
    ESP_ERROR_CHECK(aes67_node_get_clock_status(node, &clock));
    ESP_LOGI(TAG, "node %s running, audio clock state %d", AES67_VERSION_STRING, (int)clock.state);
}
