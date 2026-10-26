#!/usr/bin/env python3
"""USB recovery on a real touchscreen.

usage: sudo python3 tests/hw/usb_recovery.py [--usb 0712:0009] [--binary build/touch-to-osc]
       sudo python3 tests/hw/usb_recovery.py --service face [--listen 127.0.0.1:9480]

Needs a real USB touchscreen plugged in and root. By default it stops the touch-to-osc
service for the duration (and starts it again) and runs its own daemon on 127.0.0.1:19480;
with --service it tests that device of the running service instead (127.0.0.1 must be in
its allowed_ips). It makes the kernel drop the screen with /sys/bus/usb/devices/<port>/remove:
the screen stays plugged in but the kernel no longer drives it and does not try again,
which is what a failed enumeration leaves behind. Every device on the screen's USB bus
drops several times. It checks:
  - a reset asked for over OSC brings the screen back;
  - with recovery on, a dropped screen is noticed, reset after about 10 s and comes back;
  - with recovery off, nothing is reset; turning it on recovers the screen.
At the end the screen's USB hub is bound, the screen enumerated and recovery on, whatever
happened.
"""

import argparse
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "e2e"))
from harness import Daemon, Failure, Receiver, check  # noqa: E402

DEV = "/sys/bus/usb/devices"
HUB_DRIVER = "/sys/bus/usb/drivers/hub"
RESULTS = []
MEASURED = []


def find_port(usb):
    vendor, product = usb.split(":")
    for name in os.listdir(DEV):
        try:
            with open(os.path.join(DEV, name, "idVendor")) as f:
                v = f.read().strip()
            with open(os.path.join(DEV, name, "idProduct")) as f:
                p = f.read().strip()
        except OSError:
            continue
        if v == vendor and p == product and ":" not in name and not name.startswith("usb"):
            return name
    return None


def hub_interface(port):
    bus, chain = port.split("-", 1)
    parts = chain.split(".")
    return "%s-0:1.0" % bus if len(parts) == 1 else "%s-%s:1.0" % (bus, ".".join(parts[:-1]))


def drop(port):
    with open(os.path.join(DEV, port, "remove"), "w") as f:
        f.write("1")


def restore(port, iface):
    if not os.path.exists(os.path.join(DEV, iface, "driver")):
        with open(os.path.join(HUB_DRIVER, "bind"), "w") as f:
            f.write(iface)
    if not os.path.exists(os.path.join(DEV, port)):
        with open(os.path.join(HUB_DRIVER, "unbind"), "w") as f:
            f.write(iface)
        with open(os.path.join(HUB_DRIVER, "bind"), "w") as f:
            f.write(iface)
    deadline = time.time() + 10
    while not os.path.exists(os.path.join(DEV, port)) and time.time() < deadline:
        time.sleep(0.05)
    return os.path.exists(os.path.join(DEV, port))


_seq = [5000]


def req(r, address, *args):
    _seq[0] += 1
    s = _seq[0]
    m = r.mark()
    r.send("/touch/request", s, address, *args)
    return r.wait("/touch/reply", lambda x: x.args[0] == s, since=m).args[1:]


