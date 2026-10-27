# Open work

What the code does today is in the README's "Status" table. This file lists
what is still planned, in rough priority order. Finished phases of the
original refactoring plan (the int32 internal format, stream parameters
computed once, codec conversion by function pointer, the output modes, the
web UI, the Ethernet receive hook and clock recovery) are described in the
README and in the code.

- **Playout alignment.** A sink starts playing at whatever buffer level the
  first packets leave and keeps that level (the audio clock follows PTP, so
  it no longer drifts). Align playout to RTP timestamp plus a link offset,
  dropping or inserting on a stream restart.
- **TDM output and channel routing beyond two slots.** The I2S output is a
  stereo codec; a multichannel DAC needs TDM slots and `routing[]` applied
  to all of them.
- **Remote found/lost events** on the default event loop (today: the SAP
  callback).
- **Per-stream playout delay and jitter buffers.** `playout_delay_us` and
  `jitter_buffer_ms` are stored and not used; the original plan's
  per-channel buffers with a configurable playout delay per stream.
- **Per-stream output mode**, so one sink plays on I2S while another feeds
  the application. The mode is per node today.
- **Audio metering** (RMS and peak per channel) on the page.
- **APLL reconfiguration per sample rate** (upstream tried and reverted it).
- **A finer frame-task grid.** The tick is the node's packet time; a source
  with a shorter packet time sends several packets per tick, which is
  correct but adds up to one tick of transmit jitter for that source.
- **44.1 kHz at 1 ms.** Not an integer frame count; pick RAVENNA's 48 frames
  at 1.0884 ms or alternate 44/45 and reject the rest at configuration time.
- **AM824** against a real device.
- **96 kHz.**
- **Several nodes per device**, or stating the single-instance limit in the
  API.
- **Hardware format conversion** with the ESP32-P4 BitScrambler attached to
  the I2S DMA (L24 big-endian to int32 without CPU cycles).
- **A newer ES8311 component** that uses `driver/i2c_master.h`; the example
  uses the legacy I2C driver because the codec component does.
