/*
 * SPDX-FileCopyrightText: 2026 DatanoiseTV
 *
 * SPDX-License-Identifier: Apache-2.0
 */
/*
 * SDP (Session Description Protocol) parser and generator for AES67.
 *
 * Generates and parses SDP documents conforming to AES67 / RAVENNA
 * requirements, including PTP reference clock attributes.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "aes67_config.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AES67_SDP_MAX_LEN  1024

/* Parsed SDP session description */
typedef struct {
    /* Origin (o= line) */
    char origin_username[32];
    uint32_t session_id;
    uint32_t session_version;
    uint32_t origin_ip;

    /* Session name (s= line) */
    char session_name[64];

    /* Connection (c= line) */
    uint32_t connection_ip;             /* Multicast group address */
    uint8_t ttl;

    /* Media (m= line) */
    uint16_t port;
    uint8_t payload_type;

    /* Audio parameters (from a=rtpmap) */
    aes67_codec_t codec;
    uint32_t sample_rate;
    uint8_t channels;
    uint8_t word_length;

    /* Packet time (a=ptime) and, when the SDP says so, the exact frame
     * count (a=framecount; RAVENNA). The frame count wins: a ptime of
     * 0.333 is a rounded 16 frames. */
    uint32_t ptime_us;                  /* Packet time in microseconds */
    uint16_t framecount;                /* Frames per packet, 0 if not given */

    /* PTP reference clock (a=ts-refclk). Generating: with has_ptp_refclk
     * the grandmaster identity and domain are written, and "traceable"
     * additionally when ptp_traceable (the grandmaster is locked to a
     * primary reference such as GPS). Parsing: has_ptp_refclk is set when
     * an identity was given, ptp_traceable when "traceable" was. */
    bool has_ptp_refclk;
    uint8_t ptp_grandmaster_id[8];
    uint8_t ptp_domain;
    bool ptp_traceable;

    /* Media clock offset (a=mediaclk) */
    bool has_mediaclk;
    uint32_t mediaclk_offset;

    /* Stream direction for SDP attribute (a=sendonly / a=recvonly) */
    bool is_source;             /* true = sendonly, false = recvonly */
} aes67_sdp_t;

/**
 * Generate an AES67-compliant SDP string.
 *
 * @param sdp       Populated SDP structure
 * @param buf       Output buffer for SDP text
 * @param buf_len   Size of output buffer
 * @return Number of bytes written, or -1 on error
 */
int aes67_sdp_generate(const aes67_sdp_t *sdp, char *buf, size_t buf_len);

/**
 * Parse an SDP string into an aes67_sdp_t structure.
 *
 * @param sdp_text  Null-terminated SDP text
 * @param sdp       Output structure
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG on parse failure
 */
esp_err_t aes67_sdp_parse(const char *sdp_text, aes67_sdp_t *sdp);

/**
 * Get the codec name string for a given codec enum.
 */
const char *aes67_codec_to_str(aes67_codec_t codec);

/**
 * Parse a codec name string to enum. Returns -1 on unknown codec.
 */
int aes67_codec_from_str(const char *name);

/**
 * True if name is 1..63 bytes of printable characters (no control
 * characters, no DEL). The rule for session and stream names taken from
 * the network or from an API client.
 */
bool aes67_name_is_clean(const char *name);

#ifdef __cplusplus
}
#endif