def step(name, fn):
    started = time.time()
    try:
        fn()
        RESULTS.append("ok    %s (%.1f s)" % (name, time.time() - started))
    except Exception as e:  # noqa: BLE001
        RESULTS.append("FAIL  %s (%.1f s): %s" % (name, time.time() - started, e if isinstance(e, Failure) else repr(e)))
    print(RESULTS[-1], flush=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--usb", default="0712:0009")
    ap.add_argument("--binary", default=None, help="the touch-to-osc to test (default: TTO_BUILD or build/)")
    ap.add_argument("--service", metavar="ID", help="test this device of the running service instead of a daemon of its own")
    ap.add_argument("--listen", default="127.0.0.1:9480", help="with --service: where the service listens")
    a = ap.parse_args()
    if os.geteuid() != 0:
        print("run as root")
        return 2
    if a.binary:
        os.environ["TTO_BUILD"] = os.path.dirname(os.path.abspath(a.binary))
    port = find_port(a.usb)
    if not port:
        print("no USB device %s is set up: plug the touchscreen in" % a.usb)
        return 2
    iface = hub_interface(port)
    print("touchscreen %s on USB port %s, its hub interface %s" % (a.usb, port, iface), flush=True)
    active = subprocess.run(["systemctl", "is-active", "--quiet", "touch-to-osc"]).returncode == 0
    if a.service and not active:
        print("touch-to-osc.service is not running")
        return 2
    stopped = False
    if active and not a.service:
        subprocess.run(["systemctl", "stop", "touch-to-osc"], check=True)
        stopped = True
        print("stopped touch-to-osc.service for the test", flush=True)
    dev = a.service or "hw"
    base = "/touch/device/%s/" % dev
    d = None
    r = None
    recovery_off_left = [False]
    try:
        if a.service:
            host, _, listen_port = a.listen.rpartition(":")
            r = Receiver(19494, target=(host, int(listen_port)))
        else:
            d = Daemon("hw-usb", [{"id": dev, "usb": a.usb, "silence_ms": 0}])
            r = Receiver(19494)
        r.keep_subscribed()
        resets = [0]

        def connected(value, since=0, timeout=10):
            return r.wait(base + "connected", lambda x: x.args == [value], since=since, timeout=timeout)

        def screen_up():
            if not os.path.exists(os.path.join(DEV, port)):
                m = r.mark()
                check(restore(port, iface), "the screen did not come back after its hub was bound again")
                connected(1, since=m)

        def starts_connected():
            connected(1)
            r.wait(base + "usb_watched_port", lambda x: x.args == [port])
            check(r.wait(base + "usb_recovery").args == [1], "usb_recovery is off for %s: turn it on to test it" % dev)
            resets[0] = r.wait(base + "usb_resets").args[0]

        step("the daemon reads the screen and watches its port", starts_connected)

        def manual():
            screen_up()
            m = r.mark()
            check(req(r, base + "usb_reset") == ["ok", ""], "usb_reset refused")
            r.wait("/touch/event", lambda x: x.args[0] == "usb" and "as asked by 127.0.0.1" in x.args[1], since=m)
            done = r.wait("/touch/event", lambda x: x.args[0] == "usb" and "); waiting for the touchscreen" in x.args[1], since=m, timeout=5)
            check("(nothing else was on it)" in done.args[1] or "(it also dropped " in done.args[1], "what the reset dropped: %s" % done.args[1])
            connected(0, since=m)
            connected(1, since=m, timeout=10)
            r.wait(base + "usb_resets", lambda x: x.args == [resets[0] + 1], since=m)

        step("a reset asked for over OSC brings the screen back", manual)

        def automatic():
            screen_up()
            m = r.mark()
            t0 = time.time()
            drop(port)
            lost = connected(0, since=m)
            r.wait(base + "usb_port_state", lambda x: x.args == ["plugged in but not set up"], since=m, timeout=4)
            reason = r.wait(base + "reason", lambda x: "the kernel has not set it up" in x.args[0], since=m, timeout=4).args[0]
            check("USB port %s" % port in reason, "reason %r" % reason)
            r.wait("/touch/health", lambda x: x.args == ["fault"], since=m)
            reset = r.wait("/touch/event", lambda x: x.args[0] == "usb" and "(attempt 1)" in x.args[1], since=m, timeout=15)
            back = connected(1, since=m, timeout=10)
            r.wait("/touch/event", lambda x: x.args[0] == "usb" and "is back after 1 USB reset" in x.args[1], since=m, timeout=3)
            r.wait(base + "usb_port_state", lambda x: x.args == [""], since=m, timeout=3)
            r.wait("/touch/health", lambda x: x.args == ["ok"] and x.at >= back.at, since=m, timeout=3)
            later = r.mark()
            time.sleep(2.5)
            check(not r.all(later, "/touch/health", lambda x: x.args != ["ok"]), "the health went back from ok after the screen was back")
            MEASURED.append("dropped screen: lost after %.2f s, reset at %.1f s, back at %.1f s" % (lost.at - t0, reset.at - t0, back.at - t0))
            check(9 <= reset.at - t0 <= 13, "the reset came %.1f s after the drop, not after about 10 s" % (reset.at - t0))

        step("recovery on: a dropped screen is reset after about 10 s and comes back", automatic)

        def recovery_off():
            screen_up()
            check(req(r, base + "usb_recovery", 0) == ["ok", ""], "usb_recovery 0 refused")
            recovery_off_left[0] = True
            m = r.mark()
            drop(port)
            connected(0, since=m)
            time.sleep(14)
            check(not r.all(m, "/touch/event", lambda x: x.args[0] == "usb"), "a USB event with recovery off: %s" % r.all(m, "/touch/event", lambda x: x.args[0] == "usb"))
            check(not os.path.exists(os.path.join(DEV, port)), "the screen came back by itself")
            m2 = r.mark()
            check(req(r, base + "usb_recovery", 1) == ["ok", ""], "usb_recovery 1 refused")
            recovery_off_left[0] = False
            r.wait("/touch/event", lambda x: x.args[0] == "usb" and "(attempt 1)" in x.args[1], since=m2, timeout=15)
            connected(1, since=m2, timeout=10)

        step("recovery off: nothing is reset; turned on, the screen comes back", recovery_off)
    finally:
        ok = restore(port, iface)
        if r and recovery_off_left[0]:
            try:
                req(r, base + "usb_recovery", 1)
            except Failure as e:
                RESULTS.append("FAIL  usb_recovery could not be turned back on: %s" % e)
        if r:
            r.close()
        if d:
            rc = d.stop()
            if rc != 0:
                RESULTS.append("FAIL  the test daemon exited with %s" % rc)
            reports = d.sanitizer_reports()
            if reports:
                RESULTS.append("FAIL  sanitizer reports: %s" % reports)
        if stopped:
            subprocess.run(["systemctl", "start", "touch-to-osc"], check=False)
            print("started touch-to-osc.service again", flush=True)
        print("afterwards: hub %s bound, screen on %s set up: %s" % (iface, port, ok), flush=True)
    for m in MEASURED:
        print("measured: " + m)
    failed = [x for x in RESULTS if x.startswith("FAIL")]
    print("%d of %d passed" % (len(RESULTS) - len(failed), len(RESULTS)))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
