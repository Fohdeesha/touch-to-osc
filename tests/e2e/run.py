#!/usr/bin/env python3
"""End-to-end tests: the daemon, virtual touchscreens through uinput, a receiver like the engine.

usage: tests/e2e/run.py [--quick] [NAME ...]   (as root, on a machine with /dev/uinput)

Each test starts its own daemon on 127.0.0.1:19480 with test devices (USB vendor 7470,
never a real screen's id) and receivers on 19481-19499. --quick skips the tests that wait
more than 10 s. TTO_BUILD picks the build directory.
"""

import os
import shutil
import signal
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

    def receiver(self, subscribe=True, lease=10):
        r = Receiver(self.next_port)
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
        check(r.wait("/touch/reply", since=m, count=2)[1].args == [1, "refused", "seq used for another command"], "reused seq")
        r.send("/touch/request", 2, "/touch/nope")
        check(r.wait("/touch/reply", lambda x: x.args[0] == 2).args == [2, "refused", "unknown address"], "unknown address")
        r.send("/touch/request", 3, "/touch/request", 4, "/touch/ping")
        check(r.wait("/touch/reply", lambda x: x.args[0] == 3).args[1:] == ["refused", "a request cannot carry another request"], "nested")
        r.send("/touch/nope", 1)
        check(r.wait("/touch/error").args == ["/touch/nope", "unknown address"], "bare refusal")
        r.send("/touch/request", 5, "/touch/device/zz/enabled", 0)
        check(r.wait("/touch/reply", lambda x: x.args[0] == 5).args == [5, "refused", "no device zz"], "unknown device")
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
            "/touch/osc_api": [5],
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
        check("touch-to-osc 0.5.0 (osc_api 5)" in log, "banner")


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
            f.write('[osc]\nlisten = "127.0.0.1:%d"\nallowed_ips = ["127.0.0.1"]\n[paths]\nsettings = ""\n[[device]]\nid = "t"\nusb = "%04x:%04x"\n' % (DAEMON_PORT, TEST_VENDOR, p))
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
            (("/touch/device/t/calibration", 1.0, 2.0, 3.0), "calibration must be six numbers (an affine map, each at most 100), or none"),
            (("/touch/device/t/match_usb", "zz"), 'match_usb must be vendor:product in hex, like "0712:0009" (touch-to-osc --list shows it)'),
            (("/touch/device/t/aspect", 50.0), "aspect must be 0 (taken from the device) or between 0.1 and 10"),
            (("/touch/log_level", "loud"), "log_level must be crit, error, warning, notice, info or debug"),
            (("/touch/device/t/connected", 1), "unknown address"),
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
            f.write('[osc]\nallowed_ips = ["127.0.0.1"]\n[paths]\nsettings = "%s"\n[[device]]\nid = "t"\nusb = "%04x:%04x"\n' % (path, TEST_VENDOR, p))
        chk = subprocess.run([os.path.join(BUILD, "touch-to-osc"), "--check-config=" + cfg], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, universal_newlines=True)
        check(chk.returncode == 2 and "device.t.rotation: must be 0, 90, 180 or 270" in chk.stdout, "--check-config said %d: %s" % (chk.returncode, chk.stdout))
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
        check(d.log_text().count("(osc_api 5), run") == 2, "expected two start banners in the log")


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
def usb_reset_needs_a_screen_on_usb_and_recovery_is_a_saved_setting():
    with Ctx("usbcmd") as c:
        p = test_product()
        c.screen(p)
        c.daemon([device(p)])
        r = c.receiver()
        connected(r)
        got = req(r, "/touch/device/t/usb_reset")
        check(got == ["refused", "t has not been seen on a USB port since touch-to-osc started and no port is set for it, so there is nothing to reset: plug the screen"
                      " in again, or set match_port (port in the config file) to the USB port it is plugged into, such as 3-3"], "got %s" % got)
        check(req(r, "/touch/device/t/usb_reset", 1) == ["refused", "usb_reset takes no arguments"], "arguments")
        m = r.mark()
        check(req(r, "/touch/device/t/usb_recovery", 0) == ["ok", ""], "off")
        r.wait("/touch/device/t/usb_recovery", lambda x: x.args == [0], since=m)
        r.wait("/touch/overrides", lambda x: x.args == ["device/t/usb_recovery"], since=m)
        check(req(r, "/touch/device/t/usb_recovery", 2) == ["refused", "usb_recovery needs 0 or 1"], "bad value")
        r.none("/touch/event", lambda x: x.args[0] == "usb", during=1.5, since=0)


@test()
def a_screen_that_drops_out_is_counted_and_warned_about_for_a_minute():
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
        check(lost.args[2:] == ["warning"], "the lost event %s" % lost.args)
        c.screen(p)
        back = r.wait("/touch/event", lambda x: x.args[0] == "device" and " connected: " in x.args[1], since=m, timeout=4)
        check(back.args[2:] == ["notice"], "the connected event %s" % back.args)
        r.wait("/touch/health", lambda x: x.args == ["warning"], since=m, timeout=3)
        r.wait("/touch/reasons", lambda x: "t was unplugged or dropped off USB 1 time(s) in the last minute: check its USB cable and its joints" in x.args,
               since=m, timeout=3)
        r.wait("/touch/device/t/counts", lambda x: x.args[5] == 1, since=m, timeout=2.5)


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
        check("003/001" in unknown.args[1] and unknown.args[2:] == ["warning"], "event %s" % unknown.args)
        started = r.wait("/touch/event", lambda x: x.args[0] == "usb" and "t: resetting USB bus 3 as asked by 127.0.0.1" in x.args[1], since=m, timeout=3)
        check(started.args[2:] == ["warning"], "event %s" % started.args)
        done = r.wait("/touch/event", lambda x: x.args[0] == "usb" and "t: USB bus 3 reset in " in x.args[1], since=m, timeout=3)
        check(done.args[2:] == ["notice"], "event %s" % done.args)
        for f in ("unbind", "bind"):
            with open(os.path.join(hub, f)) as fh:
                check(fh.read().startswith("3-0:1.0"), "%s was not written" % f)
        os.rmdir(driver)
        r.wait("/touch/device/t/usb_port_state", lambda x: x.args == ["being set up"], since=m, timeout=4)
        m = r.mark()
        check(req(r, "/touch/device/t/usb_reset") == ["ok", ""], "usb_reset with a hub that does not bind")
        let_one_check_through()
        failed = r.wait("/touch/event", lambda x: x.args[0] == "usb" and "could not be bound again (the kernel did not take it)" in x.args[1], since=m, timeout=4)
        check(failed.args[2:] == ["error"], "event %s" % failed.args)
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
