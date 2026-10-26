/*
 * SPDX-FileCopyrightText: 2026 Leon de Rooij (OSHLabs)
 *
 * SPDX-License-Identifier: Apache-2.0
 */
/*
 * Ethernet receive hook: PTP and RTP are taken off the driver's input path
 * before lwIP. Installed by the node; single instance.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "esp_eth.h"
#include "esp_netif.h"
#include "aes67_rtp.h"
#include "aes67_ptp.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t frames_seen;           /* every frame the driver delivered */
    uint32_t frames_forwarded;      /* handed to lwIP */
    uint32_t ptp_consumed;          /* PTP/UDP taken by the daemon */
    uint32_t rtp_consumed;          /* RTP decoded by the engine */
} aes67_eth_rx_stats_t;

/**
 * Install the hook on the driver's input path (once) and enable it.
 * Non-matching frames are forwarded to `netif`.
 */
esp_err_t aes67_eth_rx_install(esp_eth_handle_t eth_handle, esp_netif_t *netif,
                               aes67_rtp_engine_handle_t rtp, aes67_ptp_handle_t ptp);

/** Stop consuming frames; everything is forwarded to lwIP again. */
void aes67_eth_rx_disable(void);

esp_err_t aes67_eth_rx_get_stats(aes67_eth_rx_stats_t *stats);

#ifdef __cplusplus
}
#endif
