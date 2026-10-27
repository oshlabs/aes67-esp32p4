#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Leon de Rooij (OSHLabs)
# SPDX-License-Identifier: Apache-2.0
"""Play an audio file as an AES67 stream: L24/48000/2, 1 ms packets (48
frames), payload type 98, RTP timestamps anchored to the AES67 media clock
(TAI seconds * 48000 mod 2^32) from this host's clock, announced over SAP
(RFC 2974) every 5 s, with a SAP deletion when the file ends or on Ctrl-C.

Any format ffmpeg reads is decoded with ffmpeg first; --raw takes a file that
already is big-endian signed 24-bit PCM, 48 kHz stereo, e.g. made with
`ffmpeg -i in.mp3 -f s24be -ar 48000 -ac 2 out.pcm`. A node that
auto-subscribes picks the stream up and plays it, which makes this the
listening test for a sink. Run on a wired, NTP- or PTP-synced host.

usage: aes67_play.py <file> <group> <local_ip> [--raw] [--port 5004]
                     [--name NAME] [--dscp 34] [--tai-offset 37]
"""
import argparse, random, socket, struct, subprocess, time, zlib

SPP = 48; RATE = 48000; PT = 98; FRAME_BYTES = 6   # 2 channels * 3 bytes
SAP_GROUP, SAP_PORT = "239.255.255.255", 9875


def sap_packet(origin_ip, msg_id, sdp, delete=False):
    flags = 0x20 | (0x04 if delete else 0)   # V=1, IPv4, T=0/1, no auth
    hdr = struct.pack(">BBH", flags, 0, msg_id) + socket.inet_aton(origin_ip)
    return hdr + b"application/sdp\0" + sdp.encode()


def load_pcm(path, raw):
    if raw:
        with open(path, "rb") as f:
            return f.read()
    return subprocess.run(["ffmpeg", "-loglevel", "error", "-i", path,
                           "-f", "s24be", "-ar", str(RATE), "-ac", "2", "-"],
                          check=True, stdout=subprocess.PIPE).stdout


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("file"); p.add_argument("group"); p.add_argument("local_ip")
    p.add_argument("--raw", action="store_true", help="file is s24be/48000/2 PCM, skip ffmpeg")
    p.add_argument("--port", type=int, default=5004)
    p.add_argument("--name", default=None, help="session name (default: the file name)")
    p.add_argument("--dscp", type=int, default=34, help="AES67 recommends 34 (AF41) for media")
    p.add_argument("--tai-offset", type=float, default=37.0)
    a = p.parse_args()
    name = a.name or a.file.rsplit("/", 1)[-1]

    chunk = SPP * FRAME_BYTES
    data = load_pcm(a.file, a.raw)
    data = data[: len(data) - len(data) % chunk]
    total = len(data) // chunk

    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_IF, socket.inet_aton(a.local_ip))
    s.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 15)
    s.setsockopt(socket.IPPROTO_IP, socket.IP_TOS, a.dscp << 2)

    sdp = ("v=0\r\no=- %d 1 IN IP4 %s\r\ns=%s\r\nc=IN IP4 %s/15\r\nt=0 0\r\n"
           "m=audio %d RTP/AVP %d\r\na=rtpmap:%d L24/%d/2\r\na=ptime:1\r\n"
           "a=ts-refclk:ptp=IEEE1588-2008:traceable\r\na=mediaclk:direct=0\r\n"
           % (zlib.crc32(a.group.encode()), a.local_ip, name, a.group, a.port, PT, PT, RATE))
    msg_id = zlib.crc32(sdp.encode()) & 0xFFFF

    ssrc = random.getrandbits(32); seq = random.getrandbits(16)
    start = time.time(); sent = 0; next_sap = start
    ts0 = int((start + a.tai_offset) * RATE) & 0xFFFFFFFF
    ts0 -= ts0 % SPP
    print("playing %.1f s to %s:%d as \"%s\"" % (total * SPP / RATE, a.group, a.port, name), flush=True)
    try:
        while sent < total:
            now = time.time()
            if now >= next_sap:
                s.sendto(sap_packet(a.local_ip, msg_id, sdp), (SAP_GROUP, SAP_PORT))
                next_sap = now + 5
            due = start + sent * SPP / RATE
            if now < due:
                time.sleep(due - now)
                continue
            ts = (ts0 + sent * SPP) & 0xFFFFFFFF
            hdr = struct.pack(">BBHII", 0x80, PT, seq & 0xFFFF, ts, ssrc)
            s.sendto(hdr + data[sent * chunk:(sent + 1) * chunk], (a.group, a.port))
            seq += 1; sent += 1
    except KeyboardInterrupt:
        pass
    finally:
        s.sendto(sap_packet(a.local_ip, msg_id, sdp, delete=True), (SAP_GROUP, SAP_PORT))
        print("sent %d packets, ssrc %u" % (sent, ssrc), flush=True)


if __name__ == "__main__":
    main()
