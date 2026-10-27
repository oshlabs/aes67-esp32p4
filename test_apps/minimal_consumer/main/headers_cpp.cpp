/*
 * SPDX-FileCopyrightText: 2026 Leon de Rooij (OSHLabs)
 *
 * SPDX-License-Identifier: Apache-2.0
 */
/* Every public header must compile in a C++ translation unit. */
#include "aes67.h"
#include "aes67_config.h"
#include "aes67_audio.h"
#include "aes67_clock.h"
#include "aes67_convert.h"
#include "aes67_net.h"
#include "aes67_ptp.h"
#include "aes67_rtp.h"
#include "aes67_sap.h"
#include "aes67_sdp.h"
#include "aes67_session.h"
#include "aes67_webui.h"

extern "C" int aes67_headers_cpp_ok(void);
int aes67_headers_cpp_ok(void)
{
    aes67_config_t cfg = AES67_CONFIG_DEFAULT();
    return cfg.net.rtp_port == CONFIG_AES67_RTP_PORT;
}
