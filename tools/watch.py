#!/usr/bin/env python3
"""Subscribe to touch-to-osc as a receiver should, and print what arrives.

usage: touch-to-osc-watch (or watch.py) [--target HOST:PORT] [--port N] [--lease S] [--prefix /touch] [--device ID] [--no-alive] [--no-moves]
                                        [--any-sender]

Unsubscribes first (so a restart on the same port gets the full status again) and when
stopped, renews its lease every 3 s, prints the alive, every status change, every event, every
touch with its delay since the screen reported it (needs synced clocks), and says when
the daemon is lost (no alive for 3 s), restarted (a new run) or stopped (bye). It keeps the
contacts that are down by the receiver rules in OSC.md (the Contacts class below) and says
when one of them ends without its own up or cancel. --target defaults to touch-to-osc on
this machine, 127.0.0.1:9480. Packets from any other host are left out unless --any-sender
is given.
"""

import argparse
import signal
import socket
import struct
import sys
import threading
import time

PHASES = ("down", "move", "up", "cancel")


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
        elif isinstance(a, tuple) and a[0] in ("T", "F"):
            tags += a[0]
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


def decode_message(data, strict=False):
    address, pos = _string(data, 0)
    tags, pos = _string(data, pos)
    if not tags.startswith(","):
        raise ValueError("no type tags")
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
    if strict and (pos != len(data) or len(data) % 4):
        raise ValueError("%d bytes where the message %s needs %d" % (len(data), address, pos))
    return address, args


def decode_packet(data, strict=False, depth=0):
    """Return (is_bundle, [(address, args), ...]) with nested bundles flattened.

    A message this decoder cannot read (a type added later) is left out and the rest of its
    bundle is kept. With strict, anything that is not exactly an OSC packet raises instead.
    """
    if data.startswith(b"#bundle\0"):
        if depth > 4:
            raise ValueError("bundles nested more than 4 deep")
        if len(data) < 16:
            raise ValueError("a bundle of %d bytes" % len(data))
        out = []
        pos = 16
        while pos < len(data):
            size = struct.unpack_from(">I", data, pos)[0]
            pos += 4
            if size == 0 or size % 4 or size > len(data) - pos:
                raise ValueError("a bundle element of %d bytes with %d left" % (size, len(data) - pos))
            try:
                _, inner = decode_packet(data[pos:pos + size], strict, depth + 1)
                out.extend(inner)
            except (ValueError, struct.error):
                if strict or depth > 3:
                    raise
            pos += size
        return True, out
    return False, [decode_message(data, strict)]


class Contacts:
    """The contacts a receiver holds, kept by the receiver rules in OSC.md.

    1. contacts are told apart by run, device and contact id;
    2. a new run, a bye, a lost daemon or a lost lease ends everything held;
    3. an active list ends each held contact of its screen that it leaves out, unless that
       contact's last message has a frame number above the list's;
    4. a move, up or cancel for a contact that is not held is ignored;
    5. only an up is a lift: whatever ends a contact any other way is not a tap.

    Contact ids only go up within a run, so a down that is not above the last one seen on
    its screen is a datagram that came twice or late, and is left out.

    Every method returns the contacts it ended without an up or cancel of their own, as
    (device, contact, why): a receiver treats each like a cancel.
    """

    def __init__(self):
        self.run = None
        self.held = {}
        self.newest = {}

    def _drop_all(self, why):
        ended = [(device, contact, why) for device, contact in sorted(self.held)]
        self.held.clear()
        self.newest.clear()
        return ended

    def alive(self, run):
        ended = self._drop_all("the daemon restarted") if self.run is not None and run != self.run else []
        self.run = run
        return ended

    def bye(self):
        self.run = None
        return self._drop_all("the daemon stopped")

    def lost(self):
        self.run = None
        return self._drop_all("the daemon is not heard")

    def touch(self, device, phase, contact, frame):
        """True when the message counts; False for a contact that is not held (rule 4) or a down seen before."""
        key = (device, contact)
        if phase == "down":
            if contact <= self.newest.get(device, 0):
                return False
            self.newest[device] = contact
            self.held[key] = frame
            return True
        if key not in self.held:
            return False
        if phase == "move":
            self.held[key] = frame
        else:
            del self.held[key]
        return True

    def active(self, device, frame, ids):
        gone = [c for (dev, c), last in sorted(self.held.items()) if dev == device and c not in ids and last <= frame]
        for c in gone:
            del self.held[(device, c)]
        return [(device, c, "it is not in the list of contacts down") for c in gone]


