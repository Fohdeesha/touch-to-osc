#include "control/usb.h"

#include "config/atomic_file.h"
#include "core/fd.h"
#include "core/process.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <linux/usbdevice_fs.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace tto {

namespace {

constexpr uint16_t kPortConnection = 0x0001;
constexpr uint16_t kPortEnable = 0x0002;
constexpr Nanos kEmptyForgets = 5 * kSec;

std::string root_hint(const char* what) {
    if (geteuid() == 0) return {};
    return std::string("; touch-to-osc must run as root to ") + what;
}

bool exists(const std::string& path) {
    struct stat st {};
    return stat(path.c_str(), &st) == 0;
}

bool number(const std::string& s, int max, int& out) {
    if (s.empty() || s.size() > 3 || s[0] == '0') return false;
    int v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        v = v * 10 + (c - '0');
    }
    if (v < 1 || v > max) return false;
    out = v;
    return true;
}

int read_int(const std::string& path) {
    std::ifstream in(path);
    int v = -1;
    if (!(in >> v)) return -1;
    return v;
}

Status write_text(const std::string& file, const std::string& text) {
    UniqueFd fd(open(file.c_str(), O_WRONLY | O_CLOEXEC));
    if (!fd) return err("cannot open " + file + ": " + errno_str());
    ssize_t n = write(fd.get(), text.data(), text.size());
    if (n != ssize_t(text.size())) return err("writing " + text + " to " + file + " failed: " + (n < 0 ? errno_str() : std::string("short write")));
    return {};
}

}

std::optional<UsbPortRef> parse_usb_port(const std::string& path) {
    size_t dash = path.find('-');
    if (dash == std::string::npos) return std::nullopt;
    UsbPortRef r;
    if (!number(path.substr(0, dash), 999, r.bus)) return std::nullopt;
    std::vector<int> chain;
    size_t pos = dash + 1;
    while (true) {
        size_t dot = path.find('.', pos);
        int p = 0;
        if (!number(path.substr(pos, dot == std::string::npos ? std::string::npos : dot - pos), 255, p)) return std::nullopt;
        chain.push_back(p);
        if (dot == std::string::npos) break;
        pos = dot + 1;
    }
    if (chain.size() > 7) return std::nullopt;
    std::string bus = std::to_string(r.bus);
    r.path = path;
    r.port = chain.back();
    if (chain.size() == 1) {
        r.hub = "usb" + bus;
        r.hub_interface = bus + "-0:1.0";
    } else {
        r.hub = bus + "-" + std::to_string(chain[0]);
        for (size_t i = 1; i + 1 < chain.size(); ++i) r.hub += "." + std::to_string(chain[i]);
        r.hub_interface = r.hub + ":1.0";
    }
    return r;
}

const char* port_state_text(PortState s) {
    switch (s) {
    case PortState::unknown: return "unknown";
    case PortState::empty: return "nothing plugged in";
    case PortState::setting_up: return "being set up";
    case PortState::enumerated: return "set up";
    case PortState::stuck: return "plugged in but not set up";
    }
    return "?";
}

PortState port_state_from(uint16_t port_status, bool device_present) {
    if (device_present) return PortState::enumerated;
    if (!(port_status & kPortConnection)) return PortState::empty;
    // The kernel keeps the port enabled while it sets a device up (holding the hub's lock, so an unbind
    // would wait for it) and disables the port when it gives up.
    return (port_status & kPortEnable) ? PortState::setting_up : PortState::stuck;
}

PortState read_port_state(const UsbPortRef& ref, const UsbPaths& paths, std::string* why) {
    if (exists(paths.devices + "/" + ref.path)) return PortState::enumerated;
    std::string hub = paths.devices + "/" + ref.hub;
    int busnum = read_int(hub + "/busnum"), devnum = read_int(hub + "/devnum");
    if (busnum <= 0 || devnum <= 0) {
        if (why) *why = "the USB hub " + ref.hub + " holding port " + ref.path + " is not there";
        return PortState::unknown;
    }
    char node[256];
    std::snprintf(node, sizeof node, "%s/%03d/%03d", paths.dev.c_str(), busnum, devnum);
    UniqueFd fd(open(node, O_RDWR | O_CLOEXEC));
    if (!fd) {
        if (why) *why = std::string("cannot open ") + node + ": " + errno_str() + root_hint("watch USB ports (its service does), or usb_recovery must be turned off for this screen");
        return PortState::unknown;
    }
    uint8_t buf[4] = {};
    usbdevfs_ctrltransfer c{};
    c.bRequestType = 0xA3;
    c.bRequest = 0;
    c.wValue = 0;
    c.wIndex = uint16_t(ref.port);
    c.wLength = sizeof buf;
    c.timeout = 500;
    c.data = buf;
    int n = ioctl(fd.get(), USBDEVFS_CONTROL, &c);
    if (n != int(sizeof buf)) {
        if (why) *why = "reading the status of USB port " + ref.path + " failed: " + (n < 0 ? errno_str() : std::string("short answer"));
        return PortState::unknown;
    }
    uint16_t status = uint16_t(buf[0] | (buf[1] << 8));
    return port_state_from(status, exists(paths.devices + "/" + ref.path));
}

