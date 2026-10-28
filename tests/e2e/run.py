#!/usr/bin/env python3
"""End-to-end tests: the daemon, virtual touchscreens through uinput, a receiver that subscribes like a real one.

usage: tests/e2e/run.py [--quick] [NAME ...]   (as root, on a machine with /dev/uinput)

Each test starts its own daemon on 127.0.0.1:19480 with test devices (USB vendor 7470,
never a real screen's id) and receivers on 19481-19499. --quick skips the tests that wait
more than 10 s. TTO_BUILD picks the build directory.
"""

import os
import shutil
import signal
import subprocess
import sys
import time
import traceback

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import (BUILD, DAEMON_PORT, RUN_DIR, TEST_VENDOR, Daemon, Failure, Receiver, check,  # noqa: E402
                     device, test_product)
from uinput import TouchScreen, try_grab  # noqa: E402

TRACES = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "traces")
TESTS = []
SLOW = set()
MEASURED = []


def test(slow=False):
    def wrap(fn):
        TESTS.append(fn)
        if slow:
            SLOW.add(fn.__name__)
        return fn

    return wrap


class Ctx:
    def __init__(self, name):
        self.name = name
        self.screens = []
        self.daemons = []
        self.receivers = []
        self.next_port = DAEMON_PORT + 1
        self.expect_rc = 0

    def __enter__(self):
        return self

    def screen(self, product, **kw):
        s = TouchScreen("tto-test-%d-%s" % (os.getpid(), self.name), TEST_VENDOR, product, **kw)
        self.screens.append(s)
        return s

    def daemon(self, devices, name=None, **kw):
        d = Daemon(name or "%s-%d" % (self.name, len(self.daemons)), devices, **kw)
        self.daemons.append(d)
        return d

    def receiver(self, subscribe=True, lease=10, host="127.0.0.1"):
        r = Receiver(self.next_port, host=host)
        self.next_port += 1
        self.receivers.append(r)
        if subscribe:
            r.keep_subscribed(lease)
        return r

    def __exit__(self, typ, value, tb):
        problems = []
        for d in self.daemons:
            rc = d.stop()
            if rc != self.expect_rc and typ is None:
                problems.append("the daemon exited with %s, not %d: %s" % (rc, self.expect_rc, d.log_text()[-1500:]))
            reports = d.sanitizer_reports()
            if reports:
                problems.append("sanitizer reports in %s: %s" % (d.log_path, reports))
        for s in self.screens:
            s.destroy()
        for r in self.receivers:
            r.close()
        if problems and typ is None:
            raise Failure("; ".join(problems))
        return False


def connected(r, dev="t", value=1, timeout=5, since=0):
    return r.wait("/touch/device/%s/connected" % dev, lambda m: m.args == [value], timeout=timeout, since=since)


def contact_story(msgs, dev="t"):
    """Every contact: exactly one down first, then moves, then exactly one up or cancel, then nothing."""
    stories = {}
    for m in msgs:
        parts = m.address.split("/")
        if len(parts) != 4 or parts[2] != dev or parts[3] not in ("down", "move", "up", "cancel"):
            continue
        stories.setdefault(m.args[0], []).append(parts[3])
    for c, s in stories.items():
        check(s[0] == "down", "contact %d began with %s" % (c, s[0]))
        check(s.count("down") == 1, "contact %d went down %d times" % (c, s.count("down")))
        ends = [x for x in s if x in ("up", "cancel")]
        check(len(ends) == 1 and s[-1] in ("up", "cancel"), "contact %d ended %s: %s" % (c, ends, s))
    return stories


@test()
def tap_move_lift():
    with Ctx("tap") as c:
        p = test_product()
        s = c.screen(p)
        c.daemon([device(p)])
        r = c.receiver()
        connected(r)
        s.touch(0, 1, 960, 540)
        s.frame()
        down = r.wait("/touch/t/down")
        cid, x, y, frame, t_us = down.args
        check(abs(x - 0.5) < 1e-3 and abs(y - 0.5) < 1e-3, "down at %s,%s, not the centre" % (x, y))
        check(abs(down.at * 1e6 - t_us) < 200000, "t_us is %.1f ms off the arrival" % ((down.at * 1e6 - t_us) / 1000))
        s.touch(0, 1, 1056, 540)
        s.frame()
        move = r.wait("/touch/t/move")
        check(move.args[0] == cid and move.args[3] == frame + 1, "move %s after down %s" % (move.args, down.args))
        check(abs(move.args[1] - 0.55) < 1e-3, "move x %s" % move.args[1])
        s.tick()
        s.lift(0)
        s.frame()
        up = r.wait("/touch/t/up")
        check(up.args[0] == cid and up.args[3] == frame + 2, "up %s: still frames must not use frame numbers" % up.args)
        check(len({down.bundle, move.bundle, up.bundle}) == 3, "each frame is its own bundle")
        r.none("/touch/t/cancel", during=0.2)


@test()
def ten_fingers_one_bundle():
    with Ctx("ten") as c:
        p = test_product()
        s = c.screen(p)
        c.daemon([device(p)])
        r = c.receiver()
        connected(r)
        for i in range(10):
            s.touch(i, 100 + i, 100 + 150 * i, 100 + 80 * i)
        s.frame()
        downs = r.wait("/touch/t/down", count=10)
        check(len({m.bundle for m in downs}) == 1, "ten downs in %d bundles" % len({m.bundle for m in downs}))
        check(len({m.args[3] for m in downs}) == 1, "ten downs with different frame numbers")
        check(len({m.args[0] for m in downs}) == 10, "contact ids repeat")
        alive = r.wait("/touch/device/t/active", lambda m: len(m.args) == 11, timeout=2.5)
        check(sorted(alive.args[1:]) == sorted(m.args[0] for m in downs), "active list %s" % alive.args)
        for i in range(10):
            s.lift(i)
        s.frame()
        ups = r.wait("/touch/t/up", count=10)
        check(len({m.bundle for m in ups}) == 1, "ten ups in more than one bundle")


@test()
def unplug_mid_touch_cancels_at_once():
    with Ctx("unplug") as c:
        p = test_product()
        s = c.screen(p)
        c.daemon([device(p)])
        r = c.receiver()
        connected(r)
        s.touch(0, 1, 100, 100)
        s.touch(1, 2, 300, 300)
        s.frame()
        r.wait("/touch/t/down", count=2)
        t0 = time.time()
        s.destroy()
        cancels = r.wait("/touch/t/cancel", count=2, timeout=1)
        delay = max(m.at for m in cancels) - t0
        MEASURED.append("unplug to cancel: %.1f ms" % (delay * 1000))
        check(all(m.args[1] == "device lost" for m in cancels), "cancel reasons %s" % [m.args for m in cancels])
        check(delay < 0.05, "the cancels took %.1f ms" % (delay * 1000))
        connected(r, value=0)
        r.wait("/touch/device/t/state", lambda m: m.args == ["not connected"])
        r.wait("/touch/event", lambda m: m.args[0] == "device" and "lost" in m.args[1])
        r.none("/touch/t/up", during=0.2)


@test()
def unplug_without_a_kernel_release_still_cancels():
    with Ctx("unplugread") as c:
        p = test_product()
        s = c.screen(p, btn_touch=False)
        c.daemon([device(p)])
        r = c.receiver()
        connected(r)
        s.touch(0, 1, 100, 100)
        s.frame()
        r.wait("/touch/t/down")
        t0 = time.time()
        s.destroy()
        cancel = r.wait("/touch/t/cancel", timeout=1)
        check(cancel.args[1] == "device lost", "cancel %s" % cancel.args)
        check(cancel.at - t0 < 0.05, "the cancel took %.1f ms" % ((cancel.at - t0) * 1000))
        ev = r.wait("/touch/event", lambda m: m.args[0] == "device" and "lost" in m.args[1])
        check("unplugged" in ev.args[1], "the read error path did not cancel: %s" % ev.args)


@test()
def sigterm_cancels_then_bye():
    with Ctx("sigterm") as c:
        p = test_product()
        s = c.screen(p)
        d = c.daemon([device(p)])
        r = c.receiver()
        connected(r)
        run = r.wait("/touch/alive", timeout=2.5).args[1]
        s.touch(0, 1, 100, 100)
        s.touch(1, 2, 300, 300)
        s.frame()
        r.wait("/touch/t/down", count=2)
        m = r.mark()
        rc = d.stop(signal.SIGTERM)
        check(rc == 0, "exit code %s" % rc)
        cancels = r.wait("/touch/t/cancel", count=2, since=m)
        bye = r.wait("/touch/bye", since=m)
        check(all(x.args[1] == "stopping" for x in cancels), "cancel reasons %s" % [x.args for x in cancels])
        check(bye.args == [run], "bye %s for run %s" % (bye.args, run))
        check(max(x.at for x in cancels) <= bye.at, "bye before the cancels")


@test()
def sigstop_is_lost_and_sigcont_resumes():
    with Ctx("sigstop") as c:
        p = test_product()
        s = c.screen(p)
        d = c.daemon([device(p)])
        r = c.receiver()
        connected(r)
        s.touch(0, 1, 100, 100)
        s.frame()
        cid = r.wait("/touch/t/down").args[0]
        run = r.wait("/touch/alive", timeout=2.5).args[1]
        d.pause()
        t0 = time.time()
        time.sleep(3.5)
        late = r.all(0, "/touch/alive", lambda m: m.at > t0 + 0.05)
        check(not late, "alive while stopped: %s" % late)
        d.resume()
        alive = r.wait("/touch/alive", lambda m: m.at > t0 + 3.5, timeout=2.5)
        check(alive.args[1] == run, "run changed over SIGSTOP")
        active = r.wait("/touch/device/t/active", lambda m: m.at > t0 + 3.5, timeout=2)
        check(cid in active.args[1:], "the held contact is missing from %s" % active.args)


@test()
def kernel_drop_resyncs_and_cancels():
    with Ctx("resync") as c:
        p = test_product()
        s = c.screen(p)
        d = c.daemon([device(p)])
        r = c.receiver()
        connected(r)
        s.touch(0, 1, 100, 200)
        s.frame()
        c1 = r.wait("/touch/t/down").args[0]
        d.pause()
        for i in range(3000):
            s.touch(0, 1, 100 + i % 700, 200)
            s.frame()
        d.resume()
        cancel = r.wait("/touch/t/cancel", timeout=3)
        check(cancel.args[0] == c1 and cancel.args[1] == "resync", "cancel %s" % cancel.args)
        r.wait("/touch/event", lambda m: m.args[0] == "resync", timeout=2)
        m = r.mark()
        s.touch(0, 1, 800, 800)
        s.frame()
        r.none("/touch/t/move", during=0.3, since=m)
        r.none("/touch/t/down", during=0.01, since=m)
        s.lift(0)
        s.frame()
        s.touch(0, 2, 300, 300)
        s.frame()
        c2 = r.wait("/touch/t/down", since=m).args[0]
        check(c2 != c1, "contact id reused")
        r.none("/touch/t/up", lambda x: x.args[0] == c1, during=0.2, since=0)


@test()
def lost_up_is_ended_by_the_active_list():
    with Ctx("lostup") as c:
        p = test_product()
        s = c.screen(p)
        c.daemon([device(p)])
        r = c.receiver()
        connected(r)
        r.discard = lambda address, args: address == "/touch/t/up"
        s.touch(0, 1, 100, 100)
        s.frame()
        down = r.wait("/touch/t/down")
        cid, frame = down.args[0], down.args[3]
        s.lift(0)
        s.frame()
        t0 = time.time()
        active = r.wait("/touch/device/t/active", lambda m: m.at > t0 and m.args[0] > frame, timeout=1.5)
        check(cid not in active.args[1:], "the lifted contact is still listed: %s" % active.args)
        check(active.at - t0 <= 1.1, "the list took %.2f s" % (active.at - t0))


@test()
def active_list_frame_guards_a_new_touch():
    with Ctx("activeframe") as c:
        p = test_product()
        s = c.screen(p)
        c.daemon([device(p)])
        r = c.receiver()
        connected(r)
        for k in range(3):
            s.touch(0, 100 + k, 300 + k, 300)
            s.frame()
            time.sleep(1.2)
            s.lift(0)
            s.frame()
        for k in range(30):
            s.touch(0, k + 1, 100 + k, 100)
            s.frame()
            time.sleep(0.05)
            s.lift(0)
            s.frame()
            time.sleep(0.02)
        downs = {m.args[0]: m.args[3] for m in r.all(0, "/touch/t/down")}
        listed = 0
        for a in r.all(0, "/touch/device/t/active"):
            for cid in a.args[1:]:
                listed += 1
                check(cid in downs and downs[cid] <= a.args[0], "active list %s names contact %d whose down came in a later bundle" % (a.args, cid))
        check(listed >= 3, "only %d listings: the check proved nothing" % listed)


@test()
def rotation_and_mirror_map_the_corners():
    with Ctx("rotation") as c:
        p = test_product()
        s = c.screen(p)
        c.daemon([device(p, rotation=90, mirror="x")])
        r = c.receiver()
        connected(r)
        expect = {(0, 0): (0, 0), (1920, 0): (0, 1), (1920, 1080): (1, 1), (0, 1080): (1, 0)}
        for k, ((px, py), (ex, ey)) in enumerate(expect.items()):
            m = r.mark()
            s.touch(0, k + 1, px, py)
            s.frame()
            down = r.wait("/touch/t/down", since=m)
            check(abs(down.args[1] - ex) < 1e-3 and abs(down.args[2] - ey) < 1e-3, "panel %s,%s -> %s,%s, expected %s,%s" % (px, py, down.args[1], down.args[2], ex, ey))
            s.lift(0)
            s.frame()
            r.wait("/touch/t/up", since=m)
        r.wait("/touch/device/t/aspect", lambda m: abs(m.args[0] - 9 / 16) < 1e-3)


@test()
def plain_rotation_90_turns_clockwise():
    with Ctx("rot90") as c:
        p = test_product()
        s = c.screen(p)
        c.daemon([device(p, rotation=90)])
        r = c.receiver()
        connected(r)
        s.touch(0, 1, 0, 0)
        s.frame()
        down = r.wait("/touch/t/down")
        check(abs(down.args[1] - 1) < 1e-3 and abs(down.args[2]) < 1e-3, "the panel's top left landed at %s,%s" % (down.args[1], down.args[2]))


@test()
def requests_answer_once():
    with Ctx("requests") as c:
        p = test_product()
        c.screen(p)
        c.daemon([device(p)])
        r = c.receiver()
        connected(r)
        run = r.wait("/touch/alive", timeout=2.5).args[1]
        r.send("/touch/request", 1, "/touch/ping")
        check(r.wait("/touch/reply", lambda m: m.args[0] == 1).args == [1, "ok", ""], "ping reply")
        check(r.wait("/touch/pong").args == [run], "pong")
        m = r.mark()
        r.send("/touch/request", 1, "/touch/ping")
        check(r.wait("/touch/reply", since=m).args == [1, "ok", ""], "repeat reply")
        r.none("/touch/pong", during=0.4, since=m)
        r.send("/touch/request", 1, "/touch/status/send")
        check(r.wait("/touch/reply", since=m, count=2)[1].args == [1, "refused", "seq 1 was used for another command in the last 30 s: use a new seq for each command"], "reused seq")
        r.send("/touch/request", 2, "/touch/nope")
        check(r.wait("/touch/reply", lambda x: x.args[0] == 2).args == [2, "refused", "unknown address"], "unknown address")
        r.send("/touch/request", 3, "/touch/request", 4, "/touch/ping")
        check(r.wait("/touch/reply", lambda x: x.args[0] == 3).args[1:] == ["refused", "a request cannot carry another request"], "nested")
        r.send("/touch/nope", 1)
        check(r.wait("/touch/error").args == ["/touch/nope", "unknown address"], "bare refusal")
        r.send("/touch/request", 5, "/touch/device/zz/enabled", 0)
        check(r.wait("/touch/reply", lambda x: x.args[0] == 5).args == [5, "refused", "no device zz (devices: t)"], "unknown device")
        r.send("/touch/request", 6, "/touch/device/t/enabled", 2)
        check(r.wait("/touch/reply", lambda x: x.args[0] == 6).args == [6, "refused", "enabled needs 0 or 1"], "bad value")


@test(slow=True)
def kept_reply_expires_after_30_s():
    with Ctx("kept30") as c:
        p = test_product()
        c.screen(p)
        c.daemon([device(p)])
        r = c.receiver()
        connected(r)
        r.send("/touch/request", 9, "/touch/ping")
        r.wait("/touch/pong")
        time.sleep(31)
        m = r.mark()
        r.send("/touch/request", 9, "/touch/ping")
        r.wait("/touch/pong", since=m)


