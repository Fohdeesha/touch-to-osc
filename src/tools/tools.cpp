#include "tools/tools.h"

#include "control/usb.h"
#include "core/process.h"
#include "input/mapping.h"
#include "input/reader.h"
#include "input/tracker.h"

#include <libevdev/libevdev.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <memory>
#include <poll.h>
#include <unistd.h>

namespace tto {

namespace {

struct Opened {
    int fd = -1;
    libevdev* dev = nullptr;
    int error = 0;
    ~Opened() {
        if (dev) libevdev_free(dev);
        if (fd >= 0) close(fd);
    }
};

void open_node(const std::string& node, Opened& o) {
    o.fd = open(node.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (o.fd < 0) {
        o.error = errno;
        return;
    }
    int rc = libevdev_new_from_fd(o.fd, &o.dev);
    if (rc < 0) {
        o.dev = nullptr;
        o.error = -rc;
    }
}

Axis axis(libevdev* dev, unsigned code) {
    Axis a;
    if (const input_absinfo* i = libevdev_get_abs_info(dev, code)) {
        a.min = i->minimum;
        a.max = i->maximum;
        a.resolution = i->resolution;
    }
    return a;
}

void print_touch_details(const NodeInfo& n, TouchKind kind) {
    Opened o;
    open_node(n.node, o);
    if (!o.dev) {
        std::printf("    touch details unreadable: %s%s\n", errno_str(o.error).c_str(),
                    o.error == EACCES || o.error == EPERM ? " (run as root, or as a member of the input group)" : "");
        return;
    }
    bool mt = kind == TouchKind::multitouch;
    Axis x = axis(o.dev, mt ? ABS_MT_POSITION_X : ABS_X), y = axis(o.dev, mt ? ABS_MT_POSITION_Y : ABS_Y);
    int slots = mt ? libevdev_get_num_slots(o.dev) : 1;
    std::printf("    touchscreen, %s: %d contact%s at once, x %d..%d, y %d..%d", mt ? "multitouch" : "single touch", slots, slots == 1 ? "" : "s", x.min, x.max, y.min, y.max);
    if (x.resolution > 0 && y.resolution > 0)
        std::printf(", %d units/mm (%.0f x %.0f mm)", x.resolution, (double(x.max) - double(x.min)) / x.resolution, (double(y.max) - double(y.min)) / y.resolution);
    std::printf("\n");
    int g = libevdev_grab(o.dev, LIBEVDEV_GRAB);
    if (g == 0) libevdev_grab(o.dev, LIBEVDEV_UNGRAB);
    else std::printf("    held exclusively by another program (the touch-to-osc service?)\n");
}

std::vector<const NodeInfo*> assigned(const Config& cfg, const std::vector<NodeInfo>& nodes) {
    NodeClaims claims;
    claims.set_devices(cfg.devices);
    std::vector<const NodeInfo*> out(cfg.devices.size(), nullptr);
    for (size_t i = 0; i < out.size(); ++i) {
        out[i] = claims.pick(i, nodes);
        if (out[i]) claims.claim(out[i]->sysname);
    }
    return out;
}

void print_node_head(const NodeInfo& n) {
    std::printf("%s  \"%s\"\n", n.node.c_str(), printable(n.name).c_str());
    if (!n.usb_port.empty())
        std::printf("    usb %s, serial \"%s\", port %s\n", usb_text(n.vendor, n.product).c_str(), printable(n.usb_serial).c_str(), n.usb_port.c_str());
    else
        std::printf("    id %s (not USB%s%s)\n", usb_text(n.vendor, n.product).c_str(), n.phys.empty() ? "" : ", ", printable(n.phys).c_str());
}

std::string spare_id(const Config* cfg) {
    for (int k = cfg ? int(cfg->devices.size()) + 1 : 1;; ++k) {
        std::string id = "screen" + std::to_string(k);
        if (!cfg || std::none_of(cfg->devices.begin(), cfg->devices.end(), [&](const DeviceConfig& d) { return d.id == id; })) return id;
    }
}

}

int list_devices(const Config* cfg, const SysPaths& paths, bool all) {
    auto nodes = scan_nodes(paths);
    if (nodes.empty()) {
        std::printf("no input devices found under %s\n", paths.sys_class_input.c_str());
        return 0;
    }
    std::vector<const NodeInfo*> reads = cfg ? assigned(*cfg, nodes) : std::vector<const NodeInfo*>();
    std::string usb_root = getenv("TTO_USB_ROOT") ? getenv("TTO_USB_ROOT") : "";
    UsbPaths usb = usb_root.empty() ? UsbPaths() : usb_paths_under(usb_root);
    int screens = 0, others = 0;
    for (const auto& n : nodes) {
        TouchKind kind = touch_kind(n);
        if (kind == TouchKind::none) {
            ++others;
            continue;
        }
        if (screens++ == 0) std::printf("touchscreens:\n\n");
        print_node_head(n);
        print_touch_details(n, kind);
        std::string reader;
        for (size_t i = 0; i < reads.size(); ++i) {
            if (reads[i] == &n) reader = cfg->devices[i].id;
        }
        if (!reader.empty()) {
            std::printf("    read as device \"%s\" in the config\n", reader.c_str());
        } else {
            std::printf("    %s; to read it, add this to the config:\n", cfg ? "not read by the config" : "no config read");
            std::printf("        [[device]]\n        id = \"%s\"\n        match_usb = \"%s\"\n", spare_id(cfg).c_str(), usb_text(n.vendor, n.product).c_str());
            std::string at = n.usb_port.empty() ? printable(n.phys) : n.usb_port;
            if (!at.empty()) std::printf("        match_port = \"%s\"\n", at.c_str());
        }
        if (auto ref = parse_usb_port(n.usb_port)) std::printf("    a USB reset for it resets %s\n", usb_reset_reach(*ref, usb).c_str());
        std::printf("\n");
    }
    if (screens == 0) std::printf("no touchscreens found\n\n");
    for (const auto& n : nodes) {
        std::string why;
        if (touch_kind(n, &why) != TouchKind::none) continue;
        std::vector<std::string> claimed_by;
        for (const auto& d : cfg ? cfg->devices : std::vector<DeviceConfig>()) {
            if (matches(d, n)) claimed_by.push_back(d.id);
        }
        if (!all && claimed_by.empty()) continue;
        print_node_head(n);
        std::printf("    not a touchscreen: %s\n", why.c_str());
        for (const auto& id : claimed_by) std::printf("    matches device \"%s\" in the config, which will not use it: it is not a touchscreen\n", id.c_str());
        std::printf("\n");
    }
    if (!all && others > 0) std::printf("%d other input device%s (keyboards, buttons and the like): touch-to-osc --list --all shows them\n", others, others == 1 ? "" : "s");
    if (cfg) {
        for (size_t i = 0; i < cfg->devices.size(); ++i) {
            const DeviceConfig& d = cfg->devices[i];
            if (reads[i]) continue;
            bool any = std::any_of(nodes.begin(), nodes.end(), [&](const NodeInfo& n) { return matches(d, n) && touch_kind(n) != TouchKind::none; });
            if (any) std::printf("device \"%s\" in the config (%s): every touchscreen it matches is read by another device; set match_port to tell them apart\n", d.id.c_str(), d.match_text().c_str());
            else std::printf("device \"%s\" in the config (%s): no touchscreen here matches it\n", d.id.c_str(), d.match_text().c_str());
        }
    }
    return 0;
}

int touch_to_find(const Config* cfg, const SysPaths& paths, int seconds) {
    auto nodes = scan_nodes(paths);
    std::vector<const NodeInfo*> reads = cfg ? assigned(*cfg, nodes) : std::vector<const NodeInfo*>();
    struct Watched {
        const NodeInfo* node;
        std::unique_ptr<Opened> o;
    };
    std::vector<Watched> watched;
    for (const auto& n : nodes) {
        if (touch_kind(n) == TouchKind::none || std::find(reads.begin(), reads.end(), &n) != reads.end()) continue;
        auto o = std::make_unique<Opened>();
        open_node(n.node, *o);
        if (!o->dev) {
            std::printf("%s \"%s\" cannot be opened: %s%s\n", n.node.c_str(), printable(n.name).c_str(), errno_str(o->error).c_str(),
                        o->error == EACCES || o->error == EPERM ? " (run it with sudo)" : "");
            continue;
        }
        watched.push_back({&n, std::move(o)});
    }
    if (watched.empty()) {
        std::printf("no touchscreen here is left unread by the config; for the screens it reads: touch-to-osc --send identify 30, then touch-to-osc-watch\n");
        return 0;
    }
    std::printf("touch one of the %zu touchscreen%s that no device reads (waiting %d s; the screens the config reads are left alone)\n", watched.size(),
                watched.size() == 1 ? "" : "s", seconds);
    std::fflush(stdout);
    Nanos until = monotonic_ns() + Nanos(seconds) * kSec;
    for (;;) {
        Nanos left = until - monotonic_ns();
        if (left <= 0) {
            std::printf("nothing was touched in %d s\n", seconds);
            return 1;
        }
        std::vector<pollfd> p;
        for (const auto& w : watched) p.push_back({w.o->fd, POLLIN, 0});
        int r = poll(p.data(), nfds_t(p.size()), int(left / kMs) + 1);
        if (r < 0 && errno != EINTR) return 1;
        for (size_t i = 0; i < watched.size(); ++i) {
            if (!(p[i].revents & POLLIN)) continue;
            libevdev* dev = watched[i].o->dev;
            bool touched = false;
            for (;;) {
                input_event ev{};
                int rc = libevdev_next_event(dev, LIBEVDEV_READ_FLAG_NORMAL, &ev);
                if (rc == LIBEVDEV_READ_STATUS_SYNC) {
                    while (rc == LIBEVDEV_READ_STATUS_SYNC) rc = libevdev_next_event(dev, LIBEVDEV_READ_FLAG_SYNC, &ev);
                }
                if (rc < 0) break;
                if (ev.type == EV_KEY && ev.code == BTN_TOUCH && ev.value == 1) touched = true;
                if (ev.type == EV_ABS && ev.code == ABS_MT_TRACKING_ID && ev.value >= 0) touched = true;
            }
            if (!touched) continue;
            const NodeInfo& n = *watched[i].node;
            std::string at = n.usb_port.empty() ? printable(n.phys) : n.usb_port;
            std::printf("touched: %s \"%s\", usb %s, port %s; to read it, add this to the config:\n", n.node.c_str(), printable(n.name).c_str(),
                        usb_text(n.vendor, n.product).c_str(), at.empty() ? "?" : at.c_str());
            std::printf("    [[device]]\n    id = \"%s\"\n    match_usb = \"%s\"\n", spare_id(cfg).c_str(), usb_text(n.vendor, n.product).c_str());
            if (!at.empty()) std::printf("    match_port = \"%s\"\n", at.c_str());
            return 0;
        }
    }
}

int dump_device(const Config& cfg, const std::string& id, const SysPaths& paths) {
    const DeviceConfig* d = nullptr;
    for (const auto& c : cfg.devices) {
        if (id.empty() || c.id == id) {
            d = &c;
            break;
        }
    }
    if (!d) {
        std::string ids;
        for (const auto& c : cfg.devices) ids += (ids.empty() ? "" : ", ") + c.id;
        std::fprintf(stderr, "no device \"%s\" in the config (its devices: %s)\n", id.c_str(), ids.c_str());
        return 2;
    }
    auto nodes = scan_nodes(paths);
    const NodeInfo* n = assigned(cfg, nodes)[size_t(d - cfg.devices.data())];
    if (!n) {
        std::fprintf(stderr, "device %s (%s): no touchscreen matches; plug it in, or compare with touch-to-osc --list\n", d->id.c_str(), d->match_text().c_str());
        return 1;
    }
    Opened o;
    open_node(n->node, o);
    if (!o.dev) {
        std::fprintf(stderr, "cannot open %s: %s%s\n", n->node.c_str(), errno_str(o.error).c_str(),
                     o.error == EACCES || o.error == EPERM ? " (run as root, or as a member of the input group)" : "");
        return 1;
    }
    if (libevdev_grab(o.dev, LIBEVDEV_GRAB) != 0) {
        std::fprintf(stderr, "another program holds %s exclusively, so nothing would show here. If it is the service: systemctl stop touch-to-osc, dump, then systemctl start touch-to-osc\n",
                     n->node.c_str());
        return 1;
    }
    libevdev_grab(o.dev, LIBEVDEV_UNGRAB);
    libevdev_set_clock_id(o.dev, CLOCK_MONOTONIC);
    TouchKind kind = touch_kind(*n);
    bool mt = kind == TouchKind::multitouch;
    int slots = mt ? std::clamp(libevdev_get_num_slots(o.dev), 1, kMaxSlots) : 1;
    Mapping map(*d, axis(o.dev, mt ? ABS_MT_POSITION_X : ABS_X), axis(o.dev, mt ? ABS_MT_POSITION_Y : ABS_Y));
    std::printf("dumping %s (%s) on %s as device %s: rotation %d, mirror \"%s\"%s. Ctrl+C stops.\n", n->node.c_str(), printable(n->name).c_str(),
                n->usb_port.c_str(), d->id.c_str(), d->rotation, d->mirror_text().c_str(), d->calibration.empty() ? "" : ", calibrated");
    std::printf("each line: kernel time, frame, then per contact: id phase raw x,y -> display x,y (0..1 from the top left)\n");
    std::fflush(stdout);
    ContactTracker tracker(slots);
    int32_t seq = 0;
    bool down_before = false;
    long frame = 0;
    for (;;) {
        pollfd p{o.fd, POLLIN, 0};
        if (poll(&p, 1, -1) < 0) {
            if (errno == EINTR) continue;
            return 1;
        }
        for (;;) {
            input_event ev{};
            int rc = libevdev_next_event(o.dev, LIBEVDEV_READ_FLAG_NORMAL, &ev);
            if (rc == LIBEVDEV_READ_STATUS_SYNC) {
                while (rc == LIBEVDEV_READ_STATUS_SYNC) rc = libevdev_next_event(o.dev, LIBEVDEV_READ_FLAG_SYNC, &ev);
                std::printf("the kernel dropped events here (SYN_DROPPED); state read again\n");
                if (rc == -EAGAIN) continue;
            }
            if (rc == -EAGAIN) break;
            if (rc < 0) {
                std::printf("the device went away: %s\n", errno_str(-rc).c_str());
                return 1;
            }
            if (ev.type != EV_SYN || ev.code != SYN_REPORT) continue;
            if (ev.value == 1) {
                std::printf("%ld.%06ld the kernel released the touchscreen (it is going away or was reset)\n", long(ev.input_event_sec), long(ev.input_event_usec));
                continue;
            }
            ++frame;
            bool any = false;
            for (int s = 0; s < slots; ++s) {
                RawSlot& r = tracker.raw(s);
                if (mt) {
                    r.tracking = libevdev_get_slot_value(o.dev, unsigned(s), ABS_MT_TRACKING_ID);
                    r.x = libevdev_get_slot_value(o.dev, unsigned(s), ABS_MT_POSITION_X);
                    r.y = libevdev_get_slot_value(o.dev, unsigned(s), ABS_MT_POSITION_Y);
                } else {
                    bool down = libevdev_get_event_value(o.dev, EV_KEY, BTN_TOUCH) != 0;
                    if (down && !down_before) ++seq;
                    down_before = down;
                    r.tracking = down ? seq : -1;
                    r.x = libevdev_get_event_value(o.dev, EV_ABS, ABS_X);
                    r.y = libevdev_get_event_value(o.dev, EV_ABS, ABS_Y);
                }
                any = any || r.tracking != -1;
            }
            TouchEvent evs[kMaxFrameEvents];
            int n_ev = tracker.end_frame(evs, 0);
            if (!any && n_ev == 0) continue;
            std::printf("%ld.%06ld frame %ld", long(ev.input_event_sec), long(ev.input_event_usec), frame);
            static const char* names[] = {"down", "move", "up", "cancel"};
            for (int i = 0; i < n_ev; ++i) {
                float x, y;
                map.map(evs[i].x, evs[i].y, x, y);
                std::printf("  c%d %s %d,%d -> %.4f,%.4f", evs[i].contact, names[int(evs[i].phase)], evs[i].x, evs[i].y, double(x), double(y));
            }
            if (n_ev == 0) std::printf("  (no change)");
            std::printf("\n");
            std::fflush(stdout);
        }
    }
}

}
