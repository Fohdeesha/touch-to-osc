"""Runs the daemon against virtual touchscreens and a receiver that subscribes like a real one."""

import ctypes
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
        self.problems = []
        self.story = True
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
            try:
                is_bundle, msgs = watch.decode_packet(data, strict=True)
            except Exception as e:  # noqa: BLE001
                self.problems.append("a packet that is not OSC (%s): %r" % (e, data[:96]))
                continue
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


def story_problems(items, prefix="/touch"):
    """What a receiver that missed nothing must never see, by the rules in OSC.md.

    Every contact goes down once, ends once with an up or a cancel, and nothing follows; contact
    ids rise and frame numbers run on without a gap for each device within a run; an active
    list never leaves out a contact that is down at its frame and never names one that ended
    by then; a bye comes with nothing down. Returns the problems found.
    """
    problems = []
    contacts = watch.Contacts()
    ended = {}
    last_down = {}
    last_frame = {}

    def new_run():
        ended.clear()
        last_down.clear()
        last_frame.clear()

    for m in items:
        if not m.address.startswith(prefix + "/"):
            continue
        rest = m.address[len(prefix):]
        if rest == "/alive":
            if contacts.run is not None and m.args[1] != contacts.run:
                new_run()
            contacts.alive(m.args[1])
            continue
        if rest == "/bye":
            left = contacts.bye()
            if left:
                problems.append("a bye with contacts still down: %s" % [(d, c) for d, c, _ in left])
            new_run()
            continue
        touch = watch.touch_parts(rest)
        if touch:
            dev, phase = touch
            frame = watch.touch_frame(phase, m.args)
            if frame is None:
                problems.append("a touch message with arguments %s: %s" % (m.args, m.address))
                continue
            cid = m.args[0]
            before = last_frame.get(dev)
            if before and frame == 1 and before[0] > 1:
                contacts.lost()
                new_run()
                before = None
            if before:
                want = before[0] if m.bundle == before[1] else before[0] + 1
                if frame != want:
                    problems.append("%s: frame %d after frame %d (%s)" % (dev, frame, before[0], "in one bundle" if m.bundle == before[1] else "the next bundle"))
            last_frame[dev] = (frame, m.bundle)
            key = (dev, cid)
            if phase == "down":
                if key in contacts.held or key in ended:
                    problems.append("contact %d on %s went down twice" % (cid, dev))
                if cid <= last_down.get(dev, 0):
                    problems.append("contact id %d on %s is not above %d" % (cid, dev, last_down[dev]))
                last_down[dev] = max(cid, last_down.get(dev, 0))
                contacts.touch(dev, phase, cid, frame)
            elif key in ended:
                problems.append("a %s for contact %d on %s after it had ended" % (phase, cid, dev))
            elif contacts.touch(dev, phase, cid, frame):
                if phase != "move":
                    ended[key] = frame
            else:
                if dev in last_down and cid > last_down[dev]:
                    problems.append("a %s for contact %d on %s, whose down never came" % (phase, cid, dev))
                if phase != "move":
                    ended[key] = frame
            continue
        parts = rest.split("/")
        if len(parts) == 4 and parts[1] == "device" and parts[3] == "active":
            dev, frame, ids = parts[2], m.args[0], set(m.args[1:])
            for _, c, _ in contacts.active(dev, frame, ids):
                problems.append("the active list of %s at frame %d leaves out contact %d, which is down" % (dev, frame, c))
                ended[(dev, c)] = frame
            for c in sorted(ids):
                if (dev, c) in ended and ended[(dev, c)] <= frame:
                    problems.append("the active list of %s at frame %d names contact %d, which ended at frame %d" % (dev, frame, c, ended[(dev, c)]))
    if contacts.held:
        problems.append("contacts that never ended: %s" % sorted(contacts.held))
    return problems


def freeze_thread(pid, name):
    """Stop one thread of a process as a debugger would, while its other threads run on. Returns the function that lets it go."""
    PTRACE_DETACH, PTRACE_SEIZE, PTRACE_INTERRUPT = 17, 0x4206, 0x4207
    tid = None
    for t in os.listdir("/proc/%d/task" % pid):
        with open("/proc/%d/task/%s/comm" % (pid, t)) as f:
            if f.read().strip() == name:
                tid = int(t)
    check(tid is not None, "no thread named %s in process %d" % (name, pid))
    libc = ctypes.CDLL(None, use_errno=True)
    libc.ptrace.restype = ctypes.c_long
    libc.ptrace.argtypes = [ctypes.c_long, ctypes.c_long, ctypes.c_void_p, ctypes.c_void_p]
    check(libc.ptrace(PTRACE_SEIZE, tid, None, None) == 0, "cannot take hold of thread %d: %s" % (tid, os.strerror(ctypes.get_errno())))
    check(libc.ptrace(PTRACE_INTERRUPT, tid, None, None) == 0, "cannot stop thread %d: %s" % (tid, os.strerror(ctypes.get_errno())))
    os.waitpid(tid, 0x40000000)

    def release(wait=0.0):
        # a held thread whose process exits stays behind until the one that held it collects it; until then the process
        # itself cannot be collected by its parent
        libc.ptrace(PTRACE_DETACH, tid, None, None)
        deadline = time.time() + wait
        while True:
            try:
                if os.waitpid(tid, 0x40000000 | os.WNOHANG)[0] == tid:
                    return
            except ChildProcessError:
                return
            if time.time() >= deadline:
                return
            time.sleep(0.02)

    return release


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
    def __init__(self, name, devices, extra="", destinations=(), keep_settings=False, allowed=("127.0.0.1",), env=None, binary=None, ignore=()):
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
        self.rc = None
        self.reports = []
        # a suite started in the background or under nohup hands down ignored signals, which the daemon would rightly leave ignored
        def default_signals():
            for sig in (signal.SIGHUP, signal.SIGINT, signal.SIGQUIT):
                signal.signal(sig, signal.SIG_IGN if sig in ignore else signal.SIG_DFL)

        self.proc = subprocess.Popen([binary or os.path.join(BUILD, "touch-to-osc"), "--config=" + self.config], stdout=self.log, stderr=subprocess.STDOUT,
                                     env=dict(os.environ, **env) if env else None, preexec_fn=default_signals)
        deadline = time.time() + 15
        while "OSC on UDP" not in self.log_text():
            if self.proc.poll() is not None:
                raise Failure("the daemon exited at start (%d): %s" % (self.proc.returncode, self.log_text()[-2000:]))
            if time.time() > deadline:
                self.proc.kill()
                self.proc.wait()
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
        if not self.log.closed:
            self.log.close()
            self.rc = self.proc.returncode
            self.reports = sanitizer_marks(self.log_text())
        return self.proc.returncode

    def sanitizer_reports(self):
        return self.reports


def sanitizer_marks(text):
    return [m for m in SANITIZER_MARKS if m in text]


def run_tool(args, **kw):
    """Run the daemon's binary as a command and fail on a sanitizer report in what it prints."""
    kw.setdefault("timeout", 20)
    out = subprocess.run([os.path.join(BUILD, "touch-to-osc")] + list(args), stdout=subprocess.PIPE, stderr=subprocess.STDOUT, universal_newlines=True, **kw)
    marks = sanitizer_marks(out.stdout)
    check(not marks, "sanitizer report from touch-to-osc %s: %s" % (" ".join(args), out.stdout[-3000:]))
    return out


def test_product():
    return random.randint(0x1000, 0xFFFE)


def device(product, **kw):
    d = {"id": "t", "usb": "%04x:%04x" % (TEST_VENDOR, product), "silence_ms": 0}
    d.update(kw)
    return d