@test()
def outsiders_are_refused_with_the_reason():
    import socket
    import watch
    with Ctx("outsider") as c:
        p = test_product()
        c.screen(p)
        c.daemon([device(p)])
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.bind(("127.0.0.2", c.next_port))
        sock.settimeout(2)
        try:
            sock.sendto(watch.encode("/touch/subscribe", c.next_port, 10), ("127.0.0.1", DAEMON_PORT))
            data, _ = sock.recvfrom(65536)
        finally:
            sock.close()
        _, msgs = watch.decode_packet(data)
        address, args = msgs[0]
        check(address == "/touch/error" and "127.0.0.2 may not send commands here: add it to osc.allowed_ips" in args[1], "answer %s %s" % (address, args))


@test()
def leases_deliver_renew_refuse_and_expire():
    with Ctx("leases") as c:
        p = test_product()
        s = c.screen(p)
        c.daemon([device(p)])
        r = c.receiver()
        connected(r)
        for attempt in range(3):
            m = r.mark()
            r.subscribe()
            sub = r.wait("/touch/subscribed", since=m)
            near = r.all(m, "/touch/osc_api", lambda x: abs(x.at - sub.at) < 0.1)
            if not near:
                break
        check(not near, "a renewal sent the full status")
        short = c.receiver(subscribe=False)
        short.subscribe(1)
        short.wait("/touch/osc_api", timeout=2)
        others = [c.receiver(subscribe=False) for _ in range(6)]
        for o in others:
            o.subscribe(30)
            o.wait("/touch/subscribed")
        ninth = c.receiver(subscribe=False)
        ninth.subscribe(30)
        err = ninth.wait("/touch/error")
        check("at most 8 subscribers" in err.args[1], "ninth: %s" % err.args)
        time.sleep(2.5)
        m = r.mark()
        ms = short.mark()
        s.touch(0, 1, 100, 100)
        s.frame()
        r.wait("/touch/t/down", since=m)
        short.none("/touch/t/down", during=0.3, since=ms)
        ev = r.wait("/touch/event", lambda x: x.args[0] == "subscriber" and "ended" in x.args[1], timeout=1)
        check(str(short.port) in ev.args[1], "expiry event %s" % ev.args)


@test()
def saved_destination_gets_touches_without_subscribing():
    with Ctx("saved") as c:
        p = test_product()
        s = c.screen(p)
        port = c.next_port
        c.daemon([device(p)], destinations=["127.0.0.1:%d" % port])
        r = c.receiver(subscribe=False)
        r.wait("/touch/alive", timeout=2.5)
        s.touch(0, 1, 100, 100)
        s.frame()
        r.wait("/touch/t/down")


@test()
def silence_cancels_a_held_contact():
    with Ctx("silence") as c:
        p = test_product()
        s = c.screen(p)
        c.daemon([device(p, silence_ms=200)])
        r = c.receiver()
        connected(r)
        s.touch(0, 1, 100, 100)
        s.frame()
        c1 = r.wait("/touch/t/down").args[0]
        t0 = time.time()
        cancel = r.wait("/touch/t/cancel", timeout=2)
        delay = cancel.at - t0
        MEASURED.append("silence 200 ms -> cancel after %.0f ms" % (delay * 1000))
        check(cancel.args[:2] == [c1, "device silent"], "cancel %s" % cancel.args)
        check(0.18 < delay < 0.5, "cancelled after %.0f ms" % (delay * 1000))
        r.wait("/touch/event", lambda m: m.args[0] == "silence")
        m = r.mark()
        s.touch(0, 1, 500, 500)
        s.frame()
        r.none("/touch/t/move", during=0.3, since=m)
        s.lift(0)
        s.frame()
        s.touch(0, 2, 600, 600)
        s.frame()
        c2 = r.wait("/touch/t/down", since=m).args[0]
        for _ in range(60):
            s.tick()
            time.sleep(0.016)
        r.none("/touch/t/cancel", lambda x: x.args[0] == c2, during=0.05, since=m)


@test()
def hotplug_connects_and_reconnects():
    with Ctx("hotplug") as c:
        p = test_product()
        c.daemon([device(p)])
        r = c.receiver()
        r.wait("/touch/device/t/state", lambda m: m.args == ["not connected"], timeout=3)
        r.wait("/touch/health", lambda m: m.args == ["fault"], timeout=2)
        s = c.screen(p)
        connected(r, timeout=4)
        r.wait("/touch/health", lambda m: m.args == ["ok"], timeout=2)
        s.touch(0, 1, 100, 100)
        s.frame()
        c1 = r.wait("/touch/t/down").args[0]
        s.lift(0)
        s.frame()
        s.destroy()
        connected(r, value=0)
        m = r.mark()
        s2 = c.screen(p)
        connected(r, timeout=4, since=m)
        s2.touch(0, 1, 100, 100)
        s2.frame()
        c2 = r.wait("/touch/t/down", since=m).args[0]
        check(c2 > c1, "contact ids went backwards after a reconnect")


@test()
def held_finger_at_connect_is_not_announced():
    with Ctx("heldatconnect") as c:
        p = test_product()
        s = c.screen(p)
        s.touch(0, 1, 100, 100)
        s.frame()
        c.daemon([device(p)])
        r = c.receiver()
        connected(r)
        s.touch(0, 1, 200, 200)
        s.frame()
        r.none("/touch/t/down", during=0.3, since=0)
        s.lift(0)
        s.frame()
        r.none("/touch/t/up", during=0.2, since=0)
        s.touch(0, 2, 300, 300)
        s.frame()
        r.wait("/touch/t/down")


@test()
def disable_cancels_and_enable_waits_for_a_lift():
    with Ctx("disable") as c:
        p = test_product()
        s = c.screen(p)
        c.daemon([device(p)])
        r = c.receiver()
        connected(r)
        s.touch(0, 1, 100, 100)
        s.frame()
        c1 = r.wait("/touch/t/down").args[0]
        r.send("/touch/request", 1, "/touch/device/t/enabled", 0)
        r.wait("/touch/reply", lambda m: m.args == [1, "ok", ""])
        cancel = r.wait("/touch/t/cancel")
        check(cancel.args[:2] == [c1, "device disabled"], "cancel %s" % cancel.args)
        r.wait("/touch/device/t/enabled", lambda m: m.args == [0])
        r.wait("/touch/device/t/state", lambda m: m.args == ["disabled"])
        r.wait("/touch/health", lambda m: m.args == ["warning"])
        m = r.mark()
        s.touch(0, 1, 200, 200)
        s.frame()
        s.lift(0)
        s.frame()
        s.touch(0, 3, 250, 250)
        s.frame()
        r.none("/touch/t/move", during=0.3, since=m)
        r.none("/touch/t/down", during=0.01, since=m)
        r.send("/touch/device/t/enabled", 1)
        r.wait("/touch/device/t/enabled", lambda x: x.args == [1], since=m)
        s.touch(0, 3, 300, 300)
        s.frame()
        r.none("/touch/t/move", during=0.3, since=m)
        r.none("/touch/t/down", during=0.01, since=m)
        s.lift(0)
        s.frame()
        s.touch(0, 4, 400, 400)
        s.frame()
        r.wait("/touch/t/down", since=m)


@test()
def grab_holds_the_device():
    with Ctx("grab") as c:
        p = test_product()
        s = c.screen(p)
        d = c.daemon([device(p)])
        r = c.receiver()
        connected(r)
        r.wait("/touch/device/t/grabbed", lambda m: m.args == [1])
        check(not try_grab(s.node), "another program could grab the device while the daemon holds it")
        d.stop()
        c.daemons.remove(d)
        check(try_grab(s.node), "the grab outlived the daemon")
        c.daemon([device(p, grab=False)])
        r.wait("/touch/device/t/grabbed", lambda m: m.args == [0], since=r.mark(), timeout=4)
        check(try_grab(s.node), "grab = false still holds the device")


@test()
def foreign_grab_is_a_fault_with_the_reason():
    import fcntl
    from uinput import EVIOCGRAB
    with Ctx("foreigngrab") as c:
        p = test_product()
        s = c.screen(p)
        fd = os.open(s.node, os.O_RDONLY | os.O_NONBLOCK)
        fcntl.ioctl(fd, EVIOCGRAB, 1)
        try:
            c.daemon([device(p)])
            r = c.receiver()
            r.wait("/touch/device/t/state", lambda m: m.args == ["grab refused"], timeout=4)
            reason = r.wait("/touch/device/t/reason", lambda m: m.args[0] != "").args[0]
            check("another program holds" in reason and "stop that program" in reason, "reason %r" % reason)
            r.wait("/touch/health", lambda m: m.args == ["fault"])
        finally:
            os.close(fd)
        r.wait("/touch/device/t/state", lambda m: m.args == ["ok"], timeout=4)
        s.touch(0, 1, 10, 10)
        s.frame()
        r.wait("/touch/t/down")


@test()
def a_screen_let_go_by_another_program_is_read_afresh():
    import fcntl
    from uinput import EVIOCGRAB
    with Ctx("letgo") as c:
        p = test_product()
        s = c.screen(p)
        fd = os.open(s.node, os.O_RDONLY | os.O_NONBLOCK)
        fcntl.ioctl(fd, EVIOCGRAB, 1)
        try:
            c.daemon([device(p)])
            r = c.receiver()
            r.wait("/touch/device/t/state", lambda m: m.args == ["grab refused"], timeout=4)
        finally:
            os.close(fd)
        m = r.mark()
        s.touch(0, 1, 100, 100)
        s.frame()
        time.sleep(3)
        s.lift(0)
        s.frame()
        r.wait("/touch/device/t/state", lambda x: x.args == ["ok"], timeout=4, since=m)
        time.sleep(0.5)
        contact_story(r.all(m))
        downs = r.all(m, "/touch/t/down")
        check(not downs, "a finger put down as the other program let go was announced from the state read before: %s" % downs)
        s.touch(0, 2, 200, 200)
        s.frame()
        s.lift(0)
        s.frame()
        r.wait("/touch/t/up", since=m)
        contact_story(r.all(m))


@test()
def a_second_copy_on_the_same_port_is_refused():
    with Ctx("twice") as c:
        p = test_product()
        c.screen(p)
        d = c.daemon([device(p)])
        r = c.receiver()
        connected(r)
        log = os.path.join(RUN_DIR, "twice-second.log")
        with open(log, "w") as out:
            proc = subprocess.Popen([os.path.join(BUILD, "touch-to-osc"), "--config=" + d.config], stdout=out, stderr=subprocess.STDOUT)
            try:
                rc = proc.wait(5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
                raise Failure("a second copy started on the port the first one listens on")
        with open(log) as f:
            text = f.read()
        check(rc == 3, "the second copy exited with %s, not 3: %s" % (rc, text[-600:]))
        check("cannot listen" in text and "systemctl stop touch-to-osc" in text, "message: %s" % text[-600:])
        m = r.mark()
        check(req(r, "/touch/ping") == ["ok", ""], "the first copy still answers")
        r.wait("/touch/pong", since=m)


@test()
def two_identical_screens_told_apart_by_port():
    with Ctx("two") as c:
        p = test_product()
        a = c.screen(p, phys="tto-a-%d" % os.getpid())
        b = c.screen(p, phys="tto-b-%d" % os.getpid())
        c.daemon([device(p, id="a", port="tto-a-%d" % os.getpid()), device(p, id="b", port="tto-b-%d" % os.getpid())])
        r = c.receiver()
        connected(r, "a")
        connected(r, "b")
        a.touch(0, 1, 10, 10)
        a.frame()
        b.touch(0, 1, 20, 20)
        b.frame()
        r.wait("/touch/a/down")
        r.wait("/touch/b/down")
        check(r.wait("/touch/alive", lambda m: m.args[4] == 2 and m.args[6] == 2, timeout=2.5).args[5] == 2, "alive counts")


@test()
def identical_screens_are_shared_out_in_port_order_at_every_start():
    pid = os.getpid()
    for first_made in ("b", "a"):
        with Ctx("portorder-" + first_made) as c:
            p = test_product()
            for k in (first_made, "a" if first_made == "b" else "b"):
                c.screen(p, phys="tto-order-%s-%d" % (k, pid))
            c.daemon([device(p, id="one"), device(p, id="two")])
            r = c.receiver()
            connected(r, "one")
            connected(r, "two")
            r.wait("/touch/device/one/port", lambda x: x.args == ["tto-order-a-%d" % pid])
            r.wait("/touch/device/two/port", lambda x: x.args == ["tto-order-b-%d" % pid])
            reasons = r.wait("/touch/reasons", lambda x: any("matches 2 touchscreens" in a for a in x.args)).args
            check(any("which can change at a restart: set match_port" in a for a in reasons), "reasons %s" % reasons)


@test()
def a_device_naming_a_port_gets_its_screen_before_one_matching_by_usb_id():
    pid = os.getpid()
    with Ctx("portfirst") as c:
        p = test_product()
        c.screen(p, phys="tto-first-a-%d" % pid)
        c.screen(p, phys="tto-first-b-%d" % pid)
        c.daemon([device(p, id="any"), device(p, id="front", port="tto-first-a-%d" % pid)])
        r = c.receiver()
        connected(r, "any")
        connected(r, "front")
        r.wait("/touch/device/front/port", lambda x: x.args == ["tto-first-a-%d" % pid])
        r.wait("/touch/device/any/port", lambda x: x.args == ["tto-first-b-%d" % pid])
    with Ctx("moved") as c:
        p = test_product()
        c.screen(p, phys="tto-moved-%d" % pid)
        c.daemon([device(p, port="tto-nowhere")])
        r = c.receiver()
        reason = r.wait("/touch/device/t/reason", lambda x: "plugged into port" in x.args[0], timeout=3).args[0]
        check(reason == "a 7470:%04x touchscreen is plugged into port tto-moved-%d, not tto-nowhere: plug it into tto-nowhere, or set match_port to tto-moved-%d"
              % (p, pid, pid), "reason %r" % reason)


@test()
def not_a_touchscreen_is_reported():
    with Ctx("nottouch") as c:
        p = test_product()
        c.screen(p, direct=False)
        c.daemon([device(p)])
        r = c.receiver()
        r.wait("/touch/device/t/state", lambda m: m.args == ["not a touchscreen"], timeout=3)
        reason = r.wait("/touch/device/t/reason", lambda m: "is \"" in m.args[0]).args[0]
        check("not a direct-touch device" in reason and "touch-to-osc --list" in reason, "reason %r" % reason)


@test()
def status_describes_the_device():
    with Ctx("status") as c:
        p = test_product()
        c.screen(p)
        c.daemon([device(p, label="test screen")])
        r = c.receiver()
        connected(r)
        want = {
            "/touch/osc_api": [6],
            "/touch/device/t/usb_recovery": [1],
            "/touch/device/t/usb_watched_port": [""],
            "/touch/device/t/usb_port_state": [""],
            "/touch/device/t/usb_resets": [0],
            "/touch/config": [c.daemons[0].config],
            "/touch/settings_file": [""],
            "/touch/log_level": ["info"],
            "/touch/allowed_ips": ["127.0.0.1"],
            "/touch/saved_destinations": [],
            "/touch/overrides": [],
            "/touch/device/t/match_usb": ["%04x:%04x" % (TEST_VENDOR, p)],
            "/touch/device/t/match_port": [""],
            "/touch/device/t/calibration": [],
            "/touch/device/t/grab": [1],
            "/touch/devices": ["t"],
            "/touch/device/t/label": ["test screen"],
            "/touch/device/t/state": ["ok"],
            "/touch/device/t/slots": [10],
            "/touch/device/t/usb": ["%04x:%04x" % (TEST_VENDOR, p)],
            "/touch/device/t/rotation": [0],
            "/touch/device/t/mirror": [""],
            "/touch/device/t/silence_ms": [0],
            "/touch/device/t/grabbed": [1],
            "/touch/health": ["ok"],
        }
        for address, args in want.items():
            r.wait(address, lambda m, a=args: m.args == a)
        size = r.wait("/touch/device/t/size_mm", lambda m: m.args[0] > 0).args
        check(abs(size[0] - 480) < 0.5 and abs(size[1] - 270) < 0.5, "size %s" % size)
        r.wait("/touch/device/t/aspect", lambda m: abs(m.args[0] - 16 / 9) < 1e-3)
        r.wait("/touch/device/t/counts", timeout=2.5)
        r.wait("/touch/device/t/latency_us", timeout=2.5)
        check(r.wait("/touch/host").args[0] != "", "no host name")
        u1 = r.wait("/touch/uptime_s", timeout=2.5)
        u2 = r.wait("/touch/uptime_s", lambda m: m.at > u1.at + 1.5, timeout=3)
        check(u2.args[0] > u1.args[0], "uptime %s then %s" % (u1.args, u2.args))
        m = r.mark()
        r.send("/touch/status/send")
        r.wait("/touch/run", since=m)
        log = c.daemons[0].log_text()
        with open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "CMakeLists.txt")) as f:
            version = f.read().split("project(touch-to-osc VERSION ", 1)[1].split()[0]
        check("touch-to-osc %s (osc_api 6)" % version in log, "banner for %s" % version)