def touch_parts(rest):
    """(device, phase) for a touch address after the prefix, such as /screen/down; None for anything else."""
    parts = rest.split("/")
    if len(parts) == 3 and parts[0] == "" and parts[2] in PHASES and parts[1] != "device":
        return parts[1], parts[2]
    return None


def touch_frame(phase, args):
    """The frame number of a touch message, in its full or its plain form; None when the message is not one."""
    at = 2 if phase == "cancel" else 3
    if len(args) <= at or not isinstance(args[0], int) or not isinstance(args[at], int):
        return None
    return args[at]


def shows_device(rest, device):
    """With --device, keep what is about that device or about no device in particular."""
    parts = rest.split("/")
    if len(parts) >= 3 and parts[1] == "device":
        return parts[2] == device
    if len(parts) == 3 and parts[2] in PHASES:
        return parts[1] == device
    return True


def fmt_args(args):
    out = []
    for a in args:
        out.append("%.4f" % a if isinstance(a, float) else repr(a) if isinstance(a, str) else str(a))
    return " ".join(out)


COUNTERS = ("/active", "/contacts", "/oldest_s", "/last_event_s", "/last_loss_s", "/last_gone_s", "/rate_hz", "/latency_us", "/counts", "/status_gen")
ANSWERS = ("/event", "/reply", "/error", "/pong", "/subscribed", "/unsubscribed", "/here", "/identified", "/describe/item", "/describe/done")


class Watch:
    def __init__(self, prefix, device=None, no_alive=False, no_moves=False, out=print):
        self.prefix = prefix
        self.device = device
        self.no_alive = no_alive
        self.no_moves = no_moves
        self.out = out
        self.contacts = Contacts()
        self.status = {}
        self.last_alive = None
        self.lost = False

    def say(self, stamp, *words):
        self.out(" ".join([stamp] + [str(w) for w in words]), flush=True)

    def ended(self, stamp, ended):
        for device, contact, why in ended:
            self.say(stamp, "ENDED contact %d on %s without an up or cancel: %s (not a tap)" % (contact, device, why))

    def tick(self, stamp, mono):
        if self.last_alive is not None and mono - self.last_alive > 3 and not self.lost:
            self.lost = True
            self.say(stamp, "LOST: no alive for 3 s")
            self.ended(stamp, self.contacts.lost())

    def message(self, stamp, mono, wall, address, args):
        p = self.prefix
        rest = address[len(p):] if address.startswith(p + "/") else address
        if self.device and not shows_device(rest, self.device):
            return
        if rest == "/alive":
            if len(args) < 2 or not isinstance(args[1], int):
                raise ValueError("an alive without a run")
            self.last_alive = mono
            if self.lost:
                self.lost = False
                self.say(stamp, "back")
            if self.contacts.run is not None and args[1] != self.contacts.run:
                self.say(stamp, "RESTARTED: run %d -> %d (everything held is over)" % (self.contacts.run, args[1]))
            self.ended(stamp, self.contacts.alive(args[1]))
            if not self.no_alive:
                self.say(stamp, "alive", fmt_args(args))
            return
        if rest == "/bye":
            self.say(stamp, "BYE run", fmt_args(args))
            self.ended(stamp, self.contacts.bye())
            return
        touch = touch_parts(rest)
        if touch:
            device, phase = touch
            frame = touch_frame(phase, args)
            if frame is None:
                raise ValueError("a %s with arguments %s" % (phase, args))
            known = self.contacts.touch(device, phase, args[0], frame)
            if self.no_moves and phase == "move":
                return
            timed = isinstance(args[-1], int) and len(args) == (4 if phase == "cancel" else 5)
            shown = fmt_args(args[:-1] if timed else args)
            delay = " (%.1f ms)" % ((wall * 1e6 - args[-1]) / 1000.0) if timed else ""
            ignored = "" if known else " (ignored: a down seen before)" if phase == "down" else " (ignored: this contact's down was not seen)"
            self.say(stamp, rest, shown + delay + ignored)
            return
        parts = rest.split("/")
        if len(parts) == 4 and parts[1] == "device" and parts[3] == "active":
            if not args or not all(isinstance(x, int) for x in args):
                raise ValueError("an active list with arguments %s" % args)
            self.ended(stamp, self.contacts.active(parts[2], args[0], set(args[1:])))
        if rest.endswith(COUNTERS) or rest in ("/uptime_s", "/subscribers", "/destinations"):
            if not self.no_alive:
                self.say(stamp, "  ", rest, fmt_args(args))
            return
        if rest in ANSWERS or rest.endswith(("/calibration/touch", "/calibration/fitted")):
            if rest == "/subscribed" and self.status:
                return
            self.say(stamp, rest, fmt_args(args))
            return
        if self.status.get(rest) != args:
            if rest in self.status or len(self.status) < 4096:
                self.status[rest] = args
            self.say(stamp, "status", rest, fmt_args(args))


