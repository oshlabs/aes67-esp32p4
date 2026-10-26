#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Leon de Rooij (OSHLabs)
# SPDX-License-Identifier: Apache-2.0
"""Join an AES67 multicast stream and report what is on the wire: packet rate,
sequence gaps, payload type, RTP timestamp rate, per-channel level of an L24
stereo payload, and the RTP timestamp offset against the AES67 media clock
(TAI seconds * sample rate, mod 2^32), using this host's clock (NTP-synced is
good enough to spot a seeding error; PTP-synced to judge sub-millisecond).

Run it on a wired host on the same LAN as the sender; a Wi-Fi client usually
receives no multicast at all.

usage: aes67_probe.py <group> <port> <local_ip> [seconds] [--rate 48000] [--tai-offset 37]
"""
import argparse, math, socket, struct, time


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("group"); p.add_argument("port", type=int); p.add_argument("local_ip")
    p.add_argument("seconds", type=float, nargs="?", default=5.0)
    p.add_argument("--rate", type=int, default=48000)
    p.add_argument("--tai-offset", type=float, default=37.0, help="TAI - UTC in seconds")
    a = p.parse_args()

    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("", a.port))
    s.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP,
                 struct.pack("4s4s", socket.inet_aton(a.group), socket.inet_aton(a.local_ip)))
    s.settimeout(1.0)

    t0 = time.time(); n = 0; gaps = 0; last_seq = None; frames = 0
    sq = [0.0, 0.0]; peak = [0, 0]; srcs = set(); sizes = set(); pts = set()
    first = None; last = None; clock_diffs = []
    while time.time() - t0 < a.seconds:
        try:
            d, addr = s.recvfrom(2048)
        except socket.timeout:
            continue
        now = time.time()
        n += 1; srcs.add(addr[0]); sizes.add(len(d)); pts.add(d[1] & 0x7F)
        seq = (d[2] << 8) | d[3]
        if last_seq is not None and (seq - last_seq) & 0xFFFF != 1:
            gaps += 1
        last_seq = seq
        ts = struct.unpack(">I", d[4:8])[0]
        if first is None:
            first = (now, ts)
        last = (now, ts)
        expected = int((now + a.tai_offset) * a.rate) & 0xFFFFFFFF
        clock_diffs.append((ts - expected + 2 ** 31) % 2 ** 32 - 2 ** 31)
        payload = d[12:]
        for i in range(0, len(payload) - 5, 6):
            for ch in (0, 1):
                v = int.from_bytes(payload[i + 3 * ch:i + 3 * ch + 3], "big", signed=True)
                sq[ch] += v * v; peak[ch] = max(peak[ch], abs(v))
            frames += 1

    print("packets %d in %.1f s (%.0f/s) from %s sizes %s payload types %s" % (
        n, a.seconds, n / a.seconds, sorted(srcs), sorted(sizes), sorted(pts)))
    if not n:
        return
    dt = last[0] - first[0]; dts = (last[1] - first[1]) & 0xFFFFFFFF
    print("sequence gaps %d; RTP timestamp advance %d samples over %.2f s = %.1f Hz" % (
        gaps, dts, dt, dts / dt if dt else 0))
    md = sorted(clock_diffs)[len(clock_diffs) // 2]
    print("RTP timestamp vs media clock (TAI*%d): median %+d samples = %+.3f s" % (a.rate, md, md / a.rate))
    print("  (small negative, about one packet, is normal: packets are stamped with their capture time)")
    for ch in (0, 1):
        rms = math.sqrt(sq[ch] / frames) if frames else 0
        print("ch%d rms %.1f dBFS peak %.1f dBFS" % (
            ch, 20 * math.log10(max(1, rms) / 8388608), 20 * math.log10(max(1, peak[ch]) / 8388608)))


if __name__ == "__main__":
    main()