@test()
def no_permission_says_what_to_do():
    with Ctx("noperm") as c:
        p = test_product()
        c.screen(p)
        os.makedirs("/tmp/tto-e2e", exist_ok=True)
        os.chmod("/tmp/tto-e2e", 0o755)
        d = Daemon.__new__(Daemon)
        import subprocess
        d.name = "noperm"
        d.config = "/tmp/tto-e2e/noperm.toml"
        d.log_path = "/tmp/tto-e2e/noperm.log"
        with open(d.config, "w") as f:
            f.write('[osc]\nlisten = "127.0.0.1:%d"\nallowed_ips = ["127.0.0.1"]\n[paths]\nsettings = ""\nstate = ""\n[[device]]\nid = "t"\nusb = "%04x:%04x"\n' % (DAEMON_PORT, TEST_VENDOR, p))
        os.chmod(d.config, 0o644)
        d.log = open(d.log_path, "w")
        binary = os.path.join(os.environ.get("TTO_BUILD", os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "build")), "touch-to-osc")
        d.proc = subprocess.Popen(["setpriv", "--reuid=65534", "--regid=65534", "--clear-groups", binary, "--config=" + d.config], stdout=d.log, stderr=subprocess.STDOUT)
        c.daemons.append(d)
        r = c.receiver()
        r.wait("/touch/device/t/state", lambda m: m.args == ["no permission"], timeout=5)
        reason = r.wait("/touch/device/t/reason", lambda m: m.args[0] != "").args[0]
        check("permission denied" in reason and "input group" in reason, "reason %r" % reason)


_seq = [1000]


def req(r, address, *args):
    _seq[0] += 1
    s = _seq[0]
    m = r.mark()
    r.send("/touch/request", s, address, *args)
    return r.wait("/touch/reply", lambda x: x.args[0] == s, since=m).args[1:]


@test()
def rotation_setting_restarts_reading_and_is_saved():
    with Ctx("setrot") as c:
        p = test_product()
        s = c.screen(p)
        d = c.daemon([device(p)], name="setrot")
        r = c.receiver()
        connected(r)
        s.touch(0, 1, 100, 100)
        s.frame()
        c1 = r.wait("/touch/t/down").args[0]
        m = r.mark()
        check(req(r, "/touch/device/t/rotation", 90) == ["ok", ""], "rotation refused")
        cancel = r.wait("/touch/t/cancel", since=m)
        check(cancel.args[:2] == [c1, "reconfigured"], "cancel %s" % cancel.args)
        r.wait("/touch/event", lambda x: x.args[0] == "device" and "map differently" in x.args[1], since=m)
        check(not try_grab(s.node), "the device was let go: a rotation must not reopen it")
        ev = r.wait("/touch/event", lambda x: x.args[0] == "settings", since=m)
        check(ev.args[1] == "t rotation 0 -> 90, from 127.0.0.1", "event %r" % ev.args[1])
        r.wait("/touch/device/t/rotation", lambda x: x.args == [90], since=m)
        r.wait("/touch/overrides", lambda x: x.args == ["device/t/rotation"], since=m)
        r.wait("/touch/settings_file", lambda x: x.args == [d.settings], since=m)
        check(not r.all(m, "/touch/device/t/state", lambda x: x.args == ["restarting"]), "a rotation restarted the device")
        m2 = r.mark()
        s.touch(0, 1, 150, 150)
        s.frame()
        r.none("/touch/t/down", during=0.3, since=m2)
        s.lift(0)
        s.frame()
        s.touch(0, 2, 0, 0)
        s.frame()
        down = r.wait("/touch/t/down", since=m2)
        check(abs(down.args[1] - 1) < 1e-3 and abs(down.args[2]) < 1e-3, "rotated down at %s" % down.args)
        with open(d.settings) as f:
            text = f.read()
        check("rotation = 90" in text, "settings file: %s" % text)
        s.lift(0)
        s.frame()
        check(d.stop() == 0, "exit code")
        c.daemons.remove(d)
        d2 = c.daemon([device(p)], name="setrot", keep_settings=True)
        m3 = r.mark()
        r.wait("/touch/device/t/rotation", lambda x: x.args == [90], since=m3, timeout=5)
        r.wait("/touch/overrides", lambda x: x.args == ["device/t/rotation"], since=m3)
        check("saved settings from" in d2.log_text(), "the banner does not mention the saved settings")
        m4 = r.mark()
        check(req(r, "/touch/settings/forget", "device/t/rotation") == ["ok", ""], "forget")
        r.wait("/touch/device/t/rotation", lambda x: x.args == [0], since=m4)
        r.wait("/touch/overrides", lambda x: x.args == [], since=m4)
        r.wait("/touch/settings_file", lambda x: x.args == [""], since=m4)
        check(not os.path.exists(d.settings), "the settings file is still there")


@test()
def bad_settings_are_refused_in_the_configs_words():
    with Ctx("setbad") as c:
        p = test_product()
        c.screen(p)
        d = c.daemon([device(p)])
        r = c.receiver()
        connected(r)
        cases = [
            (("/touch/device/t/rotation", 45), "rotation must be 0, 90, 180 or 270"),
            (("/touch/device/t/mirror", "z"), 'mirror must be "", "x", "y" or "xy"'),
            (("/touch/device/t/silence_ms", 10), "silence_ms must be 0 (off) or between 50 and 10000"),
            (("/touch/device/t/calibration", 1.0, 2.0, 3.0), "calibration must be six numbers [a, b, c, d, e, f] as in config.example.toml, each between -100 and 100, or none"),
            (("/touch/device/t/match_usb", "zz"), 'match_usb must be vendor:product in hex, like "0712:0009" (touch-to-osc --list shows it)'),
            (("/touch/device/t/aspect", 50.0), "aspect must be 0 (taken from the device) or between 0.1 and 10"),
            (("/touch/log_level", "loud"), "log_level must be crit, error, warning, notice, info or debug"),
            (("/touch/device/t/connected", 1), "connected is reported by touch-to-osc and cannot be set"),
            (("/touch/device/t/port", "3-3"), "port is reported by touch-to-osc and cannot be set: it is the screen read now; to choose which screen is read, set match_usb, match_serial or match_port"),
            (("/touch/device/t/rotate", 90), "unknown address (did you mean /touch/device/t/rotation?)"),
            (("/touch/restrat",), "unknown address (did you mean /touch/restart?)"),
            (("/touch/health", "ok"), "health is reported by touch-to-osc and cannot be set"),
            (("/touch/allowed_ips", "10.0.0.1"), "allowed_ips is changed with /touch/allowed_ips/add and /touch/allowed_ips/remove"),
            (("/touch2/ping",), "unknown address: every address here starts with /touch/"),
            (("/touch/allowed_ips/add", "nowhere"), "'nowhere' is not an IPv4 address"),
            (("/touch/saved_destinations/add", "1.2.3.4"), 'the destination must be an IPv4 address and a port, like "192.168.1.27:9481"'),
            (("/touch/settings/forget", "device/t/rotation"), "nothing is saved for device/t/rotation"),
            (("/touch/device/t/match_usb", ""), "device t would match nothing: it needs a usb id or a port"),
        ]
        for address_args, why in cases:
            got = req(r, *address_args)
            check(got == ["refused", why], "%s -> %s" % (address_args, got))
        r.none("/touch/event", lambda x: x.args[0] == "settings", during=0.2, since=0)
        check(not os.path.exists(d.settings), "a refused change was saved")


@test()
def a_host_cannot_lock_itself_out():
    with Ctx("lockout") as c:
        p = test_product()
        c.screen(p)
        c.daemon([device(p)], allowed=("127.0.0.1", "10.0.0.9"))
        r = c.receiver()
        connected(r)
        got = req(r, "/touch/allowed_ips/remove", "127.0.0.1")
        check(got == ["refused", "this would lock out 127.0.0.1, the host sending this command"], "own address: %s" % got)
        m = r.mark()
        check(req(r, "/touch/allowed_ips/add", "10.9.8.7") == ["ok", ""], "add")
        r.wait("/touch/allowed_ips", lambda x: x.args == ["127.0.0.1", "10.0.0.9", "10.9.8.7"], since=m)
        r.wait("/touch/overrides", lambda x: x.args == ["allowed_ips"], since=m)
        check(req(r, "/touch/allowed_ips/add", "10.9.8.7") == ["refused", "10.9.8.7 is already allowed"], "twice")
        m = r.mark()
        check(req(r, "/touch/allowed_ips/remove", "10.9.8.7") == ["ok", ""], "remove")
        r.wait("/touch/allowed_ips", lambda x: x.args == ["127.0.0.1", "10.0.0.9"], since=m)
        r.wait("/touch/overrides", lambda x: x.args == [], since=m)
        check(req(r, "/touch/allowed_ips/remove", "10.0.0.9") == ["ok", ""], "remove the other")
        got = req(r, "/touch/allowed_ips/remove", "127.0.0.1")
        check(got == ["refused", "this would lock out every host: at least one must stay allowed"], "last address: %s" % got)


@test()
def a_saved_destination_added_over_osc_gets_touches():
    with Ctx("setdest") as c:
        p = test_product()
        s = c.screen(p)
        c.daemon([device(p)])
        r = c.receiver()
        connected(r)
        plain = c.receiver(subscribe=False)
        m = r.mark()
        check(req(r, "/touch/saved_destinations/add", "127.0.0.1:%d" % plain.port, 1, 0) == ["ok", ""], "add")
        r.wait("/touch/saved_destinations", lambda x: x.args == ["127.0.0.1:%d touches" % plain.port], since=m)
        plain.wait("/touch/alive", timeout=2.5)
        s.touch(0, 1, 10, 10)
        s.frame()
        plain.wait("/touch/t/down")
        check(not plain.all(0, "/touch/health"), "a touches-only destination got the status")
        check(req(r, "/touch/saved_destinations/remove", "127.0.0.1:%d" % plain.port) == ["ok", ""], "remove")
        m2 = plain.mark()
        s.lift(0)
        s.frame()
        plain.none("/touch/t/up", during=0.3, since=m2)


@test()
def label_and_silence_apply_without_a_restart():
    with Ctx("setlive") as c:
        p = test_product()
        s = c.screen(p)
        c.daemon([device(p)])
        r = c.receiver()
        connected(r)
        s.touch(0, 1, 10, 10)
        s.frame()
        c1 = r.wait("/touch/t/down").args[0]
        m = r.mark()
        check(req(r, "/touch/device/t/label", "front screen") == ["ok", ""], "label")
        r.wait("/touch/device/t/label", lambda x: x.args == ["front screen"], since=m)
        check(req(r, "/touch/device/t/silence_ms", 200) == ["ok", ""], "silence")
        cancel = r.wait("/touch/t/cancel", since=m, timeout=2)
        check(cancel.args[:2] == [c1, "device silent"], "the held contact ended %s" % cancel.args)
        check(not r.all(m, "/touch/device/t/state", lambda x: x.args == ["restarting"]), "a live setting restarted the device")


@test()
def enabled_is_saved_and_forget_all_restores_the_config():
    with Ctx("setenabled") as c:
        p = test_product()
        c.screen(p)
        d = c.daemon([device(p)], name="setenabled")
        r = c.receiver()
        connected(r)
        check(req(r, "/touch/device/t/enabled", 0) == ["ok", ""], "off")
        check(req(r, "/touch/log_level", "debug") == ["ok", ""], "log level")
        check(d.stop() == 0, "exit code")
        c.daemons.remove(d)
        c.daemon([device(p)], name="setenabled", keep_settings=True)
        m = r.mark()
        r.wait("/touch/device/t/state", lambda x: x.args == ["disabled"], since=m, timeout=5)
        r.wait("/touch/log_level", lambda x: x.args == ["debug"], since=m)
        r.wait("/touch/overrides", lambda x: sorted(x.args) == ["device/t/enabled", "log_level"], since=m)
        m = r.mark()
        check(req(r, "/touch/settings/forget") == ["ok", ""], "forget all")
        r.wait("/touch/device/t/state", lambda x: x.args == ["ok"], since=m)
        r.wait("/touch/log_level", lambda x: x.args == ["info"], since=m)
        r.wait("/touch/overrides", lambda x: x.args == [], since=m)


@test()
def match_port_moves_reading_to_another_screen():
    with Ctx("setport") as c:
        p = test_product()
        a = c.screen(p, phys="tto-pa-%d" % os.getpid())
        b = c.screen(p, phys="tto-pb-%d" % os.getpid())
        c.daemon([device(p, port="tto-pa-%d" % os.getpid())])
        r = c.receiver()
        connected(r)
        a.touch(0, 7, 50, 50)
        a.frame()
        held = r.wait("/touch/t/down").args[0]
        m = r.mark()
        check(req(r, "/touch/device/t/match_port", "tto-pb-%d" % os.getpid()) == ["ok", ""], "port")
        cancel = r.wait("/touch/t/cancel", since=m)
        check(cancel.args[:2] == [held, "reconfigured"], "cancel %s" % cancel.args)
        a.lift(0)
        a.frame()
        r.wait("/touch/device/t/state", lambda x: x.args == ["restarting"], since=m)
        r.wait("/touch/device/t/state", lambda x: x.args == ["ok"], since=m, timeout=3)
        m2 = r.mark()
        a.touch(0, 1, 10, 10)
        a.frame()
        r.none("/touch/t/down", during=0.3, since=m2)
        b.touch(0, 1, 10, 10)
        b.frame()
        r.wait("/touch/t/down", since=m2)


@test()
def grab_setting_lets_go_of_the_device():
    with Ctx("setgrab") as c:
        p = test_product()
        s = c.screen(p)
        c.daemon([device(p)])
        r = c.receiver()
        connected(r)
        r.wait("/touch/device/t/grabbed", lambda x: x.args == [1])
        m = r.mark()
        check(req(r, "/touch/device/t/grab", 0) == ["ok", ""], "grab 0")
        r.wait("/touch/device/t/grab", lambda x: x.args == [0], since=m)
        r.wait("/touch/device/t/grabbed", lambda x: x.args == [0], since=m, timeout=3)
        check(try_grab(s.node), "the device is still held")


@test()
def a_refused_settings_file_is_set_aside():
    import subprocess
    with Ctx("setrefused") as c:
        p = test_product()
        c.screen(p)
        os.makedirs(RUN_DIR, exist_ok=True)
        path = os.path.join(RUN_DIR, "setrefused-settings.toml")
        for f in (path, path + ".refused"):
            if os.path.exists(f):
                os.remove(f)
        with open(path, "w") as f:
            f.write("[device.t]\nrotation = 45\n")
        cfg = os.path.join(RUN_DIR, "setrefused-check.toml")
        with open(cfg, "w") as f:
            f.write('[osc]\nallowed_ips = ["127.0.0.1"]\n[paths]\nsettings = "%s"\nstate = ""\n[[device]]\nid = "t"\nusb = "%04x:%04x"\n' % (path, TEST_VENDOR, p))
        chk = subprocess.run([os.path.join(BUILD, "touch-to-osc"), "--check-config=" + cfg], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, universal_newlines=True)
        check(chk.returncode == 5 and "device.t.rotation: must be 0, 90, 180 or 270" in chk.stdout, "--check-config said %d: %s" % (chk.returncode, chk.stdout))
        c.daemon([device(p)], name="setrefused", keep_settings=True)
        r = c.receiver()
        connected(r)
        r.wait("/touch/health", lambda x: x.args == ["warning"])
        reasons = r.wait("/touch/reasons", lambda x: any("settings file was refused" in a for a in x.args)).args
        check(any("device.t.rotation: must be 0, 90, 180 or 270" in a and ".refused" in a for a in reasons), "reasons %s" % reasons)
        check(os.path.exists(path + ".refused") and not os.path.exists(path), "the file was not set aside")
        r.wait("/touch/device/t/rotation", lambda x: x.args == [0])


@test()
def an_expired_lease_stops_getting_touches_while_others_renew():
    import threading
    with Ctx("leaserace") as c:
        p = test_product()
        s = c.screen(p)
        c.daemon([device(p)])
        r = c.receiver(subscribe=False)
        stop = [False]

        def hammer():
            while not stop[0]:
                r.subscribe(10)
                time.sleep(0.02)

        threading.Thread(target=hammer, daemon=True).start()
        connected(r)
        short = c.receiver(subscribe=False)
        short.subscribe(1)
        short.wait("/touch/osc_api", timeout=2)
        time.sleep(2.5)
        m = r.mark()
        ms = short.mark()
        s.touch(0, 1, 10, 10)
        s.frame()
        r.wait("/touch/t/down", since=m)
        short.none("/touch/t/down", during=0.4, since=ms)
        stop[0] = True


