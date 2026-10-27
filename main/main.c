/*
 * SPDX-FileCopyrightText: 2026 DatanoiseTV
 *
 * SPDX-License-Identifier: Apache-2.0
 */
/*
 * AES67 example application for ESP32-P4-NANO.
 *
 * Hardware configuration:
 *   - IP101 Ethernet PHY (RMII, external 50MHz crystal)
 *   - ES8311 audio codec (I2S, 48kHz, 24-bit)
 *   - PA enable on GPIO53
 *
 * This example initializes Ethernet, waits for an IP address, then
 * brings up the AES67 stack and creates a 2-channel L24 source stream
 * that captures audio from the ES8311 and streams it over RTP.
 */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_log.h"
#include "esp_timer.h"
#if CONFIG_HEAP_TRACING_STANDALONE
#include "esp_heap_trace.h"
#endif
#include "esp_err.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "driver/gpio.h"

#include "esp_eth.h"
#include "esp_eth_mac.h"
#include "esp_eth_phy.h"

#include <string.h>
#include "aes67.h"
#include "aes67_config.h"
#include "aes67_convert.h"
#include "aes67_session.h"
#include "aes67_rtp.h"
#include "aes67_sap.h"
#include "aes67_ptp.h"
#include "aes67_audio.h"
#include "dsps_tone_gen.h"
#include "driver/i2c.h"
#include "esp_ldo_regulator.h"
#include "es8311.h"
#include "nvs.h"
#include "aes67_webui.h"

/* Session handle for SAP callback */
static aes67_session_handle_t s_session = NULL;
static bool s_sink_added = false;

/* SAP discovery callback: auto-subscribe to discovered remote sources */
static void sap_discovery_cb(bool is_announce,
                              const aes67_sap_remote_source_t *source,
                              void *user_data)
{
    if (!is_announce || !source || s_sink_added || !s_session) {
        return;
    }

    /* Subscribe to any discovered source (first one wins) */

    ESP_LOGI("main", "Auto-subscribing to \"%s\" from SAP", source->name);

    uint8_t sink_id = 0;
    esp_err_t err = aes67_session_add_sink(s_session, source->name,
                                            source->sdp, &sink_id);
    if (err == ESP_OK) {
        s_sink_added = true;
        aes67_sink_t sink;
        if (aes67_session_get_sink(s_session, sink_id, &sink) == ESP_OK) {
            /* Create a matching source with the same codec, channels and
             * packet time (the engine paces each source by its own ptime) */
            uint8_t src_id = 0;
            esp_err_t src_err = aes67_session_add_source(
                s_session, "ESP32-P4 AES67",
                sink.rtp_config.channels, sink.rtp_config.codec,
                sink.rtp_config.packet_time_us, &src_id);
            if (src_err == ESP_OK) {
                ESP_LOGI("main", "Source matched to sink: %uch %s %uus",
                         sink.rtp_config.channels,
                         aes67_codec_to_str(sink.rtp_config.codec),
                         sink.rtp_config.packet_time_us);
            }
        }
        ESP_LOGI("main", "Sink added (id=%u) -- receiving \"%s\"",
                 sink_id, source->name);
    } else {
        ESP_LOGW("main", "Failed to add sink for \"%s\": %s",
                 source->name, esp_err_to_name(err));
    }
}

static const char *TAG = "main";

/* Event group bit set when the Ethernet interface obtains an IP */
#define ETH_GOT_IP_BIT  BIT0

/* ---- Pin definitions for ESP32-P4-NANO ---- */

/* RMII Ethernet (directly wired, configured via emac_config) */
#define ETH_MDC_GPIO        31
#define ETH_MDIO_GPIO       52
#define ETH_REF_CLK_GPIO    50
#define ETH_PHY_RST_GPIO    51
#define ETH_PHY_ADDR        1

/* I2S and amplifier pins come from menuconfig ("AES67 Example"):
 * onboard ES8311 (ESP32-P4-NANO / ESP32-P4-ETH): MCLK 13, BCLK 12, LRCK 10,
 * DOUT 9, DIN 11, PA 53; or an external I2S DAC/ADC on header pins. */
