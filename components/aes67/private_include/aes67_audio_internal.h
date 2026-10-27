/*
 * SPDX-FileCopyrightText: 2026 Leon de Rooij (OSHLabs)
 *
 * SPDX-License-Identifier: Apache-2.0
 */
/* Audio driver details shared inside the component only */

#pragma once

#include "aes67_audio.h"
#include "driver/i2s_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** The I2S TX channel (the playback task writes to it in callback and
 *  stream-buffer output modes). */
i2s_chan_handle_t aes67_audio_get_tx_chan(aes67_audio_handle_t handle);

#ifdef __cplusplus
}
#endif