@test()
def an_unchanged_setting_is_neither_saved_nor_reported():
    with Ctx("setsame") as c:
        p = test_product()
        c.screen(p)
        d = c.daemon([device(p)])
        r = c.receiver()
        connected(r)
        m = r.mark()
        check(req(r, "/touch/device/t/rotation", 0) == ["ok", ""], "same value")
        check(req(r, "/touch/settings/forget") == ["ok", ""], "forget nothing")
        r.none("/touch/event", lambda x: x.args[0] == "settings", during=1.5, since=m)
        check(not os.path.exists(d.settings), "an unchanged value was saved")
        m = r.mark()
        check(req(r, "/touch/device/t/rotation", 90) == ["ok", ""], "rotation 90")
        r.wait("/touch/overrides", lambda x: x.args == ["device/t/rotation"], since=m)
        m = r.mark()
        check(req(r, "/touch/device/t/rotation", 0) == ["ok", ""], "back to the config file's 0")
        r.wait("/touch/overrides", lambda x: x.args == [], since=m)
        deadline = time.time() + 3
        while os.path.exists(d.settings) and time.time() < deadline:
            time.sleep(0.05)
        check(not os.path.exists(d.settings), "the settings file stayed after the only saved setting went back to the config file's value")


@test()
def a_burst_of_settings_is_saved_and_reported_sparingly():
    import threading
    import watch
    with Ctx("setburst") as c:
        p = test_product()
        c.screen(p)
        d = c.daemon([device(p)])
        r = c.receiver()
        connected(r)
        inodes = set()
        stop = [False]

        def watch_file():
            while not stop[0]:
                try:
                    st = os.stat(d.settings)
                    inodes.add((st.st_ino, st.st_mtime_ns))
                except OSError:
                    pass
                time.sleep(0.001)

        t = threading.Thread(target=watch_file, daemon=True)
        t.start()
        m = r.mark()
        for batch in range(4):
            msgs = [watch.encode("/touch/device/t/label", "label %d" % (batch * 50 + k)) for k in range(50)]
            r.sock.sendto(watch.encode_bundle(msgs), ("127.0.0.1", DAEMON_PORT))
        r.wait("/touch/device/t/label", lambda x: x.args == ["label 199"], since=m, timeout=3)
        time.sleep(2.5)
        stop[0] = True
        t.join()
        with open(d.settings) as f:
            text = f.read()
        check("label 199" in text, "the last label was not saved: %s" % text)
        check(len(inodes) <= 2, "the settings file was rewritten %d times for one burst" % len(inodes))
        events = r.all(m, "/touch/event", lambda x: x.args[0] == "settings")
        check(len(events) <= 12, "%d settings events for one burst" % len(events))
        check(any("came too fast" in e.args[1] for e in events), "no summary of the suppressed events: %s" % [e.args for e in events[-3:]])
        r.wait("/touch/alive", lambda x: x.at > time.time() - 1.5, timeout=2)


@test()
def a_removed_failing_destination_clears_the_warning():
    with Ctx("setfail") as c:
        p = test_product()
        c.screen(p)
        c.daemon([device(p)])
        r = c.receiver()
        connected(r)
        m = r.mark()
        check(req(r, "/touch/saved_destinations/add", "255.255.255.255:9") == ["ok", ""], "add")
        r.wait("/touch/reasons", lambda x: any("sending to 255.255.255.255:9 fails" in a for a in x.args), since=m, timeout=3)
        m = r.mark()
        check(req(r, "/touch/saved_destinations/remove", "255.255.255.255:9") == ["ok", ""], "remove")
        r.wait("/touch/health", lambda x: x.args == ["ok"], since=m, timeout=3)


@test()
def the_disabled_reason_says_where_it_came_from():
    with Ctx("offreason") as c:
        p = test_product()
        c.screen(p)
        c.daemon([device(p, enabled=False)])
        r = c.receiver()
        r.wait("/touch/device/t/reason", lambda x: "in the config file (enabled = false)" in x.args[0], timeout=3)
        r.wait("/touch/health", lambda x: x.args == ["ok"], timeout=3)
        m = r.mark()
        check(req(r, "/touch/device/t/enabled", 1) == ["ok", ""], "on")
        r.wait("/touch/device/t/state", lambda x: x.args == ["ok"], since=m)
    with Ctx("offreason2") as c:
        p = test_product()
        c.screen(p)
        c.daemon([device(p)])
        r = c.receiver()
        connected(r)
        m = r.mark()
        check(req(r, "/touch/device/t/enabled", 0) == ["ok", ""], "off")
        r.wait("/touch/device/t/reason", lambda x: "by a saved setting" in x.args[0] and "/touch/device/t/enabled 1" in x.args[0], since=m)


@test()
def text_settings_must_be_ascii():
    with Ctx("ascii") as c:
        p = test_product()
        c.screen(p)
        c.daemon([device(p)])
        r = c.receiver()
        connected(r)
        got = req(r, "/touch/device/t/label", "Gesicht ö")
        check(got == ["refused", "label may only use printable ASCII characters"], "got %s" % got)


@test()
def a_refused_settings_file_keeps_a_screen_turned_off():
    with Ctx("keepoff") as c:
        p = test_product()
        c.screen(p)
        os.makedirs(RUN_DIR, exist_ok=True)
        path = os.path.join(RUN_DIR, "keepoff-settings.toml")
        for f in (path, path + ".refused"):
            if os.path.exists(f):
                os.remove(f)
        with open(path, "w") as f:
            f.write("[device.t]\nenabled = false\nrotation = 45\n")
        c.daemon([device(p)], name="keepoff", keep_settings=True)
        r = c.receiver()
        r.wait("/touch/device/t/state", lambda x: x.args == ["disabled"], timeout=3)
        reasons = r.wait("/touch/reasons", lambda x: any("stays turned off" in a for a in x.args)).args
        check(any("set aside" in a for a in reasons), "reasons %s" % reasons)
        r.wait("/touch/settings_file", lambda x: x.args == [path], timeout=3)
        with open(path) as f:
            check("enabled = false" in f.read(), "the kept setting was not saved again")


@test()
def refusals_to_many_outside_hosts_are_capped():
    import socket
    import watch
    with Ctx("outsiders") as c:
        p = test_product()
        c.screen(p)
        c.daemon([device(p)])
        socks = []
        try:
            for k in range(2, 102):
                s2 = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
                s2.bind(("127.0.0.%d" % k, 19499))
                s2.setblocking(False)
                socks.append(s2)
            for s2 in socks:
                s2.sendto(watch.encode("/touch/ping"), ("127.0.0.1", DAEMON_PORT))
            time.sleep(0.5)
            answered = 0
            for s2 in socks:
                try:
                    s2.recvfrom(65536)
                    answered += 1
                except BlockingIOError:
                    pass
        finally:
            for s2 in socks:
                s2.close()
        MEASURED.append("100 outside hosts at once: %d refusals answered" % answered)
        check(0 < answered <= 25, "%d of 100 outside hosts were answered" % answered)


@test()
def restart_starts_again_with_a_new_run():
    with Ctx("restart") as c:
        p = test_product()
        s = c.screen(p)
        d = c.daemon([device(p)])
        r = c.receiver()
        connected(r)
        run1 = r.wait("/touch/alive", timeout=2.5).args[1]
        s.touch(0, 1, 10, 10)
        s.frame()
        c1 = r.wait("/touch/t/down").args[0]
        m = r.mark()
        check(req(r, "/touch/restart") == ["ok", ""], "restart refused")
        cancel = r.wait("/touch/t/cancel", since=m)
        check(cancel.args[:2] == [c1, "restarting"], "cancel %s" % cancel.args)
        bye = r.wait("/touch/bye", since=m)
        check(bye.args == [run1], "bye %s for run %s" % (bye.args, run1))
        r.wait("/touch/alive", lambda x: x.args[1] != run1, since=m, timeout=6)
        check(d.proc.poll() is None, "the daemon process ended instead of starting again")
        connected(r, since=m)
        m2 = r.mark()
        s.touch(0, 1, 30, 30)
        s.frame()
        r.none("/touch/t/down", during=0.3, since=m2)
        s.lift(0)
        s.frame()
        s.touch(0, 2, 20, 20)
        s.frame()
        r.wait("/touch/t/down", since=m2)
        check(d.log_text().count("(osc_api 6), run") == 2, "expected two start banners in the log")


@test()
def restart_is_refused_when_the_config_would_be():
    with Ctx("restartbad") as c:
        p = test_product()
        c.screen(p)
        d = c.daemon([device(p)])
        r = c.receiver()
        connected(r)
        with open(d.config, "a") as f:
            f.write("\nbogus = 1\n")
        got = req(r, "/touch/restart")
        check(got[0] == "refused" and "keeps running" in got[1] and "bogus: unknown key" in got[1], "got %s" % got)
        time.sleep(0.5)
        check(d.proc.poll() is None, "the daemon stopped")
        r.none("/touch/bye", during=0.2, since=0)


@test()
def a_repeated_restart_request_is_answered_without_restarting_again():
    with Ctx("restartonce") as c:
        p = test_product()
        c.screen(p)
        d = c.daemon([device(p)])
        r = c.receiver()
        connected(r)
        run1 = r.wait("/touch/alive", timeout=2.5).args[1]
        _seq[0] += 1
        s = _seq[0]
        m = r.mark()
        r.send("/touch/request", s, "/touch/restart")
        check(r.wait("/touch/reply", lambda x: x.args[0] == s, since=m).args[1:] == ["ok", ""], "restart")
        run2 = r.wait("/touch/alive", lambda x: x.args[1] != run1, since=m, timeout=6).args[1]
        m = r.mark()
        r.send("/touch/request", s, "/touch/restart")
        check(r.wait("/touch/reply", lambda x: x.args[0] == s, since=m).args[1:] == ["ok", ""], "the repeated request's reply")
        r.none("/touch/bye", during=2.0, since=m)
        check(r.wait("/touch/alive", since=m).args[1] == run2, "the run changed again")
        check(d.log_text().count("(osc_api ") == 2, "expected two start banners in the log")
    with Ctx("addonce") as c:
        pa, pb = test_product(), test_product()
        c.screen(pa)
        c.screen(pb)
        d = c.daemon([device(pa)])
        r = c.receiver()
        connected(r)
        run1 = r.wait("/touch/alive", timeout=2.5).args[1]
        _seq[0] += 1
        s = _seq[0]
        m = r.mark()
        r.send("/touch/request", s, "/touch/devices/add", "b", "7470:%04x" % pb, "")
        first = r.wait("/touch/reply", lambda x: x.args[0] == s, since=m).args[1:]
        check(first == ["ok", "restarting to apply it; then touch-to-osc reads b too"], "add %s" % first)
        run2 = r.wait("/touch/alive", lambda x: x.args[1] != run1, since=m, timeout=6).args[1]
        m = r.mark()
        r.send("/touch/request", s, "/touch/devices/add", "b", "7470:%04x" % pb, "")
        check(r.wait("/touch/reply", lambda x: x.args[0] == s, since=m).args[1:] == first, "the repeated request's reply")
        r.none("/touch/bye", during=1.5, since=m)
        check(r.wait("/touch/alive", since=m).args[1] == run2, "the run changed again")


@test()
def restart_is_refused_when_the_new_listen_port_cannot_be_used():
    import socket
    with Ctx("restartport") as c:
        p = test_product()
        c.screen(p)
        d = c.daemon([device(p)])
        r = c.receiver()
        connected(r)
        taken = DAEMON_PORT + 19
        busy = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        busy.bind(("127.0.0.1", taken))
        try:
            with open(d.config) as f:
                text = f.read()
            with open(d.config, "w") as f:
                f.write(text.replace("127.0.0.1:%d" % DAEMON_PORT, "127.0.0.1:%d" % taken))
            got = req(r, "/touch/restart")
            check(got[0] == "refused" and "would listen on UDP 127.0.0.1:%d, which cannot be used" % taken in got[1], "got %s" % got)
            time.sleep(0.5)
            check(d.proc.poll() is None, "the daemon stopped")
        finally:
            busy.close()


@test()
def restart_is_refused_when_the_installed_program_would_not_start():
    with Ctx("restartnew") as c:
        p = test_product()
        c.screen(p)
        bindir = os.path.join(RUN_DIR, "restartnew-bin")
        shutil.rmtree(bindir, ignore_errors=True)
        os.makedirs(bindir)
        exe = os.path.join(bindir, "touch-to-osc")
        shutil.copy2(os.path.join(BUILD, "touch-to-osc"), exe)
        d = c.daemon([device(p)], binary=exe)
        r = c.receiver()
        connected(r)
        newer = exe + ".new"
        with open(newer, "w") as f:
            f.write("#!/bin/sh\necho \"this version refuses the config: osc.newkey is missing\"\nexit 2\n")
        os.chmod(newer, 0o755)
        os.rename(newer, exe)
        got = req(r, "/touch/restart")
        check(got[0] == "refused" and "would not start" in got[1] and "osc.newkey is missing" in got[1], "got %s" % got)
        time.sleep(0.5)
        check(d.proc.poll() is None, "the daemon stopped")
        shutil.rmtree(bindir, ignore_errors=True)


@test()
def a_usb_reset_keeps_a_marker_and_holds_off_a_restart():
    root = fake_usb_tree("usb-marker")
    devices = os.path.join(root, "sys", "bus", "usb", "devices")
    unbind = os.path.join(root, "sys", "bus", "usb", "drivers", "hub", "unbind")
    os.remove(os.path.join(devices, "usb3", "busnum"))
    with open(os.path.join(devices, "usb3", "busnum"), "w") as f:
        f.write("3\n")
    os.remove(unbind)
    os.mkfifo(unbind)
    for name, product in (("3-1", "Test Keyboard\n"), ("3-1:1.0", None), ("3-4.1", ""), ("4-1", "Elsewhere\n"), ("3-3.1", "Behind the screen\n")):
        os.makedirs(os.path.join(devices, name))
        if product is not None:
            with open(os.path.join(devices, name, "product"), "w") as f:
                f.write(product)
    with Ctx("usbmarker") as c:
        d = c.daemon([device(test_product(), port="3-3")], env={"TTO_USB_ROOT": root})
        r = c.receiver()
        r.wait("/touch/device/t/usb_watched_port", lambda x: x.args == ["3-3"])
        m = r.mark()
        check(req(r, "/touch/device/t/usb_reset") == ["ok", ""], "usb_reset")
        r.wait("/touch/event", lambda x: x.args[0] == "usb" and "resetting USB bus 3" in x.args[1], since=m, timeout=3)
        marker = d.state + ".usb-reset"
        deadline = time.time() + 3
        while not os.path.exists(marker) and time.time() < deadline:
            time.sleep(0.05)
        check(os.path.exists(marker), "no marker while the reset runs")
        with open(marker) as f:
            check(f.read() == "3-0:1.0\n", "the marker does not name the hub")
        got = req(r, "/touch/restart")
        check(got[0] == "refused" and "a USB reset is running" in got[1], "a restart during a reset: %s" % got)
        with open(unbind) as f:
            f.read()
        done = r.wait("/touch/event", lambda x: x.args[0] == "usb" and "USB bus 3 reset in" in x.args[1], since=m, timeout=3)
        check("(it also dropped Test Keyboard (3-1), a USB device (3-4.1))" in done.args[1], "what the reset dropped: %s" % done.args[1])
        check(not os.path.exists(marker), "the marker stayed after the hub was bound again")


@test(slow=True)
def a_stop_waits_for_a_usb_reset_in_flight_and_binds_the_hub():
    root = fake_usb_tree("usb-stopwait")
    devices = os.path.join(root, "sys", "bus", "usb", "devices")
    hub = os.path.join(root, "sys", "bus", "usb", "drivers", "hub")
    os.remove(os.path.join(devices, "usb3", "busnum"))
    with open(os.path.join(devices, "usb3", "busnum"), "w") as f:
        f.write("3\n")
    unbind = os.path.join(hub, "unbind")
    os.remove(unbind)
    os.mkfifo(unbind)
    with Ctx("usbstopwait") as c:
        d = c.daemon([device(test_product(), port="3-3")], env={"TTO_USB_ROOT": root})
        r = c.receiver()
        r.wait("/touch/device/t/usb_watched_port", lambda x: x.args == ["3-3"])
        m = r.mark()
        check(req(r, "/touch/device/t/usb_reset") == ["ok", ""], "usb_reset")
        r.wait("/touch/event", lambda x: x.args[0] == "usb" and "resetting USB bus 3" in x.args[1], since=m, timeout=3)
        d.proc.send_signal(signal.SIGTERM)
        time.sleep(3)
        check(d.proc.poll() is None, "the daemon stopped while a USB reset was between its unbind and its bind")
        with open(unbind) as f:
            f.read()
        d.proc.wait(10)
        check(d.proc.returncode == 0, "exit %s" % d.proc.returncode)
        with open(os.path.join(hub, "bind")) as f:
            check(f.read().startswith("3-0:1.0"), "the hub was not bound again before the stop")
        check(not os.path.exists(d.state + ".usb-reset"), "the marker stayed")