Status rebind_hub(const std::string& hub_interface, const UsbPaths& paths, const std::string& marker) {
    std::string bound = paths.devices + "/" + hub_interface + "/driver";
    if (!marker.empty()) {
        Status m = write_file_atomic(marker, hub_interface + "\n");
        (void)m;
    }
    auto done = [&] {
        if (!marker.empty()) unlink(marker.c_str());
    };
    Status un = write_text(paths.hub_driver + "/unbind", hub_interface);
    if (!un && exists(bound)) {
        done();
        return err("the USB hub " + hub_interface + " could not be reset (" + un.error() + ")" + root_hint("reset USB ports (its service does)"));
    }
    Status st;
    for (int i = 0; i < 5; ++i) {
        st = write_text(paths.hub_driver + "/bind", hub_interface);
        if (exists(bound)) {
            done();
            return {};
        }
        usleep(100000);
    }
    return err("the USB hub " + hub_interface + " could not be bound again (" + (st ? std::string("the kernel did not take it") : st.error()) +
               "): every device on it stays off until it is: echo " + hub_interface + " > " + paths.hub_driver + "/bind, or reboot");
}

Status bind_hub_left_unbound(const std::string& marker, const UsbPaths& paths, std::string* done) {
    if (marker.empty() || !exists(marker)) return {};
    auto text = read_file(marker, 256);
    std::string hub = text ? text->substr(0, text->find('\n')) : std::string();
    bool valid = hub.size() > 4 && hub.size() <= 32 && hub.compare(hub.size() - 4, 4, ":1.0") == 0 &&
                 hub.find_first_not_of("0123456789-.:") == std::string::npos;
    std::string bound = paths.devices + "/" + hub + "/driver";
    if (!valid || exists(bound)) {
        unlink(marker.c_str());
        return {};
    }
    Status st;
    for (int i = 0; i < 5; ++i) {
        st = write_text(paths.hub_driver + "/bind", hub);
        if (exists(bound)) {
            unlink(marker.c_str());
            if (done) *done = "the USB hub " + hub + " was left unbound by a reset cut short when touch-to-osc last stopped; it is bound again";
            return {};
        }
        usleep(100000);
    }
    return err("the USB hub " + hub + " was left unbound by a reset cut short when touch-to-osc last stopped and could not be bound again (" +
               (st ? std::string("the kernel did not take it") : st.error()) + "): every device on it stays off until it is: echo " + hub + " > " +
               paths.hub_driver + "/bind, or reboot");
}

std::vector<std::string> usb_neighbours(const UsbPortRef& ref, const UsbPaths& paths) {
    std::vector<std::string> out;
    std::string under = ref.hub.rfind("usb", 0) == 0 ? std::to_string(ref.bus) + "-" : ref.hub + ".";
    DIR* d = opendir(paths.devices.c_str());
    if (!d) return out;
    std::vector<std::string> names;
    while (dirent* e = readdir(d)) {
        std::string n = e->d_name;
        if (n.find(':') != std::string::npos || n.rfind(under, 0) != 0) continue;
        if (n == ref.path || n.rfind(ref.path + ".", 0) == 0) continue;
        names.push_back(n);
    }
    closedir(d);
    std::sort(names.begin(), names.end());
    for (const auto& n : names) {
        std::ifstream in(paths.devices + "/" + n + "/product");
        std::string product;
        std::getline(in, product);
        for (char& c : product) {
            if (c < 0x20 || c >= 0x7f) c = '?';
        }
        out.push_back((product.empty() ? std::string("a USB device") : product) + " (" + n + ")");
    }
    return out;
}

std::string usb_target_text(const UsbPortRef& ref) {
    return ref.hub.rfind("usb", 0) == 0 ? "USB bus " + std::to_string(ref.bus) : "the USB hub " + ref.hub;
}

