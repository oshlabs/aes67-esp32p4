/*
 * SPDX-FileCopyrightText: 2026 DatanoiseTV
 *
 * SPDX-License-Identifier: Apache-2.0
 */
/*
 * AES67 Web UI - HTTP server and WebSocket backend.
 *
 * Serves an embedded SPA, exposes REST endpoints for device status
 * and stream management, and pushes real-time status updates over
 * a WebSocket connection at 2Hz.
 */

#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "aes67.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque Web UI context */
typedef struct aes67_webui *aes67_webui_handle_t;

/**
 * Initialize the Web UI server. Does not start listening yet.
 *
 * @param node   Running AES67 node instance
 * @param port   HTTP port (e.g. 80)
 * @param handle Output handle for the created context
 * @return ESP_OK on success
 */
esp_err_t aes67_webui_init(aes67_node_handle_t node, uint16_t port,
                            aes67_webui_handle_t *handle);

/**
 * Start the HTTP server and begin accepting connections.
 */
esp_err_t aes67_webui_start(aes67_webui_handle_t handle);

/**
 * Stop the HTTP server and tear down the WebSocket push task.
 */
esp_err_t aes67_webui_stop(aes67_webui_handle_t handle);

/* ---- Runtime controls ----
 *
 * The application registers named integer controls (a codec's mic gain, a
 * playback volume, a link offset). The Web UI shows each as a slider,
 * reports its value in the status frame and in GET /api/controls, and
 * applies changes from POST /api/controls/<name> with {"value":N}, subject
 * to the same access checks as other changes. Strings must stay valid for
 * the life of the Web UI (string literals are fine). */

#define AES67_WEBUI_MAX_CONTROLS 8

typedef struct {
    const char *name;     /* API identifier: letters, digits, '_' or '-', up to 23 chars */
    const char *label;    /* shown on the page */
    const char *unit;     /* "dB", "%", "" ... */
    int32_t     min;
    int32_t     max;
    int32_t     step;     /* > 0; values are snapped to min + k*step */
    esp_err_t (*get)(void *user_data, int32_t *value);
    esp_err_t (*set)(void *user_data, int32_t value);
    void       *user_data;
} aes67_webui_control_t;

/**
 * Register a control. May be called before or after aes67_webui_start().
 * Returns ESP_ERR_NO_MEM when the table is full, ESP_ERR_INVALID_ARG on a
 * bad name, range or missing callbacks.
 */
esp_err_t aes67_webui_add_control(aes67_webui_handle_t handle,
                                  const aes67_webui_control_t *control);

#ifdef __cplusplus
}
#endif