@test(slow=True)
def a_restart_waits_for_a_usb_port_check_in_flight():
    root = fake_usb_tree("usb-restartwait")
    busnum = os.path.join(root, "sys", "bus", "usb", "devices", "usb3", "busnum")
    with Ctx("usbrestartwait") as c:
        d = c.daemon([device(test_product(), port="3-3")], env={"TTO_USB_ROOT": root})
        r = c.receiver()
        run1 = r.wait("/touch/alive", timeout=2.5).args[1]
        time.sleep(1.5)
        m = r.mark()
        check(req(r, "/touch/restart") == ["ok", ""], "restart refused")
        r.wait("/touch/bye", since=m)
        r.none("/touch/alive", lambda x: x.args[1] != run1, during=2.5, since=m)
        check("a USB port check is still waiting" not in d.log_text(), "it went on without waiting: %s" % d.log_text()[-800:])
        with open(busnum, "w") as f:
            f.write("3\n")
        r.wait("/touch/alive", lambda x: x.args[1] != run1, since=m, timeout=5)


@test()
def a_hub_left_unbound_by_a_cut_short_reset_is_bound_at_start():
    import threading
    root = fake_usb_tree("usb-cutshort")
    devices = os.path.join(root, "sys", "bus", "usb", "devices")
    driver = os.path.join(devices, "3-0:1.0", "driver")
    bind = os.path.join(root, "sys", "bus", "usb", "drivers", "hub", "bind")
    os.rmdir(driver)
    os.remove(bind)
    os.mkfifo(bind)
    written = []
    done = threading.Event()

    def kernel():
        while not done.is_set():
            with open(bind) as f:
                data = f.read()
            if done.is_set():
                return
            written.append(data)
            os.makedirs(driver, exist_ok=True)

    t = threading.Thread(target=kernel, daemon=True)
    t.start()
    for f in ("usbcutshort-settings.toml", "usbcutshort-state.toml"):
        if os.path.exists(os.path.join(RUN_DIR, f)):
            os.remove(os.path.join(RUN_DIR, f))
    marker = os.path.join(RUN_DIR, "usbcutshort-state.toml.usb-reset")
    with open(marker, "w") as f:
        f.write("3-0:1.0\n")
    try:
        with Ctx("usbcutshort") as c:
            d = c.daemon([device(test_product(), port="3-3")], name="usbcutshort", keep_settings=True, env={"TTO_USB_ROOT": root})
            deadline = time.time() + 3
            note = "was left unbound by a reset cut short when touch-to-osc last stopped; it is bound again"
            while note not in d.log_text() and time.time() < deadline:
                time.sleep(0.05)
            check(note in d.log_text(), "no note in the log")
            check(written and written[0].startswith("3-0:1.0"), "the hub was not bound at start: %s" % written)
            check(not os.path.exists(marker), "the marker stayed")
            r = c.receiver()
            r.wait("/touch/reasons", lambda x: not any("left unbound" in a for a in x.args))
    finally:
        done.set()
        try:
            os.close(os.open(bind, os.O_WRONLY | os.O_NONBLOCK))
        except OSError:
            pass


@test()
def a_screen_is_watched_on_the_port_it_was_last_read_on_after_a_restart():
    root = fake_usb_tree("usb-remember")
    busnum = os.path.join(root, "sys", "bus", "usb", "devices", "usb3", "busnum")
    os.remove(busnum)
    with open(busnum, "w") as f:
        f.write("3\n")
    for f in ("usbremember-settings.toml",):
        if os.path.exists(os.path.join(RUN_DIR, f)):
            os.remove(os.path.join(RUN_DIR, f))
    with open(os.path.join(RUN_DIR, "usbremember-state.toml"), "w") as f:
        f.write('[usb_ports]\nt = "3-3"\n')
    with Ctx("usbremember") as c:
        c.daemon([device(test_product())], name="usbremember", keep_settings=True, env={"TTO_USB_ROOT": root})
        r = c.receiver()
        r.wait("/touch/device/t/usb_watched_port", lambda x: x.args == ["3-3"], timeout=3)


@test()
def a_settings_write_stuck_on_the_disk_never_holds_up_the_heartbeat():
    with Ctx("stuckdisk") as c:
        p = test_product()
        c.screen(p)
        d = c.daemon([device(p)])
        r = c.receiver()
        connected(r)
        tmp = "%s.tmp.%d" % (d.settings, d.proc.pid)
        os.mkfifo(tmp)
        try:
            m = r.mark()
            check(req(r, "/touch/device/t/label", "stuck") == ["ok", ""], "label")
            time.sleep(4)
            alive = r.all(m, "/touch/alive")
            gaps = [b.at - a.at for a, b in zip(alive, alive[1:])]
            check(len(alive) >= 3 and max(gaps) < 1.5, "%d alives while a settings write is stuck, gaps %s" % (len(alive), gaps))
            r.wait("/touch/device/t/label", lambda x: x.args == ["stuck"], since=m)
        finally:
            fd = os.open(tmp, os.O_RDONLY | os.O_NONBLOCK)
            time.sleep(0.5)
            os.close(fd)


@test()
def a_refused_config_runs_on_the_copy_that_last_worked():
    with Ctx("lastgood") as c:
        p = test_product()
        s = c.screen(p)
        d = c.daemon([device(p)], name="lastgood")
        r = c.receiver()
        connected(r)
        deadline = time.time() + 3
        while time.time() < deadline:
            if os.path.exists(d.state) and "config_that_worked" in open(d.state).read():
                break
            time.sleep(0.05)
        check(d.stop() == 0, "the first daemon's exit code")
        r.close()
        d2 = c.daemon([device(p)], name="lastgood", keep_settings=True, extra="bogus = 1")
        r2 = c.receiver()
        connected(r2)
        r2.wait("/touch/health", lambda x: x.args == ["warning"])
        reasons = r2.wait("/touch/reasons", lambda x: any("the config file is refused" in a for a in x.args)).args
        check(any("osc.bogus: unknown key" in a and "copy of it that last worked" in a for a in reasons), "reasons %s" % reasons)
        m = r2.mark()
        s.touch(0, 1, 100, 100)
        s.frame()
        r2.wait("/touch/t/down", since=m)
        s.lift(0)
        s.frame()
        chk = subprocess.run([os.path.join(BUILD, "touch-to-osc"), "--check-config=" + d2.config], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                             universal_newlines=True)
        check(chk.returncode == 2 and "osc.bogus: unknown key" in chk.stdout and "copy of it that last worked" in chk.stdout,
              "--check-config said %d: %s" % (chk.returncode, chk.stdout))
    cfg = os.path.join(RUN_DIR, "nocopy.toml")
    with open(cfg, "w") as f:
        f.write('[osc]\nlisten = "127.0.0.1:%d"\nallowed_ips = ["127.0.0.1"]\nbogus = 1\n[paths]\nsettings = ""\nstate = ""\n[[device]]\nid = "t"\nusb = "7470:0001"\n'
                % DAEMON_PORT)
    run = subprocess.run([os.path.join(BUILD, "touch-to-osc"), "--config=" + cfg], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, universal_newlines=True,
                         timeout=10)
    check(run.returncode == 2 and "config refused" in run.stdout, "without a copy that worked: %d: %s" % (run.returncode, run.stdout))


@test()
def text_from_outside_is_never_echoed_unprintable():
    with Ctx("echo") as c:
        p = test_product()
        c.screen(p)
        c.daemon([device(p)])
        r = c.receiver()
        connected(r)
        got = req(r, "/touch/devéice/t/label", "x")
        check(got == ["refused", "a request's address must be an OSC address: a slash, then printable characters"], "address %s" % got)
        got = req(r, "/touch/allowed_ips/add", "10.0.0.é")
        check(got == ["refused", "that is not an IPv4 address"], "allowed_ips %s" % got)
        got = req(r, "/touch/settings/forget", "devé")
        check(got == ["refused", "nothing is saved under that name"], "forget %s" % got)


@test()
def a_config_edit_made_after_a_setting_was_saved_over_osc_wins_at_the_next_start():
    with Ctx("newer") as c:
        p = test_product()
        c.screen(p)
        d = c.daemon([device(p)], name="newer")
        r = c.receiver()
        connected(r)
        check(req(r, "/touch/device/t/rotation", 90) == ["ok", ""], "rotation")
        check(req(r, "/touch/allowed_ips/add", "10.0.0.5") == ["ok", ""], "allowed host")
        check(d.stop() == 0, "the first daemon's exit code")
        r.close()
        d2 = c.daemon([device(p, rotation=180)], name="newer", keep_settings=True, allowed=("127.0.0.1", "10.0.0.9"))
        r2 = c.receiver()
        connected(r2)
        r2.wait("/touch/device/t/rotation", lambda x: x.args == [180])
        r2.wait("/touch/allowed_ips", lambda x: x.args == ["127.0.0.1", "10.0.0.9", "10.0.0.5"])
        r2.wait("/touch/overrides", lambda x: x.args == ["allowed_ips"])
        check("device/t/rotation: the config file changed from 0 to 180 after 90 was saved over OSC, so the config file's 180 applies now" in d2.log_text(),
              "no note in the log")
        check("10.0.0.9, 10.0.0.5" in d2.log_text(), "the start banner does not list the allowed hosts")


@test()
def outsiders_are_told_where_the_allowed_hosts_come_from():
    with Ctx("whyrefused") as c:
        p = test_product()
        c.screen(p)
        c.daemon([device(p)], allowed=("127.0.0.1", "127.0.0.2"))
        r = c.receiver()
        connected(r)
        check(req(r, "/touch/allowed_ips/remove", "127.0.0.2") == ["ok", ""], "remove")
        import socket
        import watch
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.bind(("127.0.0.2", 0))
        s.settimeout(3)
        try:
            s.sendto(watch.encode("/touch/ping"), ("127.0.0.1", DAEMON_PORT))
            data, _ = s.recvfrom(65536)
        finally:
            s.close()
        _, msgs = watch.decode_packet(data)
        address, args = msgs[0]
        check(address == "/touch/error" and "taken off the allowed hosts over OSC" in args[1] and "/touch/allowed_ips/add 127.0.0.2" in args[1],
              "refusal %s %s" % (address, args))


@test()
def a_finger_reported_again_after_a_silence_cancel_gets_a_hint_once():
    with Ctx("quiethint") as c:
        p = test_product()
        s = c.screen(p)
        c.daemon([device(p, silence_ms=200)])
        r = c.receiver()
        connected(r)
        hint = lambda x: x.args[0] == "silence" and "reported the same finger again" in x.args[1]
        m = r.mark()
        s.touch(0, 1, 100, 100)
        s.frame()
        c1 = r.wait("/touch/t/down", since=m).args[0]
        r.wait("/touch/t/cancel", lambda x: x.args[:2] == [c1, "device silent"], since=m, timeout=2)
        r.none("/touch/event", hint, during=0.2, since=m)
        s.touch(0, 1, 140, 100)
        s.frame()
        got = r.wait("/touch/event", hint, since=m, timeout=2)
        check("set silence_ms = 0 for it" in got.args[1] and "check its USB cable" in got.args[1] and got.args[2] == "warning", "hint %s" % got.args)
        s.lift(0)
        s.frame()
        m2 = r.mark()
        s.touch(0, 2, 100, 100)
        s.frame()
        c2 = r.wait("/touch/t/down", since=m2).args[0]
        r.wait("/touch/t/cancel", lambda x: x.args[:2] == [c2, "device silent"], since=m2, timeout=2)
        s.touch(0, 2, 150, 100)
        s.frame()
        r.none("/touch/event", hint, during=0.5, since=m2)
    with Ctx("quietlift") as c:
        p = test_product()
        s = c.screen(p)
        c.daemon([device(p, silence_ms=200)])
        r = c.receiver()
        connected(r)
        m = r.mark()
        s.touch(0, 1, 100, 100)
        s.frame()
        c1 = r.wait("/touch/t/down", since=m).args[0]
        r.wait("/touch/t/cancel", lambda x: x.args[:2] == [c1, "device silent"], since=m, timeout=2)
        s.lift(0)
        s.frame()
        s.touch(0, 2, 140, 100)
        s.frame()
        r.wait("/touch/t/down", since=m)
        r.none("/touch/event", lambda x: "reported the same finger again" in x.args[1], during=0.5, since=m)


@test()
def a_screen_that_reports_no_size_says_its_aspect_is_a_guess():
    with Ctx("aspect") as c:
        p = test_product()
        c.daemon([device(p)])
        r = c.receiver()
        r.wait("/touch/device/t/connected", lambda x: x.args == [0], timeout=3)
        c.screen(p, resolution=0)
        connected(r)
        got = r.wait("/touch/event", lambda x: "reports no physical size" in x.args[1], timeout=2)
        check("its aspect 1.78" in got.args[1] and "set aspect for it" in got.args[1] and got.args[2] == "notice", "event %s" % got.args)
    with Ctx("aspectsized") as c:
        p = test_product()
        c.screen(p)
        c.daemon([device(p)])
        r = c.receiver()
        connected(r)
        r.none("/touch/event", lambda x: "reports no physical size" in x.args[1], during=0.5, since=0)


@test()
def health_and_its_reasons_always_come_in_one_bundle():
    with Ctx("healthpair") as c:
        p = test_product()
        c.screen(p)
        c.daemon([device(p)])
        r = c.receiver()
        connected(r)
        r.wait("/touch/health", lambda x: x.args == ["ok"], timeout=3)
        m = r.mark()
        check(req(r, "/touch/device/t/enabled", 0) == ["ok", ""], "off")
        h = r.wait("/touch/health", lambda x: x.args == ["warning"], since=m)
        check(r.all(m, "/touch/reasons", lambda x: x.bundle == h.bundle), "the health came without its reasons")
        m = r.mark()
        check(req(r, "/touch/device/t/label", "the other name") == ["ok", ""], "label")
        reasons = r.wait("/touch/reasons", lambda x: any("the other name is turned off" in a for a in x.args), since=m)
        check(r.all(m, "/touch/health", lambda x: x.bundle == reasons.bundle and x.args == ["warning"]), "reasons that changed came without the health")


@test()
def subscribers_are_kept_over_a_restart_and_hear_the_new_run_at_once():
    with Ctx("keeplease") as c:
        p = test_product()
        s = c.screen(p)
        d = c.daemon([device(p)], allowed=("127.0.0.1", "127.0.0.2"))
        r = c.receiver(subscribe=False)
        r.subscribe(30)
        gone = c.receiver(subscribe=False, host="127.0.0.2")
        gone.subscribe(30)
        connected(r)
        run1 = r.wait("/touch/alive", timeout=2.5).args[1]
        gone.wait("/touch/alive", timeout=2.5)
        with open(d.config) as f:
            text = f.read()
        with open(d.config, "w") as f:
            f.write(text.replace('allowed_ips = ["127.0.0.1", "127.0.0.2"]', 'allowed_ips = ["127.0.0.1"]'))
        m = r.mark()
        mg = gone.mark()
        check(req(r, "/touch/restart") == ["ok", ""], "restart refused")
        bye = r.wait("/touch/bye", since=m)
        alive = r.wait("/touch/alive", lambda x: x.args[1] != run1, since=m, timeout=3)
        MEASURED.append("restart: the new run's first alive %.0f ms after the bye, to a subscriber kept over it" % ((alive.at - bye.at) * 1000))
        check(alive.at - bye.at < 0.8, "the new run's first alive came %.2f s after the bye" % (alive.at - bye.at))
        r.wait("/touch/run", lambda x: x.args[0] != run1, since=m, timeout=1)
        connected(r, since=m)
        m2 = r.mark()
        s.touch(0, 1, 20, 20)
        s.frame()
        r.wait("/touch/t/down", since=m2)
        gone.none("/touch/alive", lambda x: x.args[1] != run1, during=1.5, since=mg)
        text = d.log_text()
        check("subscriber 127.0.0.1:%d kept over the restart" % r.port in text, "log: %s" % text[-1500:])
        check("subscriber 127.0.0.2:%d was not kept over the restart: it is no longer in osc.allowed_ips" % gone.port in text, "log: %s" % text[-1500:])