#define I2S_MCLK_GPIO       CONFIG_EXAMPLE_I2S_MCLK_GPIO
#define I2S_SCLK_GPIO       CONFIG_EXAMPLE_I2S_BCLK_GPIO
#define I2S_LRCK_GPIO       CONFIG_EXAMPLE_I2S_LRCK_GPIO
#define I2S_DOUT_GPIO       CONFIG_EXAMPLE_I2S_DOUT_GPIO
#define I2S_DIN_GPIO        CONFIG_EXAMPLE_I2S_DIN_GPIO

/* Power amplifier enable (active high) */
#define PA_CTRL_GPIO        CONFIG_EXAMPLE_PA_CTRL_GPIO

/* I2C for ES8311 codec control */
#define I2C_SCL_GPIO        8
#define I2C_SDA_GPIO        7
#define I2C_PORT            I2C_NUM_0
#define I2C_CLK_HZ          100000
#define ES8311_ADDR         ES8311_ADDRESS_0    /* 0x18, CE pin low */

static EventGroupHandle_t s_eth_event_group;

/* Ethernet link event handler */
static void eth_event_handler(void *arg, esp_event_base_t event_base,
                              int32_t event_id, void *event_data)
{
    switch (event_id) {
    case ETHERNET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "ethernet link up");
        break;
    case ETHERNET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "ethernet link down");
        break;
    case ETHERNET_EVENT_START:
        ESP_LOGI(TAG, "ethernet started");
        break;
    case ETHERNET_EVENT_STOP:
        ESP_LOGI(TAG, "ethernet stopped");
        break;
    default:
        break;
    }
}

/* IP acquisition event handler */
static void ip_event_handler(void *arg, esp_event_base_t event_base,
                             int32_t event_id, void *event_data)
{
    if (event_id == IP_EVENT_ETH_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(s_eth_event_group, ETH_GOT_IP_BIT);
    }
}

/*
 * Initialize the IP101 Ethernet PHY over RMII.
 * The ESP32-P4-NANO uses an external 50MHz crystal for the RMII reference
 * clock, so we configure EMAC_CLK_EXT_IN on GPIO50.
 */
static esp_err_t ethernet_init(esp_eth_handle_t *out_eth_handle)
{
    /* EMAC configuration with RMII pin mapping */
    eth_esp32_emac_config_t emac_config = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    emac_config.smi_gpio.mdc_num = ETH_MDC_GPIO;
    emac_config.smi_gpio.mdio_num = ETH_MDIO_GPIO;
    emac_config.interface = EMAC_DATA_INTERFACE_RMII;
    emac_config.clock_config.rmii.clock_mode = EMAC_CLK_EXT_IN;
    emac_config.clock_config.rmii.clock_gpio = ETH_REF_CLK_GPIO;

    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    mac_config.flags = ETH_MAC_FLAG_PIN_TO_CORE;  /* Pin to core 0 */
    mac_config.rx_task_prio = 24;  /* Highest: above lwIP (23), feeds audio ISR directly */
    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&emac_config, &mac_config);
    if (!mac) {
        ESP_LOGE(TAG, "failed to create EMAC");
        return ESP_FAIL;
    }

    /* IP101 PHY at address 1, with hardware reset on GPIO51 */
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.phy_addr = ETH_PHY_ADDR;
    phy_config.reset_gpio_num = ETH_PHY_RST_GPIO;
    esp_eth_phy_t *phy = esp_eth_phy_new_ip101(&phy_config);
    if (!phy) {
        ESP_LOGE(TAG, "failed to create IP101 PHY");
        return ESP_FAIL;
    }

    /* Install the Ethernet driver */
    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);
    esp_eth_handle_t eth_handle = NULL;
    esp_err_t ret = esp_eth_driver_install(&eth_config, &eth_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ethernet driver install failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Default Ethernet netif (key "ETH_DEF"); the component binds its
     * PTP daemon and lookups to config.net.interface_name, which defaults
     * to the same key. */
    esp_netif_config_t netif_config = ESP_NETIF_DEFAULT_ETH();
    esp_netif_t *eth_netif = esp_netif_new(&netif_config);
    if (!eth_netif) {
        ESP_LOGE(TAG, "failed to create ethernet netif");
        return ESP_FAIL;
    }

    /* Attach the driver to the netif via the default glue layer */
    esp_eth_netif_glue_handle_t glue = esp_eth_new_netif_glue(eth_handle);
    ret = esp_netif_attach(eth_netif, glue);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "failed to attach ethernet to netif: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Register event handlers for link and IP events */
    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID,
                                               &eth_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP,
                                               &ip_event_handler, NULL));

    /* Enable Ethernet flow control to prevent frame drops under load.
     * Without this, the EMAC drops ~8% of multicast frames at high
     * packet rates (1000 pps) because the DMA can't keep up. */
    bool flow_ctrl = true;
    esp_eth_ioctl(eth_handle, ETH_CMD_S_FLOW_CTRL, &flow_ctrl);

    /* Start Ethernet */
    ret = esp_eth_start(eth_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ethernet start failed: %s", esp_err_to_name(ret));
        return ret;
    }

    *out_eth_handle = eth_handle;
    return ESP_OK;
}

