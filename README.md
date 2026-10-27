# AES67/RAVENNA for ESP32-P4

An ESP-IDF component implementing the AES67 (and RAVENNA-compatible) audio-over-IP standard on the ESP32-P4, based on patterns from the [aes67-linux-daemon](https://github.com/bondagit/aes67-linux-daemon) reference implementation.

## Performance

- **0.7ms end-to-end latency** with ptime=0.125ms source (6 frames at 48kHz)
- **100% DMA utilization** -- zero underruns at up to 8000 pps
- **0 packet loss** with IEEE 802.3x flow control
- Tested at 645,000+ packets with zero drops or sequence gaps
- ISR-driven playback at up to 3000Hz with zero task scheduling overhead
- PTP offset to a GPS grandmaster over UDP within +/-0.85 us (hardware timestamps);
  audio clock locked to PTP within +/-11 us of phase

### Latency by Source Ptime

| Source ptime | Packets/sec | ESP32 latency | Total end-to-end |
|---|---|---|---|
| 0.125ms (6 frames) | 8000 | 0.4ms | ~0.7ms |
| 0.333ms (16 frames) | 3000 | 0.5ms | ~1.0ms |
| 1ms (48 frames) | 1000 | 0.6ms | ~1.7ms |
| 4ms (192 frames) | 250 | 0.6ms | ~2.7ms |

### Latency Breakdown (ptime=0.125ms, best case)

| Stage | Time |
|---|---|
| Source ptime buffering | 0.125ms |
| Network (LAN) | 0.1ms |
| Ethernet receive hook (parse + convert) | 0.1ms |
| Stream buffer | 0.06ms |
| DMA ring (2 x 16 frames) | 0.33ms |
| **Total** | **~0.72ms** |

## Status

What the code does today, as opposed to what is planned. "Partial" items
say what is missing.

| Area | State | Note |
|---|---|---|
| PTP timeReceiver, hardware timestamps on receive (L2 and UDP) and on transmit | implemented | lock reported from the filtered offset; GPS grandmaster: sub-microsecond |
| PTP grandmaster when no better clock is present | implemented | `config.ptp.prefer_grandmaster` |
| RTP sources (TX) at 0.125 to 4 ms, L16/L24/L32, 1 to 8 channels on the wire | implemented | capture is the codec's two slots; wider sources carry silence in channels 2+ |
| RTP sinks (RX) at 0.125 to 4 ms, L16/L24/L32, up to 8 channels | implemented | output is two I2S slots chosen by `routing[]`; TDM output is planned (no multichannel DAC yet) |
| AM824 | partial | conversion functions exist, untested against a device |
| Transmit timing on the media clock's frame grid, timestamps anchored to PTP | implemented | |
| Audio clock recovery (APLL steered to PTP) | implemented | see below |
| Playout alignment of a sink to RTP timestamp plus link offset | planned | playout starts at whatever level the first packets left; the level then stays constant |
| SAP announce/discover per RFC 2974 | implemented | 30 s interval, expiry from the observed interval |
| SDP generate/parse (ts-refclk with the real grandmaster, framecount, mediaclk) | implemented | host-tested, see `test/` |
| mDNS `_ravenna._udp` | implemented | no RTSP server, so no `_rtsp._tcp`; RAVENNA controllers that need RTSP DESCRIBE cannot fetch SDPs |
| Output modes: I2S, callback, stream buffer | implemented | callback and stream buffer go through the playback task |
| Events on the default event loop (PTP lock, sources/sinks added/removed, sink receiving/timeout) | implemented | remote found/lost events are not posted; use the SAP callback |
| Web UI and REST API | implemented | token optional, see "Security model" |
| Sample rates 44.1 kHz | partial | 1 ms at 44.1 kHz is not an integer frame count; 48 kHz is the tested rate |
| 96 kHz | not implemented | |
| Several nodes per device | not implemented | the audio driver, frame timer, Ethernet hook and clock loop are single instance |

## Features

- **PTP (IEEE 1588v2)** clock synchronization using ESP32-P4 EMAC hardware timestamping,
  on receive for both PTP over Ethernet and PTP over UDP/IPv4 (the Ethernet input hook
  hands UDP PTP frames to the daemon with the driver's timestamp), and on transmit for
  delay requests on either transport
  - timeReceiver mode: synchronizes to an external PTP grandmaster
  - Grandmaster mode: becomes the timeTransmitter when no other grandmaster is present
  - Role names follow IEEE 1588g-2022 (timeTransmitter / timeReceiver); the
    vendored daemon keeps the protocol's own field names
  - Lock state from the low-pass filtered offset, with hysteresis; status reports
    transport, grandmaster identity and clock class, offset, jitter, drift, path delay
- **RTP audio streaming** with ISR-driven DMA playback
  - Source (TX) and Sink (RX) streams
  - L16, L24, L32, AM824 codec support with MTConvert function pointer dispatch
  - Channels: 1-8 per stream on the wire; the I2S output is two slots, fed by
    the sink's `routing[]` (defaults: channels 0 and 1; a mono stream feeds both)
  - Sample rate: 48kHz (44.1kHz at frame counts that divide evenly)
  - Packet time: 0.125ms to 4ms (6 to 192 frames); each source is paced on its own
    frame grid, timestamps anchored to the PTP media clock
  - Handles 250 to 8000+ packets per second
  - A sink follows a new SSRC after two seconds of silence (source restarts)
  - DSCP/QoS marking for network prioritization
  - Ethernet receive hook inside the component: RTP for active sinks and PTP over
    UDP are taken off the driver's input path before lwIP, matched per sink on
    group, port and payload type, locked to one SSRC, with per-sink statistics
  - IEEE 802.3x flow control prevents EMAC multicast frame drops
- **ISR-driven playback** -- zero task scheduling overhead
  - DMA ISR reads directly from stream buffer at up to 3000Hz
  - Sample-hold on underrun (smooth decay instead of silence clicks)
  - 2 DMA descriptors at 16 frames = 0.33ms ring latency
- **Flexible output routing** per node
  - I2S direct output to DAC (ISR-driven, default)
  - User callback with int32 sample buffers
  - FreeRTOS StreamBuffer for application-level processing
- **SAP (RFC 2974)** session announcement and discovery
  - Announcements every 30 s (two quick repeats on creation); remote sources expire
    after ten times their observed interval
  - Remote source discovery callback (the example auto-subscribes)
- **SDP** generation and parsing (AES67-compliant)
  - `a=ts-refclk` names the grandmaster the node is locked to (and `traceable` when
    its clock class says so); re-announced on PTP lock changes
  - `a=mediaclk`, `a=framecount`, fractional ptime (0.333 ms is 16 frames)
  - Parser accepts the descriptions of Dante, SIENNA and the Linux daemon (host-tested)
- **Web UI** (optional): live page with the two clocks, sources, sinks and discovery
  over a WebSocket, a REST API, a registry for runtime controls the application
  registers (the example: mic gain, volume), header-, token- and rate-limited writes
- **mDNS/DNS-SD** device advertisement (`_ravenna._udp`, TXT from the configuration; no
  `_rtsp._tcp`, as no RTSP server runs here, streams are found over SAP)
- **I2S audio** driver with APLL clock
  - Standard I2S Philips mode, 32-bit slots
  - APLL for exact 48kHz (18.432MHz MCLK)
  - Clock recovery: the APLL is steered to PTP time (see below)
- **Optimized for real-time**
  - -O2 compiler optimization
  - IRAM on all hot-path conversion functions
  - lwIP IRAM optimizations
  - IPv6 disabled (saves 39KB flash, reduces packet processing)
  - Audio and network tasks pinned to core 0; the clock-recovery writer on core 1
  - Nothing logs from the frame task; periodic status lines are rate limited

## Hardware

The example targets the **ESP32-P4-NANO** board with:

| Function | GPIO | Notes |
|---|---|---|
| Ethernet MDC | 31 | |
| Ethernet MDIO | 52 | |
| Ethernet REF_CLK | 50 | External 50MHz crystal |
| Ethernet PHY_RST | 51 | |
| I2S MCLK | 13 | 18.432MHz via APLL |
| I2S BCLK | 36 | External DAC (onboard: 12) |
| I2S LRCLK | 33 | External DAC (onboard: 10) |
| I2S DOUT | 32 | External DAC (onboard: 9) |
| I2S DIN | 11 | Codec ADC input |
| PA Enable | 53 | Active high |
| I2C SCL | 8 | ES8311 control |
| I2C SDA | 7 | ES8311 control |

- Ethernet PHY: IP101 GRI via RMII
- Onboard audio codec: ES8311 + NS4150B amplifier (mono ADC, mono DAC)
- External I2S DAC supported: select it and its pins under `idf.py menuconfig`
  -> "AES67 Example"

The Waveshare **ESP32-P4-ETH** (with or without the PoE module) has the same
Ethernet and codec pinout and works unchanged.

## Build

Requires ESP-IDF v5.5.2+ with the ESP32-P4 target.

```bash
idf.py set-target esp32p4
idf.py build
idf.py flash monitor
```

Boards with ESP32-P4 silicon revision **v3.x** (newer ESP32-P4-ETH units report
"revision v3.2" in `idf.py flash`) need ESP-IDF v5.5.2 or later and a build that
selects chip revision v3.0 as minimum; such a build does not run on v1.x chips.
Configure with the overlay once, the choice is cached in the build directory:

```bash
idf.py -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.p4v3" set-target esp32p4
```

`tools/` holds Python helpers to verify a node against real AES67 devices (wire
probe, reference sender, receiver control, SAP announcer, API churn test); see
`tools/README.md`. `test/` has host tests and a minimal consumer project; see
`test/README.md`.

## Component Architecture

```
components/aes67/
  include/
    aes67.h             Top-level init/lifecycle API
    aes67_config.h      Configuration structs, stream params, codec utilities
    aes67_ptp.h         PTP clock synchronization
    aes67_rtp.h         RTP packet engine
    aes67_sap.h         SAP announcement/discovery
    aes67_sdp.h         SDP parser/generator
    aes67_session.h     Session manager (sources/sinks)
    aes67_audio.h       I2S audio driver with APLL and ISR playback
    aes67_clock.h       Audio clock recovery (APLL steered to PTP)
    aes67_convert.h     Sample format conversion (MTConvert pattern)
    aes67_net.h         Network utilities
    aes67_webui.h       Web UI: HTTP, WebSocket, REST, runtime controls
  private_include/      Ethernet receive hook, frame timer, mDNS, driver internals
  src/
    aes67.c             Node lifecycle, frame task (TIC), output routing
    aes67_ptp.c         PTP wrapping ptpd + esp_eth_time
    aes67_rtp.c         RTP engine: frame-path decoder, per-sink matching, transmit pacing
    aes67_eth_rx.c      Ethernet receive hook (PTP and RTP before lwIP)
    aes67_hw_timer.c    PTP-driven frame timer on the media clock's frame grid
    aes67_clock.c       Audio clock recovery (APLL steered to PTP)
    aes67_sap.c         SAP multicast announcements
    aes67_sdp.c         SDP text generation/parsing
    aes67_session.c     Stream orchestration
    aes67_audio.c       I2S DMA driver with ISR-driven playback and capture
    aes67_convert.c     L16/L24/L32/AM824 format conversion (IRAM)
    aes67_net.c         Multicast socket helpers
    aes67_mdns.c        mDNS service registration
    aes67_webui.c       Web UI
  web/index.html        The page (gzipped into the build)
  third_party/          Vendored PTP daemon and EMAC time wrapper (see PATCHES.md)
```

## Configuration

All parameters are configurable via `idf.py menuconfig` under "AES67 RAVENNA Component".
Every option listed there is read by the code; this is what each one does:

| Option | Effect |
|---|---|
| `AES67_MAX_SOURCES`, `AES67_MAX_SINKS`, `AES67_MAX_CHANNELS_PER_STREAM` | Table sizes; the channel limit also sizes the engine's scratch buffers |
| `AES67_DEFAULT_SAMPLE_RATE`, `_PACKET_TIME_US`, `_WORD_LENGTH`, `_CHANNELS` | `AES67_CONFIG_DEFAULT()` audio settings; the packet time is also the frame task's tick |
| `AES67_RTP_PORT`, `AES67_RTP_MCAST_BASE` | Default RTP port and the first multicast group for local sources |
| `AES67_SAP_ENABLED`, `AES67_MDNS_ENABLED` | Defaults for `config.sap_enabled` / `config.mdns_enabled` |
| `AES67_PTP_DOMAIN`, `AES67_PTP_PREFER_GRANDMASTER` | Defaults for `config.ptp`; passed to the PTP daemon at start (domain, permission to act as master) |
| `AES67_JITTER_BUFFER_MS`, `AES67_AUDIO_RING_BUFFER_MS`, `AES67_USE_PSRAM` | Sink playout delay, audio buffer sizing, PSRAM for large buffers |
| `AES67_TASK_PRIORITY_PTP`, `_RTP`, `_AUDIO`, `_SAP` | FreeRTOS priorities of the PTP monitor, the socket receive task, the frame/playback tasks and the SAP tasks |
| `AES67_NODE_NAME`, `AES67_I2S_PORT`, `AES67_I2S_*_GPIO` | Node name for SDP/mDNS, I2S peripheral and pins |
| `AES67_PTP_LOCK_THRESHOLD_NS` | Filtered offset below which the node reports PTP "locked" |
| `AES67_CLOCK_RECOVERY`, `AES67_CLOCK_MAX_CORRECTION_PPM` | Audio clock steering to PTP and its clamp |
| `AES67_WEBUI_ENABLED`, `AES67_WEBUI_PORT`, `AES67_WEBUI_TOKEN`, `AES67_WEBUI_PUSH_INTERVAL_MS` | Web UI on/off (in the example), its port, control token and status push rate |

Runtime fields of `aes67_config_t` beyond those:

- `config.net.eth_handle`: the Ethernet driver handle (required).
- `config.net.interface_name`: the esp_netif key of your Ethernet interface,
  default `"ETH_DEF"` (what `ESP_NETIF_DEFAULT_ETH()` creates). Node init fails
  with a clear error if no netif has that key.
- `config.sap_interval_s` / `config.sap_min_expiry_s`: own SAP announce interval
  (default 30 s, plus two quick repeats after creation) and the shortest time a
  discovered source is kept without an announcement (default 60 s; otherwise ten
  times the interval it announces at, at most an hour).
- `config.eth_rx_hook`: take PTP and RTP off the Ethernet input path before lwIP
  (default on; off uses sockets, with higher latency and software PTP timestamps).
- `config.output_mode`, `config.audio_cb`: where received audio goes (I2S, a
  callback, or a stream buffer the application reads).

## Security model

The node is built for a trusted audio network: a studio or venue LAN where
every host may already inject multicast audio and PTP. Within that model the
web UI defends against the realistic accidents and against a browser on that
LAN being turned against the device:

- Everything the device receives from the network (SAP session names, SDP,
  API bodies) is treated as data. Names are reduced to printable characters,
  JSON output is escaped, and the page renders text, never markup.
- Reads (`GET /api/*`, the WebSocket status) are open.
- Writes (`POST /api/sinks`, `DELETE /api/sinks/N`, controls) require the
  `X-AES67-Request: 1` header, which a browser does not send cross-origin
  without a CORS preflight, so a hostile page elsewhere cannot drive the API
  through your browser. Writes are rate limited (bursts of 5, one per second
  after) and logged with the client address.
- Optionally, writes require `Authorization: Bearer <token>` with the token
  from `CONFIG_AES67_WEBUI_TOKEN`. The page asks for it once per browser
  session. Set it whenever the device is reachable from outside the audio LAN.

Not defended: a host on the LAN that can already send multicast can still
announce misleading sessions or flood the PTP domain; there is no TLS, so the
token travels in clear on that LAN; and the firmware has no notion of users.
Put the device behind a firewall if any of that matters in your deployment.

## Usage

```c
#include "aes67.h"
#include "aes67_session.h"

/* Configure and start. The Ethernet driver must be installed and attached
 * to an esp_netif whose key is cfg.net.interface_name ("ETH_DEF", what
 * ESP_NETIF_DEFAULT_ETH() creates); set interface_name if yours differs. */
aes67_config_t cfg = AES67_CONFIG_DEFAULT();
cfg.net.eth_handle = my_eth_handle;

aes67_node_handle_t node;
aes67_node_init(&cfg, &node);
aes67_node_start(node);

/* Add a 2-channel L24 source */
aes67_session_handle_t session;
aes67_node_get_session(node, &session);

uint8_t id;
aes67_session_add_source(session, "My Source", 2, AES67_CODEC_L24, 0 /* default ptime */, &id);

/* Add a sink from discovered SDP */
aes67_session_add_sink(session, "My Sink", remote_sdp_text, &id);
```

### Callback output mode

```c
void my_audio_cb(const int32_t *samples, uint32_t frame_count,
                 uint8_t channels, uint8_t word_length,
                 uint32_t sample_rate, void *user_data) {
    /* Process int32 left-justified samples */
}

aes67_config_t cfg = AES67_CONFIG_DEFAULT();
cfg.output_mode = AES67_OUTPUT_CALLBACK;
cfg.audio_cb = my_audio_cb;
cfg.audio_cb_user_data = NULL;
```

## Data Flow

```
Playback (ISR-driven, default):
  Network -> Ethernet receive hook (component) -> RTP engine: validate,
          match sink, decode L16/L24/L32 to int32, route to 2 slots
          -> audio driver playout StreamBuffer -> DMA ISR (up to 3000Hz)
          -> I2S -> DAC

  In callback / stream-buffer output modes the engine's own buffer feeds
  the playback task instead of the DMA ISR.

  The DMA ISR fires when a descriptor finishes playing, reads from
  the stream buffer, and fills the next descriptor. Two descriptors
  alternate: one plays while the other is filled.

Capture:
  ADC -> I2S RX DMA -> capture StreamBuffer -> frame task (TIC, one tick per
  packet period on the PTP frame grid) -> engine: per-source rings -> RTP TX,
  each source paced by its own packet time -> network

Timing:
  PTP clock -> hardware timer ISR -> audio frame task (TIC) -> RTP TX
  APLL clock -> I2S DMA -> 48kHz sample-accurate playback
  I2S DMA ISR (frames played, PTP time) -> clock loop -> APLL trim
```

### Audio clock recovery

The I2S master clock comes from the APLL, which has the crystal's error
(tens of ppm); PTP time comes from the network. Without correction a
receiver's playout buffer slowly fills or drains and a sender's stream
runs at not quite 48000 frames per PTP second. The clock loop
(`aes67_clock.c`) closes that gap:

- the TX DMA ISR records, once per descriptor, how many frames the DAC has
  played and what PTP time it was;
- ten times a second a PI controller computes the phase error (frames
  played minus frames PTP time says there should be) and trims the APLL's
  sigma-delta word so the error goes to zero. Capture shares the master
  clock and inherits the lock;
- the correction is clamped (`CONFIG_AES67_CLOCK_MAX_CORRECTION_PPM`,
  default 200) and slew-limited to 2 ppm per update, so pull-in is
  inaudible; one modulator step is about 1.7 ppm;
- while PTP is not locked the last correction is held (holdover), and a
  step in PTP time only re-anchors the phase reference.

`aes67_node_get_clock_status()` (and `GET /api/clock`, and the "Audio
clock" tile of the web page) report the state (free, locking, locked,
holdover), the applied correction, the phase error, the residual rate error
and the number of playout dropouts. `CONFIG_AES67_CLOCK_RECOVERY=n` leaves the
audio clock free-running.

## Key Design Decisions

- **Ethernet receive hook** (in the component, installed by the node) intercepts RTP and UDP PTP before lwIP, avoiding all TCP/IP stack overhead; disable with `config.eth_rx_hook = false` to use the socket path
- **ISR-driven playback** eliminates task scheduling jitter that caused 10% throughput loss with task-based approach
- **IEEE 802.3x flow control** prevents the ESP32-P4 EMAC from dropping multicast frames under load (8% loss without it)
- **Sample-hold on underrun** produces smooth decay instead of audible silence clicks
- **StreamBuffer byte-stream** handles any packet time (0.125ms to 4ms) transparently by accumulating bytes
- **Audio and network tasks on core 0** avoids cross-core L1 cache visibility issues on ESP32-P4 dual RISC-V; the clock-recovery task runs on core 1 so its register writes never hold off the I2S interrupt
- **Frame ticks reconstructed from PTP time**: the timer interrupt only wakes the frame task, which works out from the clock how many packet periods have passed, so a wake-up lost to a busy core never loses a packet
- **One decoder for the receive path**: the engine decodes from the driver's input path, so sinks are matched and locked before anything is decoded; the socket path is the fallback, not a second implementation
- **DMA descriptors sized to 16 frames** (0.33ms) balances latency vs stability across all ptimes