@test()
def list_names_the_device_reading_each_screen_and_a_block_for_the_rest():
    with Ctx("list") as c:
        p = test_product()
        pid = os.getpid()
        c.screen(p, phys="tto-la-%d" % pid)
        c.screen(p, phys="tto-lb-%d" % pid)
        cfg = os.path.join(RUN_DIR, "list.toml")
        with open(cfg, "w") as f:
            f.write('[osc]\nallowed_ips = ["127.0.0.1"]\n[paths]\nsettings = ""\nstate = ""\n[[device]]\nid = "screen"\nusb = "%04x:%04x"\n' % (TEST_VENDOR, p))
        time.sleep(0.3)
        out = subprocess.run([os.path.join(BUILD, "touch-to-osc"), "--list", "--config=" + cfg], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                             universal_newlines=True, timeout=20).stdout
        blocks = out.split("\n\n")
        a = [b for b in blocks if "tto-la-%d" % pid in b]
        b = [b for b in blocks if "tto-lb-%d" % pid in b and "id %04x" % TEST_VENDOR in b]
        check(len(a) == 1 and 'read as device "screen" in the config' in a[0], "screen a: %s" % out)
        check(len(b) == 1 and "not read by the config; to read it, add this to the config:" in b[0] and 'match_usb = "%04x:%04x"' % (TEST_VENDOR, p) in b[0] and
              'match_port = "tto-lb-%d"' % pid in b[0] and 'id = "screen2"' in b[0], "screen b: %s" % out)
        check("touch-to-osc --list --all shows them" in out and "not a touchscreen" not in out, "others: %s" % out)
        everything = subprocess.run([os.path.join(BUILD, "touch-to-osc"), "--list", "--all", "--config=" + cfg], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                    universal_newlines=True, timeout=20).stdout
        check("not a touchscreen" in everything and "--list --all shows them" not in everything, "--all: %s" % everything)
        dump = subprocess.run([os.path.join(BUILD, "touch-to-osc"), "--dump=nope", "--config=" + cfg], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                              universal_newlines=True, timeout=20)
        check(dump.returncode == 2 and 'no device "nope" in the config (its devices: screen)' in dump.stdout, "--dump=nope: %d %s" % (dump.returncode, dump.stdout))


@test()
def status_and_send_work_from_the_command_line():
    def cli(config, *args):
        return subprocess.run([os.path.join(BUILD, "touch-to-osc"), "--config=" + config] + list(args), stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                              universal_newlines=True, timeout=20)

    with Ctx("cli") as c:
        p = test_product()
        c.screen(p)
        d = c.daemon([device(p, label="the screen")])
        r = c.receiver()
        connected(r)
        st = subprocess.run([os.path.join(BUILD, "touch-to-osc"), "--config", d.config, "--status"], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            universal_newlines=True, timeout=20)
        check(st.returncode == 0 and "health: ok" in st.stdout and "t (the screen): ok" in st.stdout and "reading tto-test-" in st.stdout, st.stdout)
        sent = cli(d.config, "--send", "device/t/rotation", "90")
        check(sent.returncode == 0 and sent.stdout == "ok\n", "rotation: %d %s" % (sent.returncode, sent.stdout))
        r.wait("/touch/device/t/rotation", lambda x: x.args == [90], timeout=3)
        bad = cli(d.config, "--send", "device/t/rotation", "45")
        check(bad.returncode == 6 and "refused: rotation must be 0, 90, 180 or 270" in bad.stdout, "45: %d %s" % (bad.returncode, bad.stdout))
        check(cli(d.config, "--send", "device/t/label", "s:42").returncode == 0, "label as text")
        r.wait("/touch/device/t/label", lambda x: x.args == ["42"], timeout=3)
        cal = cli(d.config, "--send", "/touch/device/t/calibration", "1", "0", "-0.02", "0", "1", "0")
        check(cal.returncode == 0, "calibration: %s" % cal.stdout)
        r.wait("/touch/device/t/calibration", lambda x: len(x.args) == 6 and abs(x.args[2] + 0.02) < 1e-6, timeout=3)
        st = cli(d.config, "--status")
        saved = [line for line in st.stdout.splitlines() if line.startswith("saved over OSC (these win over the config file): ")]
        check(len(saved) == 1 and all(n in saved[0] for n in ("device/t/label", "device/t/rotation", "device/t/calibration")), st.stdout)
        empty = cli(d.config, "--send")
        check(empty.returncode == 1 and "--send needs an address" in empty.stdout, "no address: %d %s" % (empty.returncode, empty.stdout))
    cfg = os.path.join(RUN_DIR, "cli-nobody.toml")
    with open(cfg, "w") as f:
        f.write('[osc]\nlisten = "127.0.0.1:%d"\nallowed_ips = ["127.0.0.1"]\n[[device]]\nid = "t"\nusb = "7470:0001"\n' % DAEMON_PORT)
    st = cli(cfg, "--status")
    check(st.returncode == 6 and "no answer from touch-to-osc on 127.0.0.1:%d" % DAEMON_PORT in st.stdout, "nobody: %d %s" % (st.returncode, st.stdout))


def raw_receiver(port, host="127.0.0.1"):
    import socket
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind((host, port))
    s.settimeout(0.2)
    return s


def raw_messages(sock, during):
    """Every message that arrives within `during` seconds, with its type tags: (address, tags, args)."""
    import socket
    import watch
    out = []
    end = time.time() + during
    while time.time() < end:
        try:
            data, _ = sock.recvfrom(65536)
        except socket.timeout:
            continue
        for part in split_packet(data):
            address, pos = watch._string(part, 0)
            tags, _ = watch._string(part, pos)
            out.append((address, tags, watch.decode_message(part)[1]))
    return out


def split_packet(data):
    import struct
    if not data.startswith(b"#bundle\0"):
        return [data]
    out, pos = [], 16
    while pos < len(data):
        size = struct.unpack_from(">I", data, pos)[0]
        out += split_packet(data[pos + 4:pos + 4 + size])
        pos += 4 + size
    return out


@test()
def health_is_per_device_and_every_event_names_its_device():
    with Ctx("devhealth") as c:
        pa, pb = test_product(), test_product()
        c.screen(pa)
        c.daemon([device(pa, id="a"), device(pb, id="b")])
        r = c.receiver()
        connected(r, "a")
        r.wait("/touch/device/b/health", lambda x: x.args == ["fault"], timeout=3)
        reasons = r.wait("/touch/device/b/reasons", lambda x: x.args and "no input device matches" in x.args[0], timeout=3)
        r.wait("/touch/device/a/health", lambda x: x.args == ["ok"], timeout=3)
        r.wait("/touch/device/a/reasons", lambda x: x.args == [], timeout=3)
        r.wait("/touch/health", lambda x: x.args == ["fault"], timeout=3)
        sub = r.wait("/touch/event", lambda x: x.args[0] == "subscriber", timeout=2)
        check(sub.args[3] == "", "a subscriber event names a device: %s" % sub.args)
        m = r.mark()
        c.screen(pb)
        back = r.wait("/touch/event", lambda x: x.args[0] == "device" and x.args[1].startswith("b connected: "), since=m, timeout=4)
        check(back.args[2:] == ["notice", "b"], "the event %s" % back.args)
        ok = r.wait("/touch/device/b/health", lambda x: x.args == ["ok"], since=m, timeout=3)
        same = [x.address for x in r.all(m) if x.bundle == ok.bundle]
        for want in ("/touch/health", "/touch/reasons", "/touch/device/a/health", "/touch/device/a/reasons", "/touch/device/b/reasons"):
            check(want in same, "%s not in the bundle of device/b/health: %s" % (want, same))
        check(same[0] == "/touch/status_gen", "the bundle does not start with its number: %s" % same)
        r.wait("/touch/health", lambda x: x.args == ["ok"], since=m, timeout=3)
        check(reasons.args[0].startswith("b: "), "device reasons %s" % reasons.args)


@test()
def last_loss_and_losses_say_a_screen_dropped_out():
    with Ctx("lastloss") as c:
        p = test_product()
        s = c.screen(p)
        c.daemon([device(p)])
        r = c.receiver()
        connected(r)
        r.wait("/touch/device/t/last_loss_s", lambda x: x.args == [-1.0], timeout=2.5)
        r.wait("/touch/device/t/last_gone_s", lambda x: x.args == [-1.0], timeout=2.5)
        m = r.mark()
        s.destroy()
        connected(r, value=0, since=m)
        gone = r.wait("/touch/device/t/last_loss_s", lambda x: x.args[0] >= 0, since=m, timeout=2.5)
        check(r.all(m, "/touch/device/t/last_gone_s", lambda x: x.bundle == gone.bundle)[0].args == [-1.0], "last_gone_s while the screen is gone")
        r.wait("/touch/event", lambda x: x.args[0] == "device" and " lost: " in x.args[1], since=m)
        m2 = r.mark()
        time.sleep(0.8)
        c.screen(p)
        connected(r, since=m2, timeout=4)
        back = r.wait("/touch/event", lambda x: x.args[0] == "device" and " connected: " in x.args[1], since=m2, timeout=3)
        check(", back after " in back.args[1], "the connected event says how long it was gone: %s" % back.args[1])
        lasted = r.wait("/touch/device/t/last_gone_s", lambda x: x.args[0] > 0, since=m, timeout=2.5).args[0]
        check(0.8 <= lasted < 4, "last_gone_s %s" % lasted)
        got = r.wait("/touch/device/t/last_loss_s", lambda x: 0 <= x.args[0] < 3, since=m, timeout=2.5)
        later = r.wait("/touch/device/t/last_loss_s", lambda x: x.at > got.at + 1.5, since=m, timeout=3)
        check(later.args[0] > got.args[0], "last_loss_s does not grow: %s then %s" % (got.args, later.args))
        r.wait("/touch/device/t/counts", lambda x: x.args[5] == 1, since=m, timeout=2.5)


@test()
def status_bundles_are_numbered_and_the_alive_carries_the_number():
    with Ctx("statusgen") as c:
        p = test_product()
        c.screen(p)
        c.daemon([device(p)])
        r = c.receiver()
        connected(r)
        time.sleep(1.2)
        m = r.mark()
        for k in range(4):
            check(req(r, "/touch/device/t/label", "label %d" % k) == ["ok", ""], "label %d" % k)
            r.wait("/touch/device/t/label", lambda x: x.args == ["label %d" % k], since=m)
        gens = [x.args[0] for x in r.all(m, "/touch/status_gen")]
        check(len(gens) >= 4 and all(b == a + 1 for a, b in zip(gens, gens[1:])), "status bundles are not numbered one by one: %s" % gens)
        for g in r.all(m, "/touch/status_gen"):
            first = [x for x in r.all(0) if x.bundle == g.bundle][0]
            check(first.address == "/touch/status_gen", "a status bundle starts with %s" % first.address)
        alive = r.wait("/touch/alive", lambda x: x.at > r.all(m, "/touch/status_gen")[-1].at, since=m, timeout=2.5)
        latest = [x.args[0] for x in r.all(m, "/touch/status_gen") if x.at <= alive.at][-1]
        check(len(alive.args) == 8 and alive.args[7] == latest, "the alive carries %s, the last status bundle was %d" % (alive.args, latest))
        m2 = r.mark()
        r.send("/touch/status/send")
        full = r.wait("/touch/osc_api", since=m2)
        full_gen = [x.args[0] for x in r.all(m2, "/touch/status_gen") if x.bundle == full.bundle]
        before = [x.args[0] for x in r.all(m, "/touch/status_gen") if x.at < full.at and x.bundle != full.bundle][-1]
        check(full_gen and full_gen[0] == before, "a full status to one receiver took a new number: %s after %d" % (full_gen, before))
        m3 = r.mark()
        check(req(r, "/touch/device/t/label", "after") == ["ok", ""], "label after")
        nxt = r.wait("/touch/status_gen", lambda x: x.args[0] > before, since=m3)
        check(nxt.args[0] == before + 1, "the next change bundle is %d after %d" % (nxt.args[0], before))


@test()
def answers_reach_the_asking_hosts_lease_ports_too():
    with Ctx("leaseanswers") as c:
        p = test_product()
        c.screen(p)
        c.daemon([device(p)])
        r = c.receiver()
        connected(r)
        other = c.receiver(subscribe=False)
        m, mo = r.mark(), other.mark()
        other.send("/touch/request", 41, "/touch/device/t/label", "from another port")
        other.wait("/touch/reply", lambda x: x.args[0] == 41, since=mo)
        r.wait("/touch/reply", lambda x: x.args[0] == 41, since=m)
        other.send("/touch/ping")
        other.wait("/touch/pong", since=mo)
        r.wait("/touch/pong", since=m)
        named = c.receiver(subscribe=False)
        mn = named.mark()
        other.send("/touch/subscribe", named.port, 99)
        named.wait("/touch/error", lambda x: x.args[0] == "/touch/subscribe" and "1-60 seconds" in x.args[1], since=mn)
        other.wait("/touch/error", lambda x: x.args[0] == "/touch/subscribe", since=mo)
        check(len(r.all(m, "/touch/reply", lambda x: x.args[0] == 41)) == 1, "the lease port got the reply twice")


@test()
def a_subscription_can_ask_for_touches_status_or_plain_types():
    with Ctx("subwhat") as c:
        p = test_product()
        s = c.screen(p)
        c.daemon([device(p)])
        t = c.receiver(subscribe=False)
        t.send("/touch/subscribe", t.port, 10, "touches")
        check(t.wait("/touch/subscribed").args == [t.port, 10, "touches"], "subscribed touches")
        st = c.receiver(subscribe=False)
        st.send("/touch/subscribe", st.port, 10, "status")
        check(st.wait("/touch/subscribed").args == [st.port, 10, "status"], "subscribed status")
        plain = raw_receiver(c.next_port)
        c.next_port += 1
        import watch
        plain.sendto(watch.encode("/touch/subscribe", plain.getsockname()[1], 10, "all plain"), ("127.0.0.1", DAEMON_PORT))
        connected(st)
        t.wait("/touch/alive", timeout=2.5)
        m_t, m_s = t.mark(), st.mark()
        s.touch(0, 1, 100, 100)
        s.frame()
        s.lift(0)
        s.frame()
        t.wait("/touch/t/up", since=m_t)
        msgs = raw_messages(plain, 2.5)
        st.none("/touch/t/down", during=0.3, since=m_s)
        check(not t.all(0, "/touch/health") and not t.all(0, "/touch/event"), "a touches lease got status or events")
        st.wait("/touch/health", timeout=1)
        sub = [x for x in msgs if x[0] == "/touch/subscribed"]
        check(sub and sub[0][2] == [plain.getsockname()[1], 10, "all plain"], "subscribed plain %s" % sub)
        downs = [x for x in msgs if x[0] == "/touch/t/down"]
        check(downs and downs[0][1] == ",iffi", "a plain down %s" % downs)
        counts = [x for x in msgs if x[0] == "/touch/device/t/counts"]
        check(counts and counts[0][1] == ",iiiiii", "plain counts %s" % counts[:1])
        check(not any("h" in x[1] for x in msgs), "a plain lease got an h argument: %s" % [x[:2] for x in msgs if "h" in x[1]][:3])
        bad = c.receiver(subscribe=False)
        bad.send("/touch/subscribe", bad.port, 10, "everything")
        check("is not one of all, touches, status, plain" in bad.wait("/touch/error").args[1], "a bad what")
        plain.close()


@test()
def doubles_work_and_other_argument_types_are_answered():
    with Ctx("types") as c:
        p = test_product()
        c.screen(p)
        c.daemon([device(p)])
        r = c.receiver()
        connected(r)
        m = r.mark()
        r.send("/touch/request", ("d", 7.0), "/touch/device/t/rotation", ("d", 90.0))
        check(r.wait("/touch/reply", lambda x: x.args[0] == 7, since=m).args == [7, "ok", ""], "doubles")
        r.wait("/touch/device/t/rotation", lambda x: x.args == [90], since=m)
        r.send("/touch/device/t/label", ("b", b"\x01\x02\x03"))
        e = r.wait("/touch/error", since=m)
        check(e.args == ["/touch/device/t/label", "argument 1 is a blob (b): commands take numbers (i, h, f, d, T, F) and text (s)"], "blob %s" % e.args)