/* Enable the power amplifier on the ES8311 output stage */
static void pa_ctrl_enable(void)
{
    if (PA_CTRL_GPIO < 0) {
        return;
    }
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << PA_CTRL_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
    gpio_set_level(PA_CTRL_GPIO, 1);
    ESP_LOGI(TAG, "PA enabled (GPIO%d high)", PA_CTRL_GPIO);
}

/* ---- Codec settings: menuconfig defaults, overridden by values saved in
 * NVS, adjustable at runtime through the Web UI controls. ---- */

static es8311_handle_t s_codec = NULL;
static int32_t s_mic_gain_db = CONFIG_EXAMPLE_MIC_GAIN_DB;
static int32_t s_volume = CONFIG_EXAMPLE_SPEAKER_VOLUME;

#define SETTINGS_NS "aes67ex"

static void settings_load(void)
{
    nvs_handle_t h;
    if (nvs_open(SETTINGS_NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    int32_t v;
    if (nvs_get_i32(h, "mic_gain", &v) == ESP_OK && v >= 0 && v <= 42) {
        s_mic_gain_db = (v / 6) * 6;
    }
    if (nvs_get_i32(h, "volume", &v) == ESP_OK && v >= 0 && v <= 100) {
        s_volume = v;
    }
    nvs_close(h);
}

static void settings_save(const char *key, int32_t value)
{
    nvs_handle_t h;
    if (nvs_open(SETTINGS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i32(h, key, value);
        nvs_commit(h);
        nvs_close(h);
    }
}

static esp_err_t apply_mic_gain(int32_t db)
{
    if (!s_codec) return ESP_ERR_INVALID_STATE;
    return es8311_microphone_gain_set(s_codec, (es8311_mic_gain_t)(ES8311_MIC_GAIN_0DB + db / 6));
}

static esp_err_t apply_volume(int32_t vol)
{
    if (!s_codec) return ESP_ERR_INVALID_STATE;
    return es8311_voice_volume_set(s_codec, (int)vol, NULL);
}

/* Web UI control callbacks */
static esp_err_t ctl_mic_gain_get(void *ud, int32_t *v) { *v = s_mic_gain_db; return ESP_OK; }
static esp_err_t ctl_mic_gain_set(void *ud, int32_t v)
{
    esp_err_t err = apply_mic_gain(v);
    if (err == ESP_OK) {
        s_mic_gain_db = v;
        settings_save("mic_gain", v);
        ESP_LOGI(TAG, "mic gain set to %ld dB", (long)v);
    }
    return err;
}
static esp_err_t ctl_volume_get(void *ud, int32_t *v) { *v = s_volume; return ESP_OK; }
static esp_err_t ctl_volume_set(void *ud, int32_t v)
{
    esp_err_t err = apply_volume(v);
    if (err == ESP_OK) {
        s_volume = v;
        settings_save("volume", v);
        ESP_LOGI(TAG, "volume set to %ld", (long)v);
    }
    return err;
}

static void register_codec_controls(aes67_webui_handle_t webui)
{
    const aes67_webui_control_t mic = {
        .name = "mic_gain", .label = "Mic gain", .unit = "dB",
        .min = 0, .max = 42, .step = 6,
        .get = ctl_mic_gain_get, .set = ctl_mic_gain_set,
    };
    const aes67_webui_control_t vol = {
        .name = "volume", .label = "Speaker volume", .unit = "%",
        .min = 0, .max = 100, .step = 1,
        .get = ctl_volume_get, .set = ctl_volume_set,
    };
    aes67_webui_add_control(webui, &mic);
    aes67_webui_add_control(webui, &vol);
}

/*
 * Initialize the ES8311 audio codec via I2C.
 * Configures it for 48kHz 24-bit I2S operation with both DAC (playback)
 * and ADC (capture) enabled.
 */
static esp_err_t es8311_codec_init(void)
{
    /* Initialize I2C master bus */
    i2c_config_t i2c_cfg = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = I2C_SDA_GPIO,
        .scl_io_num = I2C_SCL_GPIO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_CLK_HZ,
    };
    esp_err_t ret = i2c_param_config(I2C_PORT, &i2c_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C param config failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = i2c_driver_install(I2C_PORT, I2C_MODE_MASTER, 0, 0, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C driver install failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Create ES8311 codec handle */
    es8311_handle_t codec = es8311_create(I2C_PORT, ES8311_ADDR);
    s_codec = codec;
    if (!codec) {
        ESP_LOGE(TAG, "ES8311 create failed");
        return ESP_FAIL;
    }

    /* Configure clock: MCLK from MCLK pin, 48kHz sample rate.
     * I2S uses MCLK_MULTIPLE_384 (matching official ESP-IDF example).
     * MCLK = 48000 * 384 = 18.432 MHz */
    es8311_clock_config_t clk_cfg = {
        .mclk_inverted = false,
        .sclk_inverted = false,
        .mclk_from_mclk_pin = true,
        .mclk_frequency = 48000 * 384,     /* 18.432 MHz */
        .sample_frequency = 48000,
    };

    /* 32-bit resolution -- int32 maps directly, no truncation */
    ret = es8311_init(codec, &clk_cfg, ES8311_RESOLUTION_32, ES8311_RESOLUTION_32);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ES8311 init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = es8311_voice_volume_set(codec, (int)s_volume, NULL);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "ES8311 volume set failed: %s", esp_err_to_name(ret));
    }

    /* Unmute DAC output */
    ret = es8311_voice_mute(codec, false);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "ES8311 unmute failed: %s", esp_err_to_name(ret));
    }

    /* Configure microphone: analog mic; gain and volume from NVS, else
     * the menuconfig defaults (6 dB steps) */
    es8311_microphone_config(codec, false);
    apply_mic_gain(s_mic_gain_db);
    ESP_LOGI(TAG, "ES8311 mic gain %ld dB, volume %ld", (long)s_mic_gain_db, (long)s_volume);

    /* Dump ES8311 register state for debugging */
    es8311_register_dump(codec);

    ESP_LOGI(TAG, "ES8311 codec initialized (48kHz, 32-bit slots, MCLK=18.432MHz)");
    return ESP_OK;
}

