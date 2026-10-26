#!/usr/bin/env python3
"""Subscribe to touch-to-osc exactly as a receiver should, and print what arrives.

usage: touch-to-osc-watch (or watch.py) [--target HOST:PORT] [--port N] [--lease S] [--prefix /touch] [--device ID] [--no-alive] [--no-moves]

Unsubscribes first (so a restart on the same port gets the full status again) and when
stopped, renews its lease every 3 s, prints the alive, every status change, every event, every
touch with its delay since the screen reported it (needs synced clocks), and says when
the daemon is lost (no alive for 3 s), restarted (a new run) or stopped (bye). --target
defaults to touch-to-osc on this machine, 127.0.0.1:9480.
"""

import argparse
import signal
import socket
import struct
import sys
import threading
import time


def _pad(b):
    return b + b"\0" * (4 - len(b) % 4)


def encode(address, *args):
    tags = ","
    body = b""
    for a in args:
        if isinstance(a, tuple) and a[0] == "h":
            tags += "h"
            body += struct.pack(">q", a[1])
        elif isinstance(a, tuple) and a[0] == "d":
            tags += "d"
            body += struct.pack(">d", a[1])
        elif isinstance(a, tuple) and a[0] == "b":
            tags += "b"
            body += struct.pack(">I", len(a[1])) + a[1] + b"\0" * ((4 - len(a[1]) % 4) % 4)
        elif isinstance(a, bool):
            tags += "i"
            body += struct.pack(">i", int(a))
        elif isinstance(a, int):
            tags += "i"
            body += struct.pack(">i", a)
        elif isinstance(a, float):
            tags += "f"
            body += struct.pack(">f", a)
        else:
            tags += "s"
            body += _pad(str(a).encode())
    return _pad(address.encode()) + _pad(tags.encode()) + body


def encode_bundle(messages):
    out = b"#bundle\0" + struct.pack(">II", 0, 1)
    for m in messages:
        out += struct.pack(">I", len(m)) + m
    return out


def _string(data, pos):
    end = data.index(b"\0", pos)
    s = data[pos:end].decode("utf-8", "replace")
    pos = end + 1
    pos += (4 - pos % 4) % 4
    return s, pos


def decode_message(data):
    address, pos = _string(data, 0)
    tags, pos = _string(data, pos)
    args = []
    for t in tags[1:]:
        if t == "i":
            args.append(struct.unpack_from(">i", data, pos)[0])
            pos += 4
        elif t == "f":
            args.append(struct.unpack_from(">f", data, pos)[0])
            pos += 4
        elif t == "h":
            args.append(struct.unpack_from(">q", data, pos)[0])
            pos += 8
        elif t == "d":
            args.append(struct.unpack_from(">d", data, pos)[0])
            pos += 8
        elif t == "s":
            s, pos = _string(data, pos)
            args.append(s)
        elif t == "T":
            args.append(1)
        elif t == "F":
            args.append(0)
        else:
            raise ValueError("unknown type tag " + t)
    return address, args


def decode_packet(data):
    """Return (is_bundle, [(address, args), ...]) with nested bundles flattened."""
    if data.startswith(b"#bundle\0"):
        out = []
        pos = 16
        while pos < len(data):
            size = struct.unpack_from(">I", data, pos)[0]
            pos += 4
            _, inner = decode_packet(data[pos:pos + size])
            out.extend(inner)
            pos += size
        return True, out
    return False, [decode_message(data)]


def shows_device(rest, device):
    """With --device, keep what is about that device or about no device in particular."""
    parts = rest.split("/")
    if len(parts) >= 3 and parts[1] == "device":
        return parts[2] == device
    if len(parts) == 3 and parts[2] in ("down", "move", "up", "cancel"):
        return parts[1] == device
    return True


def fmt_args(args):
    out = []
    for a in args:
        out.append("%.4f" % a if isinstance(a, float) else repr(a) if isinstance(a, str) else str(a))
    return " ".join(out)