@test()
def hello_is_answered_for_any_host():
    with Ctx("hello") as c:
        p = test_product()
        c.screen(p)
        c.daemon([device(p)])
        r = c.receiver(subscribe=False)
        r.send("/touch/hello")
        here = r.wait("/touch/here")
        with open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "CMakeLists.txt")) as f:
            version = f.read().split("project(touch-to-osc VERSION ", 1)[1].split()[0]
        check(here.args[0] and here.args[1:] == [version, 6, DAEMON_PORT, 1, 1], "here %s" % here.args)
        out = raw_receiver(c.next_port, "127.0.0.2")
        import watch
        out.sendto(watch.encode("/touch/hello"), ("127.0.0.1", DAEMON_PORT))
        got = raw_messages(out, 1.0)
        check([x[2][2:] for x in got if x[0] == "/touch/here"] == [[6, DAEMON_PORT, 1, 0]], "here for an outsider %s" % got)
        out.sendto(watch.encode("/touch/ping"), ("127.0.0.1", DAEMON_PORT))
        got = raw_messages(out, 1.0)
        check([x[0] for x in got] == ["/touch/error"] and "may not send commands here" in got[0][2][1], "an outsider's ping %s" % got)
        out.close()


@test()
def identify_pauses_touches_reports_each_screen_touched_and_ends_by_itself():
    with Ctx("identify") as c:
        p = test_product()
        s = c.screen(p)
        c.daemon([device(p, label="front")])
        r = c.receiver()
        connected(r)
        m0 = r.mark()
        s.touch(0, 1, 960, 540)
        s.frame()
        held = r.wait("/touch/t/down", since=m0).args[0]
        m = r.mark()
        check(req(r, "/touch/identify", 5) == ["ok", ""], "identify on")
        cancel = r.wait("/touch/t/cancel", since=m)
        check(cancel.args[:2] == [held, "identifying"], "cancel %s" % cancel.args)
        r.wait("/touch/device/t/state", lambda x: x.args == ["identifying"], since=m)
        r.wait("/touch/identify_s", lambda x: 0 < x.args[0] <= 5, since=m)
        r.wait("/touch/reasons", lambda x: any("touches are paused while screens are identified" in a for a in x.args), since=m)
        on = r.wait("/touch/event", lambda x: x.args[0] == "identify" and "for 5 s" in x.args[1], since=m)
        check(on.args[3] == "", "identify on names a device: %s" % on.args)
        s.lift(0)
        s.frame()
        s.touch(0, 2, 480, 270)
        s.frame()
        got = r.wait("/touch/identified", since=m)
        check(got.args[:3] == ["t", "front", ""] and abs(got.args[3] - 0.25) < 0.01 and abs(got.args[4] - 0.25) < 0.01, "identified %s" % got.args)
        ev = r.wait("/touch/event", lambda x: x.args[0] == "identify" and "was touched at 0.25, 0.25" in x.args[1], since=m)
        check(ev.args[3] == "t", "the touch event %s" % ev.args)
        ma = r.mark()
        active = r.wait("/touch/device/t/active", since=ma, timeout=2.5)
        check(active.args[1:] == [], "the active list while identifying %s" % active.args)
        check(not r.all(m, "/touch/t/down"), "a touch went out while identifying")
        md = r.mark()
        check(req(r, "/touch/device/t/enabled", 0) == ["ok", ""], "off while identifying")
        r.wait("/touch/device/t/enabled", lambda x: x.args == [0], since=md)
        check(req(r, "/touch/device/t/enabled", 1) == ["ok", ""], "back on")
        r.none("/touch/t/cancel", during=0.3, since=md)
        m2 = r.mark()
        check(req(r, "/touch/identify", 0) == ["ok", ""], "identify off")
        r.wait("/touch/device/t/state", lambda x: x.args == ["ok"], since=m2)
        r.wait("/touch/identify_s", lambda x: x.args == [0], since=m2)
        s.touch(0, 2, 500, 300)
        s.frame()
        r.none("/touch/t/down", during=0.3, since=m2)
        s.lift(0)
        s.frame()
        s.touch(0, 3, 100, 100)
        s.frame()
        r.wait("/touch/t/down", since=m2)
        s.lift(0)
        s.frame()
        r.wait("/touch/t/up", since=m2)
        contact_story(r.all(0))
        m3 = r.mark()
        check(req(r, "/touch/identify", 1) == ["ok", ""], "identify for 1 s")
        r.wait("/touch/event", lambda x: x.args[0] == "identify" and "its time ran out" in x.args[1], since=m3, timeout=2.5)
        r.wait("/touch/device/t/state", lambda x: x.args == ["ok"], since=m3, timeout=1)
        s.touch(0, 4, 100, 100)
        s.frame()
        r.wait("/touch/t/down", since=m3)
        check(req(r, "/touch/identify", 61)[0] == "refused", "61 s")


@test()
def guided_calibration_reports_raw_touches_and_a_fit_applies_and_saves_the_numbers():
    with Ctx("calibrate") as c:
        p = test_product()
        s = c.screen(p)
        d = c.daemon([device(p, calibration=[1, 0, 0.1, 0, 1, 0])])
        r = c.receiver()
        connected(r)
        m = r.mark()
        check(req(r, "/touch/device/t/calibrate", 10) == ["ok", ""], "calibrate on")
        r.wait("/touch/device/t/state", lambda x: x.args == ["calibrating"], since=m)
        r.wait("/touch/device/t/health", lambda x: x.args == ["warning"], since=m)
        r.wait("/touch/device/t/calibrate_s", lambda x: 0 < x.args[0] <= 10, since=m)
        check(req(r, "/touch/identify", 5)[0] == "refused", "identify while calibrating")
        targets = [(0.1, 0.1), (0.9, 0.1), (0.1, 0.9), (0.9, 0.9)]
        points = []
        for k, (x, y) in enumerate(targets):
            # the panel is offset: each target is touched 0.02 to the right of where it is shown
            s.touch(0, 10 + k, int((x + 0.02) * 1920), int(y * 1080))
            s.frame()
            found = r.wait("/touch/device/t/calibration/touch", lambda m_: m_.args[1] == "down", since=m, count=k + 1)
            got = found[-1] if k else found
            s.lift(0)
            s.frame()
            points += [x, y, got.args[2], got.args[3]]
        ups = r.wait("/touch/device/t/calibration/touch", lambda m_: m_.args[1] == "up", since=m, count=4)
        check(len(ups) == 4, "the lifts")
        downs = r.all(m, "/touch/device/t/calibration/touch", lambda m_: m_.args[1] == "down")
        check(abs(downs[0].args[2] - 0.12) < 0.002, "u is not the uncalibrated position: %s" % downs[0].args)
        check(not r.all(m, "/touch/t/down"), "a touch went out while calibrating")
        m2 = r.mark()
        got = req(r, "/touch/device/t/calibration/fit", *points)
        check(got[0] == "ok" and got[1].startswith("fitted from 4 points: worst miss 0.0% of the screen ("), "fit %s" % got)
        fitted = r.wait("/touch/device/t/calibration/fitted", since=m2)
        check(fitted.args[0] < 0.001 and fitted.args[1] >= 0, "fitted %s" % fitted.args)
        cal = r.wait("/touch/device/t/calibration", since=m2).args
        check(abs(cal[0] - 1) < 0.01 and abs(cal[2] + 0.02) < 0.002, "the calibration %s" % cal)
        r.wait("/touch/event", lambda x: x.args[0] == "calibrate" and "fitted from 4 points" in x.args[1] and x.args[3] == "t", since=m2)
        r.wait("/touch/overrides", lambda x: "device/t/calibration" in x.args, since=m2)
        check(req(r, "/touch/device/t/calibrate", 0) == ["ok", ""], "calibrate off")
        r.wait("/touch/device/t/state", lambda x: x.args == ["ok"], since=m2)
        m3 = r.mark()
        s.touch(0, 30, int(0.52 * 1920), 540)
        s.frame()
        down = r.wait("/touch/t/down", since=m3)
        check(abs(down.args[1] - 0.5) < 0.005, "a touch after the fit lands at %s" % down.args)
        s.lift(0)
        s.frame()
        refused = [
            ((0.1, 0.1, 0.1, 0.1, 0.9, 0.9, 0.9, 0.9), "takes 3 to 16 points"),
            ((0.1, 0.1, 0.1, 0.1, 0.5, 0.5, 0.5, 0.5, 0.9, 0.9, 0.9, 0.9), "lie on one line"),
            ((0.1, 0.1, 0.1, 0.1, 0.9, 0.1, 0.9, 0.1, 0.1, 0.9, 0.1, 0.9, 0.9, 0.9, 0.5, 0.5), "worst miss"),
            ((1.5, 0.1, 0.1, 0.1, 0.9, 0.1, 0.9, 0.1, 0.1, 0.9, 0.1, 0.9), "0..1 of the display"),
        ]
        for args, why in refused:
            got = req(r, "/touch/device/t/calibration/fit", *args)
            check(got[0] == "refused" and why in got[1], "%s: %s" % (why, got))
        contact_story(r.all(0))
    with open(d.settings) as f:
        check("calibration" in f.read(), "the fitted calibration was not saved")


def description(r, since=None):
    import json
    m = r.mark() if since is None else since
    r.send("/touch/describe")
    done = r.wait("/touch/describe/done", since=m, timeout=3)
    items = [json.loads(x.args[0]) for x in r.all(m, "/touch/describe/item")]
    check(len(items) == done.args[0], "describe sent %d items and said %d" % (len(items), done.args[0]))
    return {(i.get("kind"), i.get("address", i.get("id"))): i for i in items}


@test()
def describe_lists_every_setting_action_and_device_with_its_values():
    with Ctx("describe") as c:
        p = test_product()
        c.screen(p)
        c.daemon([device(p, label="front")])
        r = c.receiver()
        connected(r)
        d = description(r)
        rot = d[("setting", "device/t/rotation")]
        check(rot["type"] == "int" and rot["values"] == [0, 90, 180, 270] and rot["value"] == 0 and rot["config"] == 0 and rot["source"] == "config" and
              rot["effect"] == "cancels" and rot["device"] == "t", "rotation %s" % rot)
        check(d[("device", "t")]["label"] == "front", "device %s" % d[("device", "t")])
        check(d[("action", "device/t/usb_reset")]["confirm"].startswith("Every device on"), "usb_reset %s" % d[("action", "device/t/usb_reset")])
        check(d[("action", "identify")]["args"] == [{"name": "seconds", "type": "int", "range": [0, 60]}], "identify %s" % d[("action", "identify")])
        for key in [("setting", "device/t/%s" % k) for k in ("label", "enabled", "mirror", "calibration", "aspect", "silence_ms", "grab", "match_usb",
                                                                   "match_serial", "match_port", "usb_recovery")] + \
                   [("setting", k) for k in ("log_level", "allowed_ips", "saved_destinations", "listen", "prefix")] + \
                   [("action", k) for k in ("restart", "devices/add", "devices/remove", "settings/forget", "device/t/calibrate", "device/t/calibration/fit")]:
            check(key in d, "describe misses %s" % (key,))
        check(d[("setting", "listen")]["effect"] == "restart" and d[("setting", "listen")]["value"] == "127.0.0.1:%d" % DAEMON_PORT, "listen %s" % d[("setting", "listen")])
        check(req(r, "/touch/device/t/rotation", 90) == ["ok", ""], "rotation 90")
        time.sleep(0.6)
        d = description(r)
        rot = d[("setting", "device/t/rotation")]
        check(rot["value"] == 90 and rot["config"] == 0 and rot["source"] == "saved", "rotation after a change %s" % rot)
        m = r.mark()
        r.send("/touch/describe")
        r.send("/touch/describe")
        r.wait("/touch/error", lambda x: "at most 2 descriptions a second" in x.args[1], since=m)


@test()
def devices_added_and_removed_over_osc_are_saved_and_applied_by_a_restart():
    with Ctx("devadd") as c:
        pa, pb = test_product(), test_product()
        c.screen(pa)
        d = c.daemon([device(pa)])
        r = c.receiver()
        connected(r)
        m = r.mark()
        sb = c.screen(pb)
        ours = lambda args: [a for a in args if " 7470:" in a]
        unused = r.wait("/touch/unused_screens", lambda x: any("7470:%04x" % pb in a for a in x.args), since=m, timeout=8)
        check(len(ours(unused.args)) == 1, "unused %s" % unused.args)
        r.wait("/touch/event", lambda x: x.args[0] == "screens" and "7470:%04x" % pb in x.args[1] and "devices/add" in x.args[1], since=m, timeout=2)
        for args, why in [(("t", "7470:%04x" % pb, ""), "there is a device t already"), (("B", "7470:%04x" % pb, ""), "the id may only use"),
                          (("device", "7470:%04x" % pb, ""), "is a name touch-to-osc uses"), (("b", "", ""), "would match nothing"),
                          (("b", "zz", ""), "match_usb must be")]:
            got = req(r, "/touch/devices/add", *args)
            check(got[0] == "refused" and why in got[1], "%s: %s" % (args, got))
        run1 = r.wait("/touch/alive", timeout=2.5).args[1]
        m = r.mark()
        import watch
        r.sock.sendto(watch.encode_bundle([watch.encode("/touch/request", 901, "/touch/devices/add", "b", "7470:%04x" % pb, ""),
                                           watch.encode("/touch/device/t/rotation", 90)]), ("127.0.0.1", DAEMON_PORT))
        got = r.wait("/touch/reply", lambda x: x.args[0] == 901, since=m).args[1:]
        check(got == ["ok", "restarting to apply it; then touch-to-osc reads b too"], "add %s" % got)
        late = r.wait("/touch/error", lambda x: x.args[0] == "/touch/device/t/rotation", since=m)
        check("touch-to-osc is restarting" in late.args[1], "a command after it in the bundle: %s" % late.args)
        r.wait("/touch/bye", since=m)
        r.wait("/touch/alive", lambda x: x.args[1] != run1, since=m, timeout=5)
        r.wait("/touch/devices", lambda x: x.args == ["t", "b"], since=m, timeout=3)
        connected(r, "b", since=m, timeout=4)
        r.wait("/touch/overrides", lambda x: "devices" in x.args, since=m, timeout=3)
        with open(d.settings) as f:
            check("devices_added" in f.read(), "the added device was not saved")
        r.wait("/touch/unused_screens", lambda x: ours(x.args) == [], since=m, timeout=8)
        check(req(r, "/touch/device/b/label", "the second") == ["ok", ""], "a setting on the added device")
        run2 = r.wait("/touch/alive", timeout=2.5).args[1]
        m = r.mark()
        got = req(r, "/touch/devices/remove", "b")
        check(got == ["ok", "restarting to apply it; then touch-to-osc no longer reads b"], "remove %s" % got)
        r.wait("/touch/alive", lambda x: x.args[1] != run2, since=m, timeout=5)
        r.wait("/touch/devices", lambda x: x.args == ["t"], since=m, timeout=3)
        r.wait("/touch/unused_screens", lambda x: len(ours(x.args)) == 1, since=m, timeout=8)
        got = req(r, "/touch/devices/remove", "t")
        check(got[0] == "refused" and "the only device" in got[1], "remove the last %s" % got)
        got = req(r, "/touch/devices/remove", "zz")
        check(got[0] == "refused" and "no device zz" in got[1], "remove a missing one %s" % got)
        m = r.mark()
        sb.destroy()
        r.wait("/touch/event", lambda x: x.args[0] == "screens" and "went away" in x.args[1], since=m, timeout=8)
        check(d.log_text().count("(osc_api 6), run") == 3, "expected three runs")


@test()
def listen_and_prefix_changed_over_osc_move_the_daemon_after_its_restart():
    import socket
    with Ctx("listen") as c:
        p = test_product()
        c.screen(p)
        d = c.daemon([device(p)])
        r = c.receiver()
        connected(r)
        busy = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        busy.bind(("127.0.0.1", DAEMON_PORT + 9))
        try:
            got = req(r, "/touch/listen", "127.0.0.1:%d" % (DAEMON_PORT + 9))
            check(got[0] == "refused" and "cannot be used" in got[1], "a busy port %s" % got)
        finally:
            busy.close()
        check(req(r, "/touch/listen", "nowhere")[0] == "refused", "a bad address")
        run1 = r.wait("/touch/alive", timeout=2.5).args[1]
        m = r.mark()
        got = req(r, "/touch/listen", "127.0.0.1:%d" % (DAEMON_PORT + 8))
        check(got == ["ok", "restarting to apply it; then touch-to-osc listens on 127.0.0.1:%d: send commands there" % (DAEMON_PORT + 8)], "listen %s" % got)
        r.wait("/touch/alive", lambda x: x.args[1] != run1, since=m, timeout=5)
        r.target = ("127.0.0.1", DAEMON_PORT + 8)
        m = r.mark()
        r.send("/touch/ping")
        r.wait("/touch/pong", since=m)
        r.wait("/touch/listen", lambda x: x.args == ["127.0.0.1:%d" % (DAEMON_PORT + 8)], timeout=3)
        run2 = r.wait("/touch/alive", timeout=2.5).args[1]
        m = r.mark()
        got = req(r, "/touch/prefix", "/t2")
        check(got == ["ok", "restarting to apply it; then every address starts with /t2"], "prefix %s" % got)
        r.wait("/t2/alive", lambda x: x.args[1] != run2, since=m, timeout=5)
        r.prefix = "/t2"
        m = r.mark()
        r.send("/t2/ping")
        r.wait("/t2/pong", since=m)
        _seq[0] += 1
        s = _seq[0]
        m = r.mark()
        r.send("/t2/request", s, "/t2/settings/forget")
        reply = r.wait("/t2/reply", lambda x: x.args[0] == s, since=m)
        check(reply.args[1] == "ok", "forget %s" % reply.args)
        r.prefix = "/touch"
        r.target = ("127.0.0.1", DAEMON_PORT)
        r.wait("/touch/alive", since=m, timeout=6)
        connected(r, since=m, timeout=4)


