"""Runs the daemon against virtual touchscreens and a receiver that subscribes like the engine."""

import os
import random
import signal
import socket
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import watch  # noqa: E402

BUILD = os.environ.get("TTO_BUILD", os.path.join(ROOT, "build"))
RUN_DIR = os.environ.get("TTO_E2E_DIR", "/root/touch-to-osc-run/e2e")
DAEMON_PORT = 19480
TEST_VENDOR = 0x7470
SANITIZER_MARKS = ("ERROR: AddressSanitizer", "runtime error:", "LeakSanitizer", "WARNING: ThreadSanitizer")


class Failure(Exception):
    pass


def check(cond, what):
    if not cond:
        raise Failure(what)


class Msg:
    def __init__(self, at, bundle, address, args):
        self.at = at
        self.bundle = bundle
        self.address = address
        self.args = args

    def __repr__(self):
        return "%s %s" % (self.address, self.args)


class Receiver:
    def __init__(self, port, prefix="/touch", target=("127.0.0.1", DAEMON_PORT), host="127.0.0.1"):
        self.port = port
        self.prefix = prefix
        self.target = target
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind((host, port))
        self.sock.settimeout(0.1)
        self.items = []
        self.lock = threading.Lock()
        self.bundles = 0
        self.discard = None
        self.closed = False
        self.renewing = False
        self.thread = threading.Thread(target=self._loop, daemon=True)
        self.thread.start()

    def _loop(self):
        while not self.closed:
            try:
                data, _ = self.sock.recvfrom(65536)
            except socket.timeout:
                continue
            except OSError:
                return
            now = time.time()
            is_bundle, msgs = watch.decode_packet(data)
            with self.lock:
                self.bundles += 1
                for address, args in msgs:
                    if self.discard and self.discard(address, args):
                        continue
                    self.items.append(Msg(now, self.bundles, address, args))

    def send(self, address, *args):
        self.sock.sendto(watch.encode(address, *args), self.target)

    def subscribe(self, seconds=10):
        self.send(self.prefix + "/subscribe", self.port, seconds)

    def keep_subscribed(self, seconds=10):
        self.renewing = True

        def renew():
            while self.renewing and not self.closed:
                self.subscribe(seconds)
                time.sleep(3)

        threading.Thread(target=renew, daemon=True).start()

    def mark(self):
        with self.lock:
            return len(self.items)

    def all(self, since=0, address=None, pred=None):
        with self.lock:
            items = list(self.items[since:])
        return [m for m in items if (address is None or m.address == address) and (pred is None or pred(m))]

    def wait(self, address, pred=None, timeout=2.0, since=0, count=1):
        deadline = time.time() + timeout
        while True:
            found = self.all(since, address, pred)
            if len(found) >= count:
                return found[count - 1] if count == 1 else found[:count]
            if time.time() > deadline:
                raise Failure("waited %.1f s for %d x %s, got %s" % (timeout, count, address, found))
            time.sleep(0.005)

    def none(self, address, pred=None, during=0.5, since=None):
        start = self.mark() if since is None else since
        time.sleep(during)
        found = self.all(start, address, pred)
        check(not found, "expected no %s, got %s" % (address, found))

    def close(self):
        self.renewing = False
        self.closed = True
        self.thread.join(2)
        self.sock.close()


def device_block(d):
    lines = ["[[device]]"]
    for k, v in d.items():
        if isinstance(v, bool):
            lines.append("%s = %s" % (k, "true" if v else "false"))
        elif isinstance(v, (int, float)):
            lines.append("%s = %s" % (k, v))
        elif isinstance(v, list):
            lines.append("%s = [%s]" % (k, ", ".join(str(x) for x in v)))
        else:
            lines.append('%s = "%s"' % (k, v))
    return "\n".join(lines)


class Daemon:
    def __init__(self, name, devices, extra="", destinations=(), keep_settings=False, allowed=("127.0.0.1",), env=None, binary=None):
        os.makedirs(RUN_DIR, exist_ok=True)
        self.name = name
        self.config = os.path.join(RUN_DIR, name + ".toml")
        self.log_path = os.path.join(RUN_DIR, name + ".log")
        self.settings = os.path.join(RUN_DIR, name + "-settings.toml")
        self.state = os.path.join(RUN_DIR, name + "-state.toml")
        if not keep_settings:
            for f in (self.settings, self.settings + ".refused", self.state, self.state + ".usb-reset"):
                if os.path.exists(f):
                    os.remove(f)
        text = '[osc]\nlisten = "127.0.0.1:%d"\nallowed_ips = [%s]\n%s\n[paths]\nsettings = "%s"\nstate = "%s"\n' % (
            DAEMON_PORT, ", ".join('"%s"' % a for a in allowed), extra, self.settings, self.state)
        for dest in destinations:
            text += '[[destination]]\naddress = "%s"\n' % dest
        text += "\n".join(device_block(d) for d in devices) + "\n"
        with open(self.config, "w") as f:
            f.write(text)
        self.log = open(self.log_path, "w")
        self.proc = subprocess.Popen([binary or os.path.join(BUILD, "touch-to-osc"), "--config=" + self.config], stdout=self.log, stderr=subprocess.STDOUT,
                                     env=dict(os.environ, **env) if env else None)
        deadline = time.time() + 15
        while "OSC on UDP" not in self.log_text():
            if self.proc.poll() is not None:
                raise Failure("the daemon exited at start (%d): %s" % (self.proc.returncode, self.log_text()[-2000:]))
            if time.time() > deadline:
                raise Failure("the daemon did not start: " + self.log_text()[-2000:])
            time.sleep(0.02)

    def log_text(self):
        with open(self.log_path) as f:
            return f.read()

    def pause(self):
        self.proc.send_signal(signal.SIGSTOP)

    def resume(self):
        self.proc.send_signal(signal.SIGCONT)

    def stop(self, sig=signal.SIGTERM, timeout=15):
        if self.proc.poll() is None:
            self.proc.send_signal(signal.SIGCONT)
            self.proc.send_signal(sig)
            try:
                self.proc.wait(timeout)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()
                raise Failure("the daemon did not stop within %d s" % timeout)
        self.log.close()
        return self.proc.returncode

    def sanitizer_reports(self):
        text = self.log_text()
        return [m for m in SANITIZER_MARKS if m in text]


def test_product():
    return random.randint(0x1000, 0xFFFE)


def device(product, **kw):
    d = {"id": "t", "usb": "%04x:%04x" % (TEST_VENDOR, product), "silence_ms": 0}
    d.update(kw)
    return d
