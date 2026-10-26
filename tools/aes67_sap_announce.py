#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Leon de Rooij (OSHLabs)
# SPDX-License-Identifier: Apache-2.0
"""Announce fake AES67 sessions over SAP (RFC 2974) to exercise a node's
discovery table and web UI: N sessions with long or awkward names, repeated
every few seconds, with SAP deletions on exit.

usage: aes67_sap_announce.py <local_ip> [--count 16] [--name-len 63]
                             [--quotes] [--seconds 60] [--interval 5]
                             [--group 239.69.200.1] [--channels 8] [--name NAME]
                             [--ptime 1]

With --count 1, --group, --channels and --name it announces one real session,
e.g. the stream aes67_send.py is sending, so a node that auto-subscribes picks
it up.

--quotes puts '"', '\\' and a single quote into every name, which a JSON or
HTML builder must escape. Run on a wired host on the node's LAN.
"""
import argparse, socket, struct, time, zlib

SAP_GROUP, SAP_PORT = "239.255.255.255", 9875


def sap_packet(origin_ip, msg_id, sdp, delete=False):
    flags = 0x20 | (0x04 if delete else 0)   # V=1, IPv4, T=0/1, no auth
    hdr = struct.pack(">BBH", flags, 0, msg_id) + socket.inet_aton(origin_ip)
    return hdr + b"application/sdp\0" + sdp.encode()


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("local_ip")
    p.add_argument("--count", type=int, default=16)
    p.add_argument("--name-len", type=int, default=63)
    p.add_argument("--quotes", action="store_true")
    p.add_argument("--seconds", type=float, default=60)
    p.add_argument("--interval", type=float, default=5)
    p.add_argument("--group", default=None, help="multicast group for session 0 (others count up from it)")
    p.add_argument("--channels", type=int, default=8)
    p.add_argument("--name", default=None, help="session name (default: fake-NN-xxx...)")
    p.add_argument("--ptime", default="1", help="a=ptime value, e.g. 0.125, 0.333, 1, 4")
    a = p.parse_args()

    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_IF, socket.inet_aton(a.local_ip))
    s.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 15)

    sessions = []
    base_group = [int(x) for x in (a.group or "239.69.200.0").split(".")]
    for i in range(a.count):
        base = ('fake"\\\'' if a.quotes else "fake") + "-%02d-" % i
        name = a.name if (a.name and a.count == 1) else (base + "x" * a.name_len)[:a.name_len]
        g = base_group[:]
        g[3] = (g[3] + (i if a.group else i + 1)) & 0xFF
        group = ".".join(str(x) for x in g)
        sdp = ("v=0\r\no=- %d 1 IN IP4 %s\r\ns=%s\r\nc=IN IP4 %s/15\r\nt=0 0\r\n"
               "m=audio 5004 RTP/AVP 98\r\na=rtpmap:98 L24/48000/%d\r\na=ptime:%s\r\n"
               "a=ts-refclk:ptp=IEEE1588-2008:traceable\r\na=mediaclk:direct=0\r\n"
               % (100000 + i, a.local_ip, name, group, a.channels, a.ptime))
        sessions.append((zlib.crc32(sdp.encode()) & 0xFFFF, sdp))

    t0 = time.time(); rounds = 0
    try:
        while time.time() - t0 < a.seconds:
            for msg_id, sdp in sessions:
                s.sendto(sap_packet(a.local_ip, msg_id, sdp), (SAP_GROUP, SAP_PORT))
            rounds += 1
            time.sleep(a.interval)
    finally:
        for msg_id, sdp in sessions:
            s.sendto(sap_packet(a.local_ip, msg_id, sdp, delete=True), (SAP_GROUP, SAP_PORT))
    print("announced %d sessions for %d rounds, then deleted them" % (len(sessions), rounds))


if __name__ == "__main__":
    main()