def main():
    ap = argparse.ArgumentParser(description="watch what touch-to-osc sends")
    ap.add_argument("--target", default="127.0.0.1:9480", help="the daemon, HOST:PORT (its osc.listen port; default 127.0.0.1:9480)")
    ap.add_argument("--port", type=int, default=0, help="local UDP port to receive on (default: any free one)")
    ap.add_argument("--lease", type=int, default=10, help="seconds a subscription lasts without a renewal, 1-60 (default 10)")
    ap.add_argument("--prefix", default="/touch")
    ap.add_argument("--device", help="only show the touches and status of this device id")
    ap.add_argument("--no-alive", action="store_true", help="do not print the 1 s alive and counters")
    ap.add_argument("--no-moves", action="store_true", help="do not print moves")
    ap.add_argument("--any-sender", action="store_true", help="also take packets that do not come from the target's address")
    a = ap.parse_args()
    host, sep, port = a.target.rpartition(":")
    if not sep or not host or not port.isdigit() or not 0 < int(port) < 65536:
        ap.error("--target takes HOST:PORT, such as 192.168.1.50:9480")
    if not 1 <= a.lease <= 60:
        ap.error("--lease takes 1 to 60 seconds")
    try:
        target = (socket.gethostbyname(host), int(port))
    except OSError as e:
        ap.error("cannot find the host %s: %s" % (host, e))
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        sock.bind(("0.0.0.0", a.port))
    except (OSError, OverflowError) as e:
        ap.error("cannot receive on UDP port %d (%s): pick another with --port, or leave it out for any free one" % (a.port, e))
    me = sock.getsockname()[1]
    p = a.prefix
    renew_every = min(3.0, a.lease / 3.0)

    def send(message):
        try:
            sock.sendto(message, target)
            return None
        except OSError as e:
            return e

    def renew():
        failing = False
        while True:
            problem = send(encode(p + "/subscribe", me, a.lease))
            if problem and not failing:
                print("          cannot send to %s:%d (%s); trying again every %.1f s" % (target[0], target[1], problem, renew_every), flush=True)
            failing = bool(problem)
            time.sleep(renew_every)

    def stop(*_):
        send(encode(p + "/unsubscribe", me))
        sys.exit(0)

    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)
    send(encode(p + "/unsubscribe", me))
    threading.Thread(target=renew, daemon=True).start()
    print("receiving on port %d, subscribing to %s:%d" % (me, target[0], target[1]), flush=True)
    sock.settimeout(0.5)
    t0 = time.monotonic()
    heard = False
    hinted = False
    strangers = set()
    watch = Watch(p, a.device, a.no_alive, a.no_moves)
    while True:
        mono = time.monotonic()
        stamp = "%9.3f" % (mono - t0)
        watch.tick(stamp, mono)
        if not heard and not hinted and mono - t0 > 3:
            hinted = True
            if strangers:
                print("%s nothing from %s yet, but packets come from %s: if that is the daemon answering from another of its addresses, use that address as "
                      "--target, or add --any-sender" % (stamp, target[0], ", ".join(sorted(strangers))), flush=True)
            else:
                print("%s nothing from %s:%d yet: is touch-to-osc running there, is %d its osc.listen port, is this machine in its allowed_ips, and is "
                      "--prefix right?" % (stamp, target[0], target[1], target[1]), flush=True)
        try:
            data, sender = sock.recvfrom(65536)
        except socket.timeout:
            continue
        except OSError as e:
            print("%s receiving failed: %s" % (stamp, e), flush=True)
            time.sleep(0.5)
            continue
        mono = time.monotonic()
        stamp = "%9.3f" % (mono - t0)
        if sender[0] != target[0] and not a.any_sender:
            if sender[0] not in strangers and len(strangers) < 16:
                strangers.add(sender[0])
                print("%s leaving out packets from %s: not the daemon at %s (--any-sender takes them too)" % (stamp, sender[0], target[0]), flush=True)
            continue
        heard = True
        try:
            _, msgs = decode_packet(data)
        except (ValueError, struct.error) as e:
            print("%s undecodable packet: %s" % (stamp, e), flush=True)
            continue
        for address, args in msgs:
            try:
                watch.message(stamp, mono, time.time(), address, args)
            except (ValueError, TypeError, IndexError) as e:
                print("%s a message that is not what OSC.md describes: %s %s (%s)" % (stamp, address, args, e), flush=True)


if __name__ == "__main__":
    try:
        main()
    except BrokenPipeError:
        sys.exit(0)
