#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Leon de Rooij (OSHLabs)
# SPDX-License-Identifier: Apache-2.0
"""Relay an AES67 multicast stream to another group, packets unchanged, and
announce the copy over SAP (RFC 2974), with a SAP deletion on exit.

A node does not receive the multicast it sends itself (its own frames do not
come back from the switch), so it cannot play its own source. Relaying that
source to a second group lets the node subscribe to it: with the example's
auto-subscribe, the node's microphone then plays on its own speaker, an echo
over the network. It also repeats a stream for a receiver that only sees
another group.

The announced SDP describes the stream (--encoding, --rate, --channels,
--ptime, --pt) and must match what the source sends; the relay does not look
inside the packets. Run it on a wired host on the node's LAN.

usage: aes67_relay.py <src_group> <dst_group> <local_ip> [--port 5004]
                      [--name NAME] [--encoding L24] [--rate 48000]
                      [--channels 2] [--ptime 1] [--pt 98] [--dscp 34]
"""
import argparse, socket, struct, time, zlib

SAP_GROUP, SAP_PORT = "239.255.255.255", 9875


def sap_packet(origin_ip, msg_id, sdp, delete=False):
    flags = 0x20 | (0x04 if delete else 0)   # V=1, IPv4, T=0/1, no auth
    hdr = struct.pack(">BBH", flags, 0, msg_id) + socket.inet_aton(origin_ip)
    return hdr + b"application/sdp\0" + sdp.encode()


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("src_group"); p.add_argument("dst_group"); p.add_argument("local_ip")
    p.add_argument("--port", type=int, default=5004, help="RTP port, same on both groups")
    p.add_argument("--name", default="relay", help="session name in the SAP announcement")
    p.add_argument("--encoding", default="L24", choices=["L16", "L24", "L32"])
    p.add_argument("--rate", type=int, default=48000)
    p.add_argument("--channels", type=int, default=2)
    p.add_argument("--ptime", default="1", help="a=ptime value, e.g. 0.125, 0.333, 1, 4")
    p.add_argument("--pt", type=int, default=98, help="RTP payload type")
    p.add_argument("--dscp", type=int, default=34, help="AES67 recommends 34 (AF41) for media")
    a = p.parse_args()

    rx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    rx.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    rx.bind((a.src_group, a.port))
    rx.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP,
                  socket.inet_aton(a.src_group) + socket.inet_aton(a.local_ip))
    rx.settimeout(0.5)

    tx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    tx.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_IF, socket.inet_aton(a.local_ip))
    tx.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 15)
    tx.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_LOOP, 0)
    tx.setsockopt(socket.IPPROTO_IP, socket.IP_TOS, a.dscp << 2)

    sdp = ("v=0\r\no=- %d 1 IN IP4 %s\r\ns=%s\r\nc=IN IP4 %s/15\r\nt=0 0\r\n"
           "m=audio %d RTP/AVP %d\r\na=rtpmap:%d %s/%d/%d\r\na=ptime:%s\r\n"
           "a=ts-refclk:ptp=IEEE1588-2008:traceable\r\na=mediaclk:direct=0\r\n"
           % (zlib.crc32(a.dst_group.encode()), a.local_ip, a.name, a.dst_group,
              a.port, a.pt, a.pt, a.encoding, a.rate, a.channels, a.ptime))
    msg_id = zlib.crc32(sdp.encode()) & 0xFFFF

    print("relaying %s -> %s:%d as \"%s\"" % (a.src_group, a.dst_group, a.port, a.name), flush=True)
    relayed = 0; next_sap = 0.0; next_log = time.time() + 10
    try:
        while True:
            now = time.time()
            if now >= next_sap:
                tx.sendto(sap_packet(a.local_ip, msg_id, sdp), (SAP_GROUP, SAP_PORT))
                next_sap = now + 5
            if now >= next_log:
                print("relayed %d packets" % relayed, flush=True)
                next_log = now + 60
            try:
                pkt = rx.recv(2048)
            except socket.timeout:
                continue
            tx.sendto(pkt, (a.dst_group, a.port))
            relayed += 1
    except KeyboardInterrupt:
        pass
    finally:
        tx.sendto(sap_packet(a.local_ip, msg_id, sdp, delete=True), (SAP_GROUP, SAP_PORT))
        print("relayed %d packets" % relayed, flush=True)


if __name__ == "__main__":
    main()
