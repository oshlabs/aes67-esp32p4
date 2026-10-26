#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Leon de Rooij (OSHLabs)
# SPDX-License-Identifier: Apache-2.0
"""Control and inspect an AES67 receiver that speaks the Coveloz/BACH JSON API
(Neumann KH 120 II / KH 150 AES67 monitors, among others).

The device exposes `POST http://<host>/jsonapi/` with a body
`{"command": "<cmd>", "json": {...}}` and no authentication.

Examples:
  aes67_receiver_ctl.py -s 172.31.1.37 info
  aes67_receiver_ctl.py -s 172.31.1.37 clock
  aes67_receiver_ctl.py -s 172.31.1.37 advertised          # SAP-discovered senders
  aes67_receiver_ctl.py -s 172.31.1.37 streams             # configured RX streams
  aes67_receiver_ctl.py -s 172.31.1.37 counters "ESP32-P4 Mic"
  aes67_receiver_ctl.py -s 172.31.1.37 subscribe --advertised "ESP32-P4 Mic"
  aes67_receiver_ctl.py -s 172.31.1.37 subscribe --sdp stream.sdp --name "lab tone"
  aes67_receiver_ctl.py -s 172.31.1.37 unsubscribe "ESP32-P4 Mic"
  aes67_receiver_ctl.py -s 172.31.1.37 raw get_system_info
  aes67_receiver_ctl.py -s 172.31.1.37 pcap 5             # capture on eth0, print URL

Reception is indicated by a finite `networkdelay` (ns) and the absence of the
"Network Delay is greater than the link offset" alarm; `rx_pkts` never moves on
the KH 120 II firmware v4.4.14. The monitor has a single output channel, so only
one RX stream can be enabled at a time; a paused stream still holds the channel.
"""
import argparse, json, sys, urllib.request


class Receiver:
    def __init__(self, host):
        self.url = "http://%s/jsonapi/" % host
        self.host = host

    def call(self, cmd, args=None):
        body = json.dumps({"command": cmd, "json": args or {}}).encode()
        req = urllib.request.Request(self.url, data=body,
                                     headers={"Content-Type": "application/json"})
        with urllib.request.urlopen(req, timeout=10) as r:
            return json.load(r)

    def streams(self):
        return self.call("get_destination_session_list")["list"]

    def find(self, name):
        for s in self.streams():
            if s["name"] == name:
                return s
        sys.exit("no RX stream named %r; configured: %s" % (name, [s["name"] for s in self.streams()]))


def cmd_info(rx, a):
    i = rx.call("get_system_info")["systemInfo"]
    print(i["product"], i["fw-version"], "eth:", [(e["ipv4"], e["linkup"]) for e in i["eth"]])


def cmd_clock(rx, a):
    c = rx.call("get_system_clock_info")
    print("ref:", c["status"]["current-ref"]["string"], "system:", c["status"]["system"]["string"],
          "interface:", c["status"]["interface"]["string"])


def cmd_advertised(rx, a):
    for s in rx.call("get_advertised_session_list")["list"]:
        m = s["session"]["media"][0]
        print("%-3s %-30s %s -> %s:%s  %s  ptime %s  inuse=%s" % (
            s["session-id"], s["session"]["info"], s["host"]["ip"], m["transport-ip"], m["udp"],
            "%s/%s/%s" % (m["rtpmap"][0]["codec"], m["rtpmap"][0]["channel-freq"],
                          m["rtpmap"][0]["number-channels"]), m["ptime"], s["inuse"]))


def cmd_streams(rx, a):
    for s in rx.streams():
        print("%-30s %-8s session %s stream %s" % (s["name"], s["state"], s["session"], s["stream"]))


