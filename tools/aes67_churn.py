#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Leon de Rooij (OSHLabs)
# SPDX-License-Identifier: Apache-2.0
"""Stress a node's stream table: add and delete a sink through the REST API
every <interval> seconds for <seconds>, while the node keeps streaming, and
report heap before and after. Pair it with a receiver listening to the node
(tools/aes67_receiver_ctl.py counters) to see whether playback is disturbed.

usage: aes67_churn.py <host> [--seconds 300] [--interval 2.2] [--token T]

The node rate-limits changes (bursts of 5, then one per second). One cycle is
two calls, so a sustained interval under about 2 s gets 429 answers; those are
retried, not counted as failures, but they slow the test down.
"""
import argparse, json, time, urllib.request, urllib.error

SDP = ("v=0\r\no=- 1 1 IN IP4 10.0.0.1\r\ns=churn test\r\nc=IN IP4 239.69.250.1/15\r\nt=0 0\r\n"
       "m=audio 5004 RTP/AVP 98\r\na=rtpmap:98 L24/48000/2\r\na=ptime:1\r\n"
       "a=ts-refclk:ptp=IEEE1588-2008:traceable\r\na=mediaclk:direct=0\r\n")


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("host"); p.add_argument("--seconds", type=float, default=300)
    p.add_argument("--interval", type=float, default=2.2); p.add_argument("--token")
    a = p.parse_args()
    U = "http://%s" % a.host

    def call(method, path, body=None):
        req = urllib.request.Request(U + path, method=method,
                                     data=json.dumps(body).encode() if body is not None else None)
        req.add_header("X-AES67-Request", "1")
        if body is not None:
            req.add_header("Content-Type", "application/json")
        if a.token:
            req.add_header("Authorization", "Bearer " + a.token)
        try:
            with urllib.request.urlopen(req, timeout=8) as r:
                return r.status, r.read().decode()
        except urllib.error.HTTPError as e:
            return e.code, e.read().decode()[:80]
        except Exception as e:
            return 0, str(e)

    def info():
        return json.loads(call("GET", "/api/info")[1])

    i0 = info()
    print("start: uptime %ds heap_free %d heap_min %d" % (i0["uptime"], i0["heap_free"], i0["heap_min"]))
    t0 = time.time(); adds = dels = fails = 0; last = t0
    while time.time() - t0 < a.seconds:
        st, body = call("POST", "/api/sinks", {"name": "churn", "sdp": SDP})
        if st == 200:
            adds += 1
            sid = json.loads(body)["id"]
            time.sleep(a.interval / 2)
            for attempt in range(10):
                st2, body2 = call("DELETE", "/api/sinks/%d" % sid)
                if st2 != 429:
                    break
                time.sleep(1.1)
            if st2 == 200:
                dels += 1
            else:
                fails += 1
                print("delete failed:", st2, body2)
        elif st == 429:
            time.sleep(1.1)
        else:
            fails += 1
            print("add failed:", st, body)
            time.sleep(1.0)
        time.sleep(a.interval / 2)
        if time.time() - last > 30:
            last = time.time(); i = info()
            print("%4ds: adds %d dels %d fails %d heap_free %d" % (time.time() - t0, adds, dels, fails, i["heap_free"]))
    i1 = info()
    print("end: uptime %ds adds %d dels %d fails %d heap_free %d (delta %+d) heap_min %d" % (
        i1["uptime"], adds, dels, fails, i1["heap_free"], i1["heap_free"] - i0["heap_free"], i1["heap_min"]))
    print("rebooted during test" if i1["uptime"] < i0["uptime"] else "no reboot")


if __name__ == "__main__":
    main()