@test()
def list_touch_names_the_unread_screen_that_was_touched():
    with Ctx("listtouch") as c:
        pa, pb = test_product(), test_product()
        pid = os.getpid()
        a = c.screen(pa, phys="tto-ta-%d" % pid)
        b = c.screen(pb, phys="tto-tb-%d" % pid)
        cfg = os.path.join(RUN_DIR, "listtouch.toml")
        with open(cfg, "w") as f:
            f.write('[osc]\nallowed_ips = ["127.0.0.1"]\n[paths]\nsettings = ""\nstate = ""\n[[device]]\nid = "t"\nusb = "%04x:%04x"\n' % (TEST_VENDOR, pa))
        time.sleep(0.3)
        proc = subprocess.Popen([os.path.join(BUILD, "touch-to-osc"), "--list", "--touch", "8", "--config=" + cfg], stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, universal_newlines=True)
        time.sleep(1.0)
        a.touch(0, 1, 100, 100)
        a.frame()
        time.sleep(0.3)
        b.touch(0, 1, 100, 100)
        b.frame()
        out, _ = proc.communicate(timeout=12)
        check(proc.returncode == 0 and "touched: " in out and "tto-tb-%d" % pid in out and 'match_usb = "%04x:%04x"' % (TEST_VENDOR, pb) in out and
              "tto-ta-%d" % pid not in out.split("touched: ", 1)[1], "--list --touch: %d %s" % (proc.returncode, out))
        quiet = subprocess.run([os.path.join(BUILD, "touch-to-osc"), "--list", "--touch=1", "--config=" + cfg], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                               universal_newlines=True, timeout=10)
        check(quiet.returncode == 1 and "nothing was touched in 1 s" in quiet.stdout, "nothing touched: %d %s" % (quiet.returncode, quiet.stdout))


@test()
def a_journal_stream_variable_alone_does_not_change_how_it_logs():
    with Ctx("journalvar") as c:
        p = test_product()
        c.screen(p)
        d = c.daemon([device(p)], env={"JOURNAL_STREAM": "1:1"})
        r = c.receiver()
        connected(r)
        text = d.log_text()
        check("NOTICE touch-to-osc" in text and not any(line.startswith("<") for line in text.splitlines()), "log %s" % text[:400])


@test()
def check_config_without_root_says_to_use_sudo():
    os.makedirs("/tmp/tto-e2e", exist_ok=True)
    os.chmod("/tmp/tto-e2e", 0o755)
    settings = "/tmp/tto-e2e/sudo-settings.toml"
    with open(settings, "w") as f:
        f.write('[device.t]\nlabel = "x"\n')
    os.chmod(settings, 0o640)
    cfg = "/tmp/tto-e2e/sudo.toml"
    with open(cfg, "w") as f:
        f.write('[osc]\nallowed_ips = ["127.0.0.1"]\n[paths]\nsettings = "%s"\nstate = ""\n[[device]]\nid = "t"\nusb = "7470:0001"\n' % settings)
    os.chmod(cfg, 0o644)
    chk = subprocess.run(["setpriv", "--reuid=65534", "--regid=65534", "--clear-groups", os.path.join(BUILD, "touch-to-osc"), "--check-config", cfg],
                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT, universal_newlines=True)
    check(chk.returncode == 0 and "can only be read by root: run it with sudo" in chk.stdout, "--check-config said %d: %s" % (chk.returncode, chk.stdout))
    chk = subprocess.run([os.path.join(BUILD, "touch-to-osc"), "--check-config", cfg], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, universal_newlines=True)
    check(chk.returncode == 0 and "wins over the config for: device/t/label" in chk.stdout, "as root: %d: %s" % (chk.returncode, chk.stdout))


@test()
def usb_reset_needs_a_screen_on_usb_and_recovery_is_a_saved_setting():
    with Ctx("usbcmd") as c:
        p = test_product()
        c.screen(p)
        c.daemon([device(p)])
        r = c.receiver()
        connected(r)
        got = req(r, "/touch/device/t/usb_reset")
        check(got == ["refused", "t has not been seen on a USB port since touch-to-osc started and no port is set for it, so there is nothing to reset: plug the screen"
                      " in again, or set match_port to the USB port it is plugged into, such as 3-3"], "got %s" % got)
        check(req(r, "/touch/device/t/usb_reset", 1) == ["refused", "usb_reset takes no arguments"], "arguments")
        m = r.mark()
        check(req(r, "/touch/device/t/usb_recovery", 0) == ["ok", ""], "off")
        r.wait("/touch/device/t/usb_recovery", lambda x: x.args == [0], since=m)
        r.wait("/touch/overrides", lambda x: x.args == ["device/t/usb_recovery"], since=m)
        check(req(r, "/touch/device/t/usb_recovery", 2) == ["refused", "usb_recovery needs 0 or 1"], "bad value")
        r.none("/touch/event", lambda x: x.args[0] == "usb", during=1.5, since=0)


@test()
def a_screen_that_drops_out_is_counted_and_health_is_ok_again_once_it_reads():
    with Ctx("losses") as c:
        p = test_product()
        s = c.screen(p)
        c.daemon([device(p)])
        r = c.receiver()
        connected(r)
        counts = r.wait("/touch/device/t/counts", timeout=2.5).args
        check(len(counts) == 6 and counts[5] == 0, "counts %s" % counts)
        m = r.mark()
        s.destroy()
        lost = r.wait("/touch/event", lambda x: x.args[0] == "device" and " lost: " in x.args[1], since=m)
        check(lost.args[2:] == ["warning", "t"], "the lost event %s" % lost.args)
        m1 = r.mark()
        c.screen(p)
        back = r.wait("/touch/event", lambda x: x.args[0] == "device" and " connected: " in x.args[1], since=m1, timeout=4)
        check(back.args[2:] == ["notice", "t"], "the connected event %s" % back.args)
        r.wait("/touch/health", lambda x: x.args == ["fault"], since=m, timeout=3)
        r.wait("/touch/device/t/state", lambda x: x.args == ["ok"], since=m, timeout=3)
        m2 = r.mark()
        r.wait("/touch/device/t/counts", lambda x: x.args[5] == 1, since=m, timeout=2.5)
        time.sleep(2.5)
        health = r.all(m, "/touch/health")
        check(health and health[-1].args == ["ok"], "health after the screen came back: %s" % health[-3:])
        r.none("/touch/health", lambda x: x.args != ["ok"], during=0.1, since=m2)
        r.none("/touch/reasons", lambda x: x.args != [], during=0.1, since=m2)


def fake_usb_tree(name):
    """A /sys and /dev for TTO_USB_ROOT with port 3-3 on bus 3, whose busnum is a FIFO: a port check reading it waits until
    the test writes to it, the way a check waits for the hub's lock while the kernel retries a device."""
    root = os.path.join(RUN_DIR, name)
    shutil.rmtree(root, ignore_errors=True)
    devices = os.path.join(root, "sys", "bus", "usb", "devices")
    hub = os.path.join(root, "sys", "bus", "usb", "drivers", "hub")
    os.makedirs(os.path.join(devices, "usb3"))
    os.makedirs(os.path.join(devices, "3-0:1.0", "driver"))
    os.makedirs(hub)
    os.makedirs(os.path.join(root, "dev", "bus", "usb", "003"))
    for f in ("bind", "unbind"):
        open(os.path.join(hub, f), "w").close()
    with open(os.path.join(devices, "usb3", "devnum"), "w") as f:
        f.write("1\n")
    os.mkfifo(os.path.join(devices, "usb3", "busnum"))
    return root


@test(slow=True)
def a_port_check_waiting_on_the_kernel_never_holds_up_the_heartbeat():
    root = fake_usb_tree("usb-root")
    busnum = os.path.join(root, "sys", "bus", "usb", "devices", "usb3", "busnum")
    driver = os.path.join(root, "sys", "bus", "usb", "devices", "3-0:1.0", "driver")
    hub = os.path.join(root, "sys", "bus", "usb", "drivers", "hub")

    def let_one_check_through():
        with open(busnum, "w") as f:
            f.write("3\n")

    with Ctx("usbwait") as c:
        d = c.daemon([device(test_product(), port="3-3")], env={"TTO_USB_ROOT": root})
        r = c.receiver()
        r.wait("/touch/device/t/usb_watched_port", lambda x: x.args == ["3-3"])
        r.wait("/touch/device/t/usb_port_state", lambda x: x.args == ["being set up"], timeout=4)
        check("TTO_USB_ROOT is set" in d.log_text(), "no warning about TTO_USB_ROOT in the log")
        m = r.mark()
        time.sleep(4)
        alive = r.all(m, "/touch/alive")
        gaps = [b.at - a.at for a, b in zip(alive, alive[1:])]
        check(len(alive) >= 3 and max(gaps) < 1.5, "%d alives while a port check waits, gaps %s" % (len(alive), gaps))
        check(req(r, "/touch/device/t/usb_reset") == ["ok", ""], "usb_reset while a check waits")
        check(req(r, "/touch/device/t/usb_reset") == ["refused", "a USB reset is already running; try again in a moment"], "a second usb_reset")
        r.none("/touch/event", lambda x: x.args[0] == "usb" and "resetting" in x.args[1], during=1.0, since=m)
        m = r.mark()
        let_one_check_through()
        unknown = r.wait("/touch/event", lambda x: x.args[0] == "usb" and "cannot tell whether anything is plugged into USB port 3-3" in x.args[1], since=m, timeout=3)
        check("003/001" in unknown.args[1] and unknown.args[2:] == ["warning", "t"], "event %s" % unknown.args)
        started = r.wait("/touch/event", lambda x: x.args[0] == "usb" and "t: resetting USB bus 3 as asked by 127.0.0.1" in x.args[1], since=m, timeout=3)
        check(started.args[2:] == ["warning", "t"], "event %s" % started.args)
        done = r.wait("/touch/event", lambda x: x.args[0] == "usb" and "t: USB bus 3 reset in " in x.args[1], since=m, timeout=3)
        check(done.args[2:] == ["notice", "t"], "event %s" % done.args)
        for f in ("unbind", "bind"):
            with open(os.path.join(hub, f)) as fh:
                check(fh.read().startswith("3-0:1.0"), "%s was not written" % f)
        os.rmdir(driver)
        r.wait("/touch/device/t/usb_port_state", lambda x: x.args == ["being set up"], since=m, timeout=4)
        m = r.mark()
        check(req(r, "/touch/device/t/usb_reset") == ["ok", ""], "usb_reset with a hub that does not bind")
        let_one_check_through()
        failed = r.wait("/touch/event", lambda x: x.args[0] == "usb" and "could not be bound again (the kernel did not take it)" in x.args[1], since=m, timeout=4)
        check(failed.args[2:] == ["error", "t"], "event %s" % failed.args)
        r.wait("/touch/reasons", lambda x: any("could not be bound again" in a for a in x.args), since=m, timeout=3)
        r.wait("/touch/device/t/usb_port_state", lambda x: x.args == ["being set up"] and x.at > failed.at, since=m, timeout=4)
        m = r.mark()
        t0 = time.time()
        rc = d.stop()
        took = time.time() - t0
        check(rc == 0 and took < 5, "with a port check waiting, the daemon stopped in %.1f s with exit %s" % (took, rc))
        r.wait("/touch/bye", since=m, timeout=1)
        check("a USB port check is still waiting in the kernel; stopping without waiting for it" in d.log_text(), "no note about the waiting check")


def replay(name, silence_ms=200, after=None):
    with Ctx("trace-" + name) as c:
        p = test_product()
        s = c.screen(p)
        c.daemon([device(p, silence_ms=silence_ms)])
        r = c.receiver()
        connected(r)
        m = r.mark()
        t0 = time.time()
        s.replay(os.path.join(TRACES, "qdtech-%s.evemu" % name))
        time.sleep(0.3)
        msgs = r.all(m)
        stories = contact_story(msgs)
        extra = after(r) if after else None
        return msgs, stories, t0, extra


@test()
def real_trace_tap_and_doubletap():
    for name, taps in (("tap", 1), ("doubletap", 2)):
        msgs, stories, t0, _ = replay(name)
        check(len(stories) == taps, "%s: %d contacts" % (name, len(stories)))
        check(all(st[-1] == "up" for st in stories.values()), "%s ended %s" % (name, stories))


@test()
def real_trace_corners():
    msgs, stories, t0, _ = replay("corners-top")
    downs = [m.args for m in msgs if m.address == "/touch/t/down"]
    check(len(downs) == 2, "corners-top: %s" % downs)
    check(downs[0][1] < 0.05 and downs[0][2] < 0.06 and downs[1][1] > 0.95 and downs[1][2] < 0.06, "top corners at %s" % downs)


@test()
def real_trace_ten_fingers_and_palm():
    msgs, stories, t0, _ = replay("ten")
    check(len(stories) == 10, "ten: %d contacts" % len(stories))
    msgs, stories, t0, _ = replay("palm")
    check(len(stories) >= 10 and all(st[-1] == "up" for st in stories.values()), "palm: %s" % stories)


@test()
def real_trace_pinch_drag_five():
    for name, n in (("pinch", 2), ("drag", 1), ("five", 5)):
        msgs, stories, t0, _ = replay(name)
        check(len(stories) == n, "%s: %d contacts" % (name, len(stories)))
        check(not [m for m in msgs if m.address == "/touch/t/cancel"], "%s: cancelled" % name)


@test(slow=True)
def real_trace_hold_30_s_is_never_cancelled():
    msgs, stories, t0, _ = replay("hold")
    check(len(stories) == 1 and list(stories.values())[0][-1] == "up", "hold: %s" % stories)


@test(slow=True)
def real_trace_cable_wiggle():
    msgs, stories, t0, lat = replay("wiggle", after=lambda r: r.wait("/touch/device/t/latency_us", lambda m: m.args[0] > 0, timeout=2.5).args)
    check(not [m for m in msgs if m.address == "/touch/t/cancel"], "wiggle: cancelled")
    MEASURED.append("latency on the wiggle replay (frame time to sent): p50 %d us, p99 %d us" % (lat[0], lat[1]))


@test()
def real_trace_extender_pull_with_silence():
    msgs, stories, t0, _ = replay("pull", silence_ms=200)
    cancels = [m for m in msgs if m.address == "/touch/t/cancel"]
    check(len(cancels) == 1 and cancels[0].args[1] == "device silent", "pull: %s" % [m.args for m in cancels])
    at = cancels[0].at - t0
    MEASURED.append("extender pull replay: cancelled %.2f s after the replay began (last frame at 2.67 s, the kernel's release at 6.37 s)" % at)
    check(2.8 < at < 3.3, "cancelled at %.2f s" % at)


@test()
def real_trace_extender_pull_without_silence():
    msgs, stories, t0, _ = replay("pull", silence_ms=0)
    cancels = [m for m in msgs if m.address == "/touch/t/cancel"]
    check(len(cancels) == 1 and cancels[0].args[1] == "device lost", "pull: %s" % [m.args for m in cancels])
    check(not [m for m in msgs if m.address == "/touch/t/up"], "the kernel's release became an up")
    at = cancels[0].at - t0
    check(6.3 < at < 6.7, "cancelled at %.2f s" % at)


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    quick = "--quick" in sys.argv
    if os.geteuid() != 0:
        print("run as root (uinput and the input devices need it)")
        return 2
    selected = [t for t in TESTS if (not args or t.__name__ in args or any(a in t.__name__ for a in args)) and not (quick and t.__name__ in SLOW)]
    failed = 0
    for t in selected:
        started = time.time()
        try:
            t()
            print("ok    %s (%.1f s)" % (t.__name__, time.time() - started), flush=True)
        except Failure as e:
            failed += 1
            print("FAIL  %s (%.1f s): %s" % (t.__name__, time.time() - started, e), flush=True)
        except Exception:  # noqa: BLE001
            failed += 1
            print("FAIL  %s: %s" % (t.__name__, traceback.format_exc()), flush=True)
    for m in MEASURED:
        print("measured: " + m)
    print("%d of %d passed" % (len(selected) - failed, len(selected)))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
