/*
 * SPDX-FileCopyrightText: 2026 DatanoiseTV
 *
 * SPDX-License-Identifier: Apache-2.0
 */
/*
 * AES67/RAVENNA audio-over-IP component for ESP-IDF
 *
 * Top-level API for initialization, configuration and lifecycle management.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "esp_event.h"
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include "aes67_config.h"
#include "aes67_session.h"
#include "aes67_audio.h"
#include "aes67_sap.h"
#include "aes67_ptp.h"
#include "aes67_rtp.h"
#include "aes67_clock.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque handle for an AES67 node instance */
typedef struct aes67_node *aes67_node_handle_t;

/* Event types posted to the application event loop */
typedef enum {
    AES67_EVENT_PTP_LOCKED,         /* PTP clock achieved lock */
    AES67_EVENT_PTP_UNLOCKED,       /* PTP clock lost lock */
    AES67_EVENT_SOURCE_ADDED,       /* Local source stream created */
    AES67_EVENT_SOURCE_REMOVED,     /* Local source stream removed */
    AES67_EVENT_SINK_ADDED,         /* Local sink stream created */
    AES67_EVENT_SINK_REMOVED,       /* Local sink stream removed */
    AES67_EVENT_REMOTE_FOUND,       /* Remote source discovered via SAP/mDNS */
    AES67_EVENT_REMOTE_LOST,        /* Remote source timed out */
    AES67_EVENT_SINK_RX_ACTIVE,     /* Sink is receiving audio data */
    AES67_EVENT_SINK_RX_TIMEOUT,    /* Sink stopped receiving audio data */
} aes67_event_t;

/* Event base declaration for ESP event loop */
ESP_EVENT_DECLARE_BASE(AES67_EVENT);

/**
 * Initialize the AES67 node with the given configuration.
 *
 * Brings up PTP, RTP engine, SAP, mDNS and audio subsystems.
 * The Ethernet interface must already be initialized and connected.
 *
 * @param config  Node configuration; required, and config->net.eth_handle
 *                must be set. Start from AES67_CONFIG_DEFAULT() and fill
 *                in the Ethernet handle and I2S pins. One node per device.
 * @param handle  Output handle for the created node instance.
 * @return ESP_OK on success
 */
esp_err_t aes67_node_init(const aes67_config_t *config, aes67_node_handle_t *handle);

/**
 * Start the AES67 node. Begins PTP sync, SAP announcements and enables
 * stream processing.
 */
esp_err_t aes67_node_start(aes67_node_handle_t handle);

/**
 * Stop the AES67 node. Tears down all active streams and halts PTP.
 */
esp_err_t aes67_node_stop(aes67_node_handle_t handle);

/**
 * Destroy the AES67 node and free all resources.
 */
esp_err_t aes67_node_destroy(aes67_node_handle_t handle);

/**
 * Get the node's current configuration (read-only snapshot).
 */
esp_err_t aes67_node_get_config(aes67_node_handle_t handle, aes67_config_t *config);

/**
 * Get the session manager handle from a running node.
 * Useful for adding/removing sources and sinks at the application level.
 */
esp_err_t aes67_node_get_session(aes67_node_handle_t handle,
                                  aes67_session_handle_t *session);

/**
 * Get the audio driver handle from a running node.
 */
esp_err_t aes67_node_get_audio(aes67_node_handle_t handle,
                               aes67_audio_handle_t *audio);

/**
 * Get the SAP handle from a running node.
 */
esp_err_t aes67_node_get_sap(aes67_node_handle_t handle,
                              aes67_sap_handle_t *sap);

/**
 * Get the PTP handle from a running node.
 */
esp_err_t aes67_node_get_ptp(aes67_node_handle_t handle,
                              aes67_ptp_handle_t *ptp);

/**
 * Get the RTP engine handle from a running node.
 */
esp_err_t aes67_node_get_rtp(aes67_node_handle_t handle,
                              aes67_rtp_engine_handle_t *rtp);

/* Receive-path counters: the Ethernet hook and the RTP engine's decoder */
typedef struct {
    uint32_t frames_seen;           /* every frame the driver delivered */
    uint32_t frames_forwarded;      /* handed to lwIP */
    uint32_t ptp_consumed;          /* PTP/UDP taken by the daemon with hardware stamps */
    uint32_t rtp_consumed;          /* RTP decoded into a sink */
    uint32_t rtp_unmatched;         /* RTP for no sink, forwarded */
    uint32_t rtp_malformed;         /* RTP that failed validation */
    uint32_t ssrc_rejected;         /* RTP from a second sender on a sink's group */
    uint32_t playout_overflow;      /* decoded packets the playout buffer could not take */
} aes67_node_rx_stats_t;

esp_err_t aes67_node_get_rx_stats(aes67_node_handle_t handle, aes67_node_rx_stats_t *stats);

/* Frame task (TIC) and hardware frame timer counters */
typedef struct {
    uint32_t ticks;                 /* frame periods processed */
    uint32_t catchups;              /* ticks processed late, after a lost wake-up */
    uint32_t deferrals;             /* ticks that waited for the capture chunk to land */
    uint32_t stalls;                /* timer silent or too far behind; capture backlog dropped */
    uint32_t stalls_logged;         /* internal */
    uint32_t capture_backlog_frames;/* captured frames waiting (constant once the clock is locked) */
    uint32_t timer_fires;           /* hardware frame timer interrupts */
    uint32_t timer_reanchors;       /* timer target skipped ahead */
    uint32_t timer_rearms;          /* timer re-armed after a stall */
} aes67_node_tic_stats_t;

esp_err_t aes67_node_get_tic_stats(aes67_node_handle_t handle, aes67_node_tic_stats_t *stats);

/* Audio clock recovery: how the APLL is tracking PTP */
esp_err_t aes67_node_get_clock_status(aes67_node_handle_t handle, aes67_clock_status_t *status);


#ifdef __cplusplus
}
#endif