def main():
    ap = argparse.ArgumentParser(description="watch what touch-to-osc sends")
    ap.add_argument("--target", default="127.0.0.1:9480", help="the daemon, HOST:PORT (its osc.listen port; default 127.0.0.1:9480)")
    ap.add_argument("--port", type=int, default=0, help="local UDP port to receive on (default: any free one)")
    ap.add_argument("--lease", type=int, default=10)
    ap.add_argument("--prefix", default="/touch")
    ap.add_argument("--device", help="only show the touches and status of this device id")
    ap.add_argument("--no-alive", action="store_true", help="do not print the 1 s alive and counters")
    ap.add_argument("--no-moves", action="store_true", help="do not print moves")
    a = ap.parse_args()
    host, port = a.target.rsplit(":", 1)
    target = (host, int(port))
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("0.0.0.0", a.port))
    me = sock.getsockname()[1]
    p = a.prefix
    state = {"run": None, "last_alive": None, "lost": False, "status": {}}

    def renew():
        while True:
            sock.sendto(encode(p + "/subscribe", me, a.lease), target)
            time.sleep(3)

    def stop(*_):
        sock.sendto(encode(p + "/unsubscribe", me), target)
        sys.exit(0)

    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)
    sock.sendto(encode(p + "/unsubscribe", me), target)
    threading.Thread(target=renew, daemon=True).start()
    print("receiving on port %d, subscribing to %s:%d" % (me, target[0], target[1]), flush=True)
    sock.settimeout(0.5)
    t0 = time.time()
    heard = False
    hinted = False
    while True:
        now = time.time()
        if state["last_alive"] and now - state["last_alive"] > 3 and not state["lost"]:
            state["lost"] = True
            print("%9.3f LOST: no alive for 3 s" % (now - t0), flush=True)
        if not heard and not hinted and now - t0 > 3:
            hinted = True
            print("%9.3f nothing from %s:%d yet: is touch-to-osc running there, is %d its osc.listen port, and is --prefix right?"
                  % (now - t0, target[0], target[1], target[1]), flush=True)
        try:
            data, _ = sock.recvfrom(65536)
        except socket.timeout:
            continue
        heard = True
        now = time.time()
        try:
            bundle, msgs = decode_packet(data)
        except Exception as e:  # noqa: BLE001
            print("%9.3f undecodable packet: %s" % (now - t0, e), flush=True)
            continue
        stamp = "%9.3f" % (now - t0)
        for address, args in msgs:
            rest = address[len(p):] if address.startswith(p) else address
            if a.device and not shows_device(rest, a.device):
                continue
            if rest == "/alive":
                state["last_alive"] = now
                if state["lost"]:
                    state["lost"] = False
                    print(stamp, "back", flush=True)
                if state["run"] is not None and args[1] != state["run"]:
                    print(stamp, "RESTARTED: run %d -> %d (everything held is over)" % (state["run"], args[1]), flush=True)
                state["run"] = args[1]
                if not a.no_alive:
                    print(stamp, "alive", fmt_args(args), flush=True)
                continue
            if rest == "/bye":
                print(stamp, "BYE run", args[0], flush=True)
                continue
            if rest.endswith(("/down", "/move", "/up", "/cancel")) and not rest.startswith("/device/"):
                if a.no_moves and rest.endswith("/move"):
                    continue
                delay = (now * 1e6 - args[-1]) / 1000.0
                print(stamp, rest, fmt_args(args[:-1]), "(%.1f ms)" % delay, flush=True)
                continue
            counters = ("/active", "/contacts", "/oldest_s", "/last_event_s", "/last_loss_s", "/rate_hz", "/latency_us", "/counts", "/status_gen")
            if rest.endswith(counters) or rest in ("/uptime_s", "/subscribers", "/destinations"):
                if not a.no_alive:
                    print(stamp, "  ", rest, fmt_args(args), flush=True)
                continue
            if rest in ("/event", "/reply", "/error", "/pong", "/subscribed", "/unsubscribed"):
                if rest == "/subscribed" and state["status"]:
                    continue
                print(stamp, rest, fmt_args(args), flush=True)
                continue
            if state["status"].get(rest) != args:
                state["status"][rest] = args
                print(stamp, "status", rest, fmt_args(args), flush=True)


if __name__ == "__main__":
    main()
