#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Leon de Rooij (OSHLabs)
# SPDX-License-Identifier: Apache-2.0
"""Reference AES67 sender for receiver testing: L24/48000/2, 1 ms packets
(48 frames), payload type 98, RTP timestamps anchored to the AES67 media clock
(TAI seconds * 48000 mod 2^32) from this host's clock, a 440 Hz tone at
-20 dBFS on both channels.

Use it to prove a receiver's path works before blaming a device under test,
and to bisect header fields (DSCP, TTL) by running it with the DUT's values.
Run on a wired, NTP- or PTP-synced host.

usage: aes67_send.py <group> <port> <local_ip> [--dscp 34] [--ttl 15] [--seconds 60]
"""
import argparse, math, random, socket, struct, time

SPP = 48; RATE = 48000; PT = 98


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("group"); p.add_argument("port", type=int); p.add_argument("local_ip")
    p.add_argument("--dscp", type=int, default=34, help="AES67 recommends 34 (AF41) for media")
    p.add_argument("--ttl", type=int, default=15)
    p.add_argument("--seconds", type=float, default=60.0)
    p.add_argument("--tai-offset", type=float, default=37.0)
    a = p.parse_args()

    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind((a.local_ip, a.port))
    s.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_IF, socket.inet_aton(a.local_ip))
    s.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, a.ttl)
    s.setsockopt(socket.IPPROTO_IP, socket.IP_TOS, a.dscp << 2)

    ssrc = random.getrandbits(32); seq = random.getrandbits(16)
    amp = int(0.1 * 8388607)
    tone = [amp * math.sin(2 * math.pi * 440 * n / RATE) for n in range(RATE)]

    def l24(v):
        return int(v).to_bytes(3, "big", signed=True)

    payloads = [b"".join(l24(tone[n]) + l24(tone[n]) for n in range(i, i + SPP)) for i in range(0, RATE, SPP)]

    start = time.time(); sent = 0
    ts0 = int((start + a.tai_offset) * RATE) & 0xFFFFFFFF
    ts0 -= ts0 % SPP
    next_t = start
    while time.time() - start < a.seconds:
        now = time.time()
        if now < next_t:
            time.sleep(next_t - now)
            continue
        ts = (ts0 + sent * SPP) & 0xFFFFFFFF
        hdr = struct.pack(">BBHII", 0x80, PT, seq & 0xFFFF, ts, ssrc)
        s.sendto(hdr + payloads[sent % len(payloads)], (a.group, a.port))
        seq += 1; sent += 1
        next_t = start + sent * 0.001
    print("sent %d packets, ssrc %u, dscp %d, ttl %d" % (sent, ssrc, a.dscp, a.ttl))


if __name__ == "__main__":
    main()