def cmd_counters(rx, a):
    s = rx.find(a.name)
    c = rx.call("get_session_counters", {"id": s["session"]})["audio-counters"][0]["counters"]
    nd = c["networkdelay"]
    print("networkdelay:", "n/a (not receiving / late)" if nd == 0xFFFFFFFF else "%.3f ms" % (nd / 1e6),
          "pktloss:", c["pktloss"], json.dumps(c["detailed"]))
    for sess in rx.call("get_alarm_status")["sessions"]:
        if sess["id"] == s["session"]:
            for st in sess["streams"]:
                for al in st.get("alarms", []):
                    print("ALARM:", al["severity"], al["description"])


def cmd_subscribe(rx, a):
    if a.advertised:
        adv = [s for s in rx.call("get_advertised_session_list")["list"] if s["session"]["info"] == a.advertised]
        if not adv:
            sys.exit("no advertised session named %r" % a.advertised)
        sid, sdp, name = adv[0]["session-id"], "", a.name or a.advertised
        src_ip = adv[0]["host"]["ip"]
        pt = adv[0]["session"]["media"][0]["payload_types"][0]
    else:
        sdp = open(a.sdp).read().replace("\r\n", "\n")
        if not sdp.endswith("\n"):
            sdp += "\n"
        sid, name = 0, a.name or "sdp stream"
        src_ip = next((l.split()[-1] for l in sdp.splitlines() if l.startswith("a=source-filter:")), "0.0.0.0")
        pt = int(next(l for l in sdp.splitlines() if l.startswith("m=audio")).split()[-1])
    add = {"channel-list": [{"id": a.channel, "type": "tdm"}], "link-offset": a.link_offset,
           "source-ip": src_ip, "name": name,
           "rem-sid": {"id": sid, "media-idx": 0, "payload-type": pt},
           "session-id": 0, "user-sdp": sdp}
    r = rx.call("stream_add_destination", add)
    if not r.get("success"):
        sys.exit("refused: %s %s" % (r.get("error_string"), r.get("auxmsg")))
    print("subscribed:", name, "session", r["session-id"], "stream", r["audio-sources"][0]["id"])


def cmd_unsubscribe(rx, a):
    s = rx.find(a.name)
    print(json.dumps(rx.call("session_remove", {"id": s["session"]})))


def cmd_raw(rx, a):
    print(json.dumps(rx.call(a.command, json.loads(a.args) if a.args else {}), indent=1))


def cmd_pcap(rx, a):
    r = rx.call("request_packet_capture", {"duration": a.seconds, "interface": a.interface})
    print(r.get("auxmsg"))
    print("download after %d s: http://%s/debug/userPktCapture_%s_%dsec.pcap" % (a.seconds, rx.host, a.interface, a.seconds))


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("-s", "--speaker", required=True, help="receiver host or IP")
    sub = p.add_subparsers(dest="cmd", required=True)
    sub.add_parser("info").set_defaults(f=cmd_info)
    sub.add_parser("clock").set_defaults(f=cmd_clock)
    sub.add_parser("advertised").set_defaults(f=cmd_advertised)
    sub.add_parser("streams").set_defaults(f=cmd_streams)
    c = sub.add_parser("counters"); c.add_argument("name"); c.set_defaults(f=cmd_counters)
    s = sub.add_parser("subscribe")
    g = s.add_mutually_exclusive_group(required=True)
    g.add_argument("--advertised", help="name of a SAP-advertised session")
    g.add_argument("--sdp", help="file with the sender's SDP")
    s.add_argument("--name"); s.add_argument("--link-offset", type=int, default=16000)
    s.add_argument("--channel", type=int, default=0, help="receiver output channel id")
    s.set_defaults(f=cmd_subscribe)
    u = sub.add_parser("unsubscribe"); u.add_argument("name"); u.set_defaults(f=cmd_unsubscribe)
    r = sub.add_parser("raw"); r.add_argument("command"); r.add_argument("args", nargs="?"); r.set_defaults(f=cmd_raw)
    pc = sub.add_parser("pcap"); pc.add_argument("seconds", type=int); pc.add_argument("--interface", default="eth0")
    pc.set_defaults(f=cmd_pcap)
    a = p.parse_args()
    a.f(Receiver(a.speaker), a)


if __name__ == "__main__":
    main()
