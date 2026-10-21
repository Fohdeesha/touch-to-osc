"""Virtual touchscreens through /dev/uinput, for tests only."""

import fcntl
import os
import struct
import time

EV_SYN, EV_KEY, EV_ABS, EV_MSC = 0, 1, 3, 4
SYN_REPORT = 0
MSC_TIMESTAMP = 5
BTN_TOUCH = 0x14A
ABS_X, ABS_Y = 0x00, 0x01
ABS_MT_SLOT = 0x2F
ABS_MT_POSITION_X, ABS_MT_POSITION_Y = 0x35, 0x36
ABS_MT_TRACKING_ID = 0x39
INPUT_PROP_DIRECT = 1
BUS_VIRTUAL = 0x06


def _ioc(direction, nr, size):
    return (direction << 30) | (size << 16) | (ord("U") << 8) | nr


UI_DEV_CREATE = _ioc(0, 1, 0)
UI_DEV_DESTROY = _ioc(0, 2, 0)
UI_DEV_SETUP = _ioc(1, 3, 92)
UI_ABS_SETUP = _ioc(1, 4, 28)
UI_SET_EVBIT = _ioc(1, 100, 4)
UI_SET_KEYBIT = _ioc(1, 101, 4)
UI_SET_ABSBIT = _ioc(1, 103, 4)
UI_SET_MSCBIT = _ioc(1, 104, 4)
UI_SET_PHYS = _ioc(1, 108, 8)
UI_SET_PROPBIT = _ioc(1, 110, 4)
EVIOCGRAB = (1 << 30) | (4 << 16) | (ord("E") << 8) | 0x90


def ui_get_sysname(length):
    return _ioc(2, 44, length)


class TouchScreen:
    def __init__(self, name, vendor, product, slots=10, xmax=1920, ymax=1080, resolution=4, direct=True, phys=None, multitouch=True, btn_touch=True):
        self.fd = os.open("/dev/uinput", os.O_WRONLY | os.O_NONBLOCK)
        self.slots = slots
        self.multitouch = multitouch
        self.btn_touch = btn_touch
        self.active = {}
        self.stamp = 0
        for ev in (EV_SYN, EV_KEY, EV_ABS, EV_MSC):
            fcntl.ioctl(self.fd, UI_SET_EVBIT, ev)
        fcntl.ioctl(self.fd, UI_SET_KEYBIT, BTN_TOUCH)
        fcntl.ioctl(self.fd, UI_SET_MSCBIT, MSC_TIMESTAMP)
        axes = [(ABS_X, 0, xmax, resolution), (ABS_Y, 0, ymax, resolution)]
        if multitouch:
            axes += [
                (ABS_MT_SLOT, 0, slots - 1, 0),
                (ABS_MT_POSITION_X, 0, xmax, resolution),
                (ABS_MT_POSITION_Y, 0, ymax, resolution),
                (ABS_MT_TRACKING_ID, 0, 65535, 0),
            ]
        for code, lo, hi, res in axes:
            fcntl.ioctl(self.fd, UI_SET_ABSBIT, code)
            fcntl.ioctl(self.fd, UI_ABS_SETUP, struct.pack("H2xiiiiii", code, 0, lo, hi, 0, 0, res))
        if direct:
            fcntl.ioctl(self.fd, UI_SET_PROPBIT, INPUT_PROP_DIRECT)
        if phys:
            fcntl.ioctl(self.fd, UI_SET_PHYS, phys.encode() + b"\0")
        fcntl.ioctl(self.fd, UI_DEV_SETUP, struct.pack("HHHH80sI", BUS_VIRTUAL, vendor, product, 1, name.encode()[:79], 0))
        fcntl.ioctl(self.fd, UI_DEV_CREATE)
        buf = bytearray(64)
        fcntl.ioctl(self.fd, ui_get_sysname(len(buf)), buf)
        self.sysname = bytes(buf).split(b"\0")[0].decode()
        self.node = self._find_node()

    def _find_node(self):
        base = "/sys/devices/virtual/input/" + self.sysname
        deadline = time.time() + 5
        while time.time() < deadline:
            for e in os.listdir(base):
                if e.startswith("event") and os.path.exists("/dev/input/" + e):
                    return "/dev/input/" + e
            time.sleep(0.01)
        raise RuntimeError("the virtual touchscreen's event node did not appear")

    def emit(self, typ, code, value):
        os.write(self.fd, struct.pack("qqHHi", 0, 0, typ, code, value))

    def frame(self):
        if self.btn_touch:
            if self.active:
                first = min(self.active)
                self.emit(EV_ABS, ABS_X, self.active[first][0])
                self.emit(EV_ABS, ABS_Y, self.active[first][1])
            self.emit(EV_KEY, BTN_TOUCH, 1 if self.active else 0)
        self.tick()

    def tick(self):
        """A frame with nothing changed, as the real screen sends every 16 ms while a finger is down."""
        self.stamp += 16000
        self.emit(EV_MSC, MSC_TIMESTAMP, self.stamp & 0x7FFFFFFF)
        self.emit(EV_SYN, SYN_REPORT, 0)

    def touch(self, slot, tracking, x, y):
        if self.multitouch:
            self.emit(EV_ABS, ABS_MT_SLOT, slot)
            self.emit(EV_ABS, ABS_MT_TRACKING_ID, tracking)
            self.emit(EV_ABS, ABS_MT_POSITION_X, x)
            self.emit(EV_ABS, ABS_MT_POSITION_Y, y)
        self.active[slot] = (x, y)

    def lift(self, slot):
        if self.multitouch:
            self.emit(EV_ABS, ABS_MT_SLOT, slot)
            self.emit(EV_ABS, ABS_MT_TRACKING_ID, -1)
        self.active.pop(slot, None)

    def destroy(self):
        if self.fd is None:
            return
        try:
            fcntl.ioctl(self.fd, UI_DEV_DESTROY)
        finally:
            os.close(self.fd)
            self.fd = None

    def replay(self, path, speed=1.0):
        """Replay the E: lines of an evemu recording with their timing."""
        start = time.monotonic()
        with open(path) as f:
            for line in f:
                if not line.startswith("E:"):
                    continue
                parts = line.split()
                t = float(parts[1]) / speed
                typ, code, value = int(parts[2], 16), int(parts[3], 16), int(parts[4])
                delay = start + t - time.monotonic()
                if delay > 0:
                    time.sleep(delay)
                self.emit(typ, code, value)


def try_grab(node):
    fd = os.open(node, os.O_RDONLY | os.O_NONBLOCK)
    try:
        fcntl.ioctl(fd, EVIOCGRAB, 1)
        fcntl.ioctl(fd, EVIOCGRAB, 0)
        return True
    except OSError:
        return False
    finally:
        os.close(fd)
