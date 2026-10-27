/*
 * SPDX-FileCopyrightText: 2026 Leon de Rooij (OSHLabs)
 *
 * SPDX-License-Identifier: Apache-2.0
 */
/*
 * mDNS/DNS-SD advertisement of this node as a RAVENNA/AES67 device
 * (_ravenna._udp). Streams themselves are announced over SAP; RAVENNA's
 * RTSP service is not advertised because no RTSP server runs here.
 */

#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "aes67_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Register the _ravenna._udp service with a TXT record built from the config. */
esp_err_t aes67_mdns_init(const aes67_config_t *config);

/** Remove the services and stop mDNS. */
void aes67_mdns_deinit(void);

#ifdef __cplusplus
}
#endif