void app_main(void)
{
    /* Initialize NVS -- required by some ESP-IDF components internally */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    settings_load();

    /* Configure LDO4 to 3.3V for SDIO/SPI power domain.
     * Must be done early before any peripherals on this rail. */
    esp_ldo_channel_handle_t ldo4 = NULL;
    esp_ldo_channel_config_t ldo_cfg = {
        .chan_id = 4,
        .voltage_mv = 3300,
    };
    if (esp_ldo_acquire_channel(&ldo_cfg, &ldo4) == ESP_OK) {
        ESP_LOGI(TAG, "LDO4 set to 3.3V");
    } else {
        ESP_LOGW(TAG, "LDO4 config failed (may already be set)");
    }

    /* Create the default event loop used by Ethernet and AES67 events */
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* Initialize the TCP/IP stack */
    ESP_ERROR_CHECK(esp_netif_init());

    /* Set up the event group used to synchronize on IP acquisition */
    s_eth_event_group = xEventGroupCreate();

    /* Bring up Ethernet with the IP101 PHY */
    esp_eth_handle_t eth_handle = NULL;
    ESP_ERROR_CHECK(ethernet_init(&eth_handle));

    /* Block until we have an IP address. The AES67 stack needs network
     * connectivity for PTP, RTP multicast and SAP. */
    ESP_LOGI(TAG, "waiting for IP address...");
    xEventGroupWaitBits(s_eth_event_group, ETH_GOT_IP_BIT,
                        pdFALSE, pdTRUE, portMAX_DELAY);

    /* Enable the power amplifier before starting audio */
    pa_ctrl_enable();

    /* ES8311 codec init is deferred until AFTER the AES67 I2S driver starts.
     * The ES8311 needs MCLK to be running during initialization. */

    /* Build the AES67 node configuration with our I2S pins and codec settings */
    aes67_config_t aes67_cfg = AES67_CONFIG_DEFAULT();
    strlcpy(aes67_cfg.node_name, "ESP32-P4 AES67", sizeof(aes67_cfg.node_name));

    aes67_cfg.net.eth_handle = eth_handle;

    aes67_cfg.i2s_pins.mck_gpio  = I2S_MCLK_GPIO;
    aes67_cfg.i2s_pins.bck_gpio  = I2S_SCLK_GPIO;
    aes67_cfg.i2s_pins.ws_gpio   = I2S_LRCK_GPIO;
    aes67_cfg.i2s_pins.dout_gpio = I2S_DOUT_GPIO;
    aes67_cfg.i2s_pins.din_gpio  = I2S_DIN_GPIO;

    aes67_cfg.audio.sample_rate   = 48000;
    aes67_cfg.audio.channels      = 2;
    aes67_cfg.audio.codec         = AES67_CODEC_L24;
    aes67_cfg.audio.word_length   = 3;       /* 24-bit = 3 bytes */
    aes67_cfg.audio.packet_time_us = 1000;   /* 1ms packet time per AES67 */

    /* Initialize the AES67 node */
    aes67_node_handle_t node = NULL;
    ESP_ERROR_CHECK(aes67_node_init(&aes67_cfg, &node));

    /* Start PTP sync, RTP engine, SAP announcements, the Ethernet receive
     * hook and audio I/O. This enables I2S, which starts MCLK generation. */
    ESP_ERROR_CHECK(aes67_node_start(node));

    /* Initialize ES8311 codec AFTER I2S is running (MCLK must be active).
     * This is the critical init order discovered through testing. */
    esp_err_t codec_ret = es8311_codec_init();
    if (codec_ret != ESP_OK) {
        ESP_LOGE(TAG, "ES8311 codec init FAILED: %s", esp_err_to_name(codec_ret));
    } else {
        ESP_LOGI(TAG, "ES8311 codec initialized (MCLK running)");
    }

    /* Obtain the session manager handle from the node so we can add streams */
    aes67_session_handle_t session = NULL;
    ESP_ERROR_CHECK(aes67_node_get_session(node, &session));
    s_session = session;

    /* Register SAP callback to auto-subscribe to discovered sources.
     * This will add a sink for the first remote source found (e.g. SIENNA). */
    aes67_sap_handle_t sap = NULL;
    /* Get SAP handle from the node config - it's stored internally.
     * For now, register via the session manager's SAP handle. */
    if (aes67_node_get_sap(node, &sap) == ESP_OK && sap) {
        aes67_sap_register_cb(sap, sap_discovery_cb, NULL);
        ESP_LOGI(TAG, "SAP auto-subscribe enabled");

        /* Check if any sources were already discovered before callback
         * was registered (SAP starts during node_start). */
        int remote_count = aes67_sap_get_remote_count(sap);
        for (int i = 0; i < remote_count && !s_sink_added; i++) {
            aes67_sap_remote_source_t remote;
            if (aes67_sap_get_remote(sap, i, &remote) == ESP_OK) {
                sap_discovery_cb(true, &remote, NULL);
            }
        }
    }

#if CONFIG_EXAMPLE_CREATE_MIC_SOURCE
    /* Create a mic source at boot so receivers (e.g. an AES67 speaker)
     * can subscribe without any remote source being present. The ES8311
     * ADC is mono; the stream is stereo L24 at the default 1 ms ptime.
     * The SAP callback may add a second, codec-matched source later. */
    {
        uint8_t src_id = 0;
        esp_err_t src_err = aes67_session_add_source(session, CONFIG_EXAMPLE_MIC_SOURCE_NAME,
                                                     2, AES67_CODEC_L24, 0, &src_id);
        if (src_err == ESP_OK) {
            ESP_LOGI(TAG, "Mic source \"%s\" created (id=%u): 2ch L24, announced via SAP",
                     CONFIG_EXAMPLE_MIC_SOURCE_NAME, src_id);
        } else {
            ESP_LOGE(TAG, "Mic source create failed: %s", esp_err_to_name(src_err));
        }
    }
#endif

#if CONFIG_AES67_WEBUI_ENABLED
    /* Start the WebUI (HTTP + WebSocket) */
    aes67_webui_handle_t webui = NULL;
    if (aes67_webui_init(node, CONFIG_AES67_WEBUI_PORT, &webui) == ESP_OK) {
        register_codec_controls(webui);
        aes67_webui_start(webui);
    }
#endif

    ESP_LOGI(TAG, "node ready");

#if CONFIG_EXAMPLE_RESTART_TEST_CYCLES > 0
    /* Lifecycle test: stop and start the node repeatedly. With
     * CONFIG_HEAP_TRACING_STANDALONE the allocations that survive the
     * last cycle are dumped with their call stacks. */
    {
#if CONFIG_HEAP_TRACING_STANDALONE
        static heap_trace_record_t s_trace[200];
        heap_trace_init_standalone(s_trace, 200);
#endif
        uint32_t heap_before = esp_get_free_heap_size();
        for (int cycle = 1; cycle <= CONFIG_EXAMPLE_RESTART_TEST_CYCLES; cycle++) {
            vTaskDelay(pdMS_TO_TICKS(2000));
#if CONFIG_HEAP_TRACING_STANDALONE
            if (cycle == CONFIG_EXAMPLE_RESTART_TEST_CYCLES) {
                heap_trace_start(HEAP_TRACE_LEAKS);
            }
#endif
            int64_t t0 = esp_timer_get_time();
            esp_err_t e1 = aes67_node_stop(node);
            int64_t t1 = esp_timer_get_time();
            vTaskDelay(pdMS_TO_TICKS(500));
            esp_err_t e2 = aes67_node_start(node);
            int64_t t2 = esp_timer_get_time();
            bool ok = heap_caps_check_integrity_all(true);
            ESP_LOGI(TAG, "lifecycle cycle %d: stop %s in %lld ms, start %s in %lld ms, "
                     "heap free %lu (delta %+ld), integrity %s",
                     cycle, esp_err_to_name(e1), (long long)((t1 - t0) / 1000),
                     esp_err_to_name(e2), (long long)((t2 - t1 - 500000) / 1000),
                     (unsigned long)esp_get_free_heap_size(),
                     (long)((int32_t)esp_get_free_heap_size() - (int32_t)heap_before),
                     ok ? "ok" : "CORRUPT");
        }
#if CONFIG_HEAP_TRACING_STANDALONE
        /* Stop once more so the dump shows only what a full stop/start/stop
         * left behind, then bring the node back up. */
        aes67_node_stop(node);
        heap_trace_stop();
        heap_trace_dump();
        aes67_node_start(node);
#endif
        ESP_LOGI(TAG, "lifecycle test done");
    }
#endif

    /* Monitor loop: ISR health only */
    extern void aes67_audio_log_isr_stats(void);
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        aes67_audio_log_isr_stats();
    }
}
