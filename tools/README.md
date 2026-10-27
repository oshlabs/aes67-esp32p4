# Verification tools

Plain-Python helpers (standard library only) for checking a node against a real
AES67 receiver. They were written while bringing the example up against a
Neumann KH 120 II AES67 monitor and are the verification recipe referred to by
commit messages.

| Script | Purpose |
|---|---|
| `aes67_probe.py` | Join a multicast stream on a wired host: packet rate, sequence gaps, payload type, per-channel level, and RTP timestamp offset against the AES67 media clock (TAI * rate). |
| `aes67_send.py` | Reference sender (L24/48k/2, 1 ms, tone) with correct timestamps. Proves a receiver's path, and bisects DSCP/TTL questions. |
| `aes67_play.py` | Play an audio file (anything ffmpeg reads, or raw s24be PCM) as an L24/48k/2 stream with SAP, timestamps on the media clock. A node that auto-subscribes plays it: the listening test for a sink. |
| `aes67_relay.py` | Relay a multicast stream to another group, packets unchanged, and announce the copy over SAP. A node cannot receive its own multicast; relayed, it can play its own source (with auto-subscribe: the microphone on its own speaker). |
| `aes67_receiver_ctl.py` | Subscribe, inspect and capture on a Coveloz/BACH-based receiver (Neumann KH series) through its JSON API. |

## Typical session

```bash
# What is on the wire from the node (run on a wired host; local_ip is that host's address)
tools/aes67_probe.py 239.69.0.1 5004 172.31.0.9 6

# Is the receiver locked to PTP and does it see the node's SAP announcement?
tools/aes67_receiver_ctl.py -s 172.31.1.37 clock
tools/aes67_receiver_ctl.py -s 172.31.1.37 advertised

# Subscribe the receiver to the node's stream and watch its receive state
tools/aes67_receiver_ctl.py -s 172.31.1.37 subscribe --advertised "ESP32-P4 Mic"
tools/aes67_receiver_ctl.py -s 172.31.1.37 counters "ESP32-P4 Mic"

# Control experiment: a known-good stream from a wired host
tools/aes67_send.py 239.69.1.10 5004 172.31.0.9 --seconds 120 &
```

A receiver of this family reports reception as a finite `networkdelay` and no
"packets arriving too late" alarm. A stream whose RTP timestamps are not anchored
to the media clock produces the alarm and silence even though every packet
arrives, which is the first thing `aes67_probe.py` makes visible.