std::string usb_reset_reach(const UsbPortRef& ref, const UsbPaths& paths) {
    std::string target = usb_target_text(ref);
    auto others = usb_neighbours(ref, paths);
    if (others.empty()) return target + ", which holds nothing else";
    std::string list;
    for (const auto& o : others) list += (list.empty() ? "" : ", ") + o;
    return target + ", which also holds " + list;
}

Nanos UsbRecovery::wait_for(int attempts) {
    static const int steps[] = {10, 20, 40, 80, 160};
    if (attempts >= 0 && attempts < 5) return Nanos(steps[attempts]) * kSec;
    return 300 * kSec;
}

bool UsbRecovery::observe(Nanos now, PortState state) {
    if (state == PortState::empty) {
        if (!empty_) {
            empty_ = true;
            empty_since_ = now;
        }
        if (now - empty_since_ >= kEmptyForgets) attempts_ = 0;
    } else {
        empty_ = false;
    }
    if (state != PortState::stuck) {
        stuck_ = false;
        return false;
    }
    if (!stuck_) {
        stuck_ = true;
        stuck_since_ = now;
    }
    return now - stuck_since_ >= wait_for(attempts_);
}

void UsbRecovery::reset_done(Nanos now) {
    if (attempts_ < 1000) ++attempts_;
    stuck_ = true;
    stuck_since_ = now;
}

void reset_done_on_hub(std::vector<UsbRecovery>& recs, const std::vector<std::string>& watched, size_t asked, const std::string& hub, Nanos now) {
    for (size_t j = 0; j < recs.size(); ++j) {
        if (j == asked || (j < watched.size() && !hub.empty() && watched[j] == hub)) recs[j].reset_done(now);
    }
}

void UsbRecovery::connected() {
    stuck_ = false;
    empty_ = false;
    attempts_ = 0;
}

UsbPaths usb_paths_under(const std::string& root) {
    UsbPaths p;
    p.devices = root + "/sys/bus/usb/devices";
    p.hub_driver = root + "/sys/bus/usb/drivers/hub";
    p.dev = root + "/dev/bus/usb";
    return p;
}

UsbWorker::UsbWorker(UsbPaths paths, PortReader reader) : paths_(std::move(paths)), reader_(std::move(reader)) {}

UsbWorker::~UsbWorker() {
    if (!thread_.joinable()) return;
    if (running()) thread_.detach();
    else thread_.join();
}

void UsbWorker::check(std::vector<std::pair<size_t, UsbPortRef>> ports, Nanos now) {
    launch(false, now, [ports = std::move(ports), paths = paths_, reader = reader_](Done& out) {
        for (const auto& [device, ref] : ports) {
            UsbReading r;
            r.device = device;
            r.port = ref.path;
            Nanos t0 = monotonic_ns();
            r.state = reader ? reader(ref, &r.why) : read_port_state(ref, paths, &r.why);
            // A check that waited for the hub's lock ran while the kernel was busy with the hub (it holds the lock for its
            // whole series of tries at setting a device up), so the kernel has not given up on the port.
            if (r.state == PortState::stuck && monotonic_ns() - t0 >= kSlowPortCheck) r.state = PortState::setting_up;
            out.readings.push_back(std::move(r));
        }
    });
}

void UsbWorker::reset(const UsbPortRef& ref, Nanos now) {
    launch(true, now, [ref, paths = paths_, marker = marker_](Done& out) {
        out.dropped = usb_neighbours(ref, paths);
        out.result = rebind_hub(ref.hub_interface, paths, marker);
    });
}

void UsbWorker::launch(bool reset, Nanos now, std::function<void(Done&)> work) {
    if (thread_.joinable()) thread_.join();
    auto job = std::make_shared<Job>();
    job->reset = reset;
    job->started = now;
    job->out.reset = reset;
    job_ = job;
    auto task = [job, work = std::move(work)] {
        set_thread_name("usb");
        Nanos t0 = monotonic_ns();
        try {
            work(job->out);
        } catch (const std::exception& e) {
            job->out.result = err(std::string("internal error: ") + e.what());
        }
        job->out.took = monotonic_ns() - t0;
        job->done.store(true);
    };
    thread_ = std::thread(std::move(task));
}

bool UsbWorker::finished(Done& out) {
    if (!job_ || !job_->done.load()) return false;
    if (thread_.joinable()) thread_.join();
    out = std::move(job_->out);
    job_.reset();
    return true;
}

bool UsbWorker::wait(Nanos max) {
    Nanos until = monotonic_ns() + max;
    while (running()) {
        if (monotonic_ns() >= until) return false;
        usleep(10000);
    }
    return true;
}

}
