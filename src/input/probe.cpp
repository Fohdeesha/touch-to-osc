#include "input/probe.h"

#include <algorithm>
#include <cstdlib>
#include <dirent.h>
#include <fstream>
#include <limits.h>
#include <linux/input.h>
#include <sstream>
#include <sys/stat.h>

namespace tto {

namespace {

std::string read_line(const std::string& path) {
    std::ifstream in(path);
    std::string s;
    if (in) std::getline(in, s);
    return s;
}

std::string read_raw(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::string s((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    while (!s.empty() && (s.back() == '\n' || s.back() == '\0')) s.pop_back();
    return s;
}

uint16_t read_hex16(const std::string& path) { return uint16_t(std::strtoul(read_line(path).c_str(), nullptr, 16)); }

std::vector<unsigned long long> read_bitmap(const std::string& path) {
    std::istringstream in(read_line(path));
    std::vector<unsigned long long> words;
    std::string w;
    while (in >> w) words.push_back(std::strtoull(w.c_str(), nullptr, 16));
    std::reverse(words.begin(), words.end());
    return words;
}

bool bit(const std::vector<unsigned long long>& words, unsigned b) {
    size_t i = b / 64;
    return i < words.size() && (words[i] >> (b % 64)) & 1ULL;
}

bool exists(const std::string& path) {
    struct stat st {};
    return stat(path.c_str(), &st) == 0;
}

void find_usb_parent(const std::string& input_dir, std::string& port, std::string& serial) {
    char real[PATH_MAX];
    if (!realpath(input_dir.c_str(), real)) return;
    std::string p = real;
    while (p.size() > 1) {
        size_t slash = p.rfind('/');
        if (slash == std::string::npos || slash == 0) return;
        p = p.substr(0, slash);
        if (exists(p + "/idVendor") && exists(p + "/busnum") && exists(p + "/devpath")) {
            port = p.substr(p.rfind('/') + 1);
            serial = read_raw(p + "/serial");
            return;
        }
    }
}

}

std::vector<NodeInfo> scan_nodes(const SysPaths& paths) {
    std::vector<NodeInfo> out;
    DIR* d = opendir(paths.sys_class_input.c_str());
    if (!d) return out;
    while (dirent* e = readdir(d)) {
        std::string sys = e->d_name;
        if (sys.rfind("event", 0) != 0) continue;
        const char* digits = sys.c_str() + 5;
        if (!*digits) continue;
        char* end = nullptr;
        long num = std::strtol(digits, &end, 10);
        if (!end || *end) continue;
        NodeInfo n;
        n.sysname = sys;
        n.number = int(num);
        n.node = paths.dev_input + "/" + sys;
        std::string dev = paths.sys_class_input + "/" + sys + "/device";
        n.name = read_line(dev + "/name");
        n.phys = read_line(dev + "/phys");
        n.bustype = read_hex16(dev + "/id/bustype");
        n.vendor = read_hex16(dev + "/id/vendor");
        n.product = read_hex16(dev + "/id/product");
        auto props = read_bitmap(dev + "/properties");
        auto abs = read_bitmap(dev + "/capabilities/abs");
        auto key = read_bitmap(dev + "/capabilities/key");
        n.direct = bit(props, INPUT_PROP_DIRECT);
        n.mt_slots = bit(abs, ABS_MT_SLOT);
        n.mt_xy = bit(abs, ABS_MT_POSITION_X) && bit(abs, ABS_MT_POSITION_Y);
        n.abs_xy = bit(abs, ABS_X) && bit(abs, ABS_Y);
        n.btn_touch = bit(key, BTN_TOUCH);
        find_usb_parent(dev, n.usb_port, n.usb_serial);
        out.push_back(std::move(n));
    }
    closedir(d);
    std::sort(out.begin(), out.end(), [](const NodeInfo& a, const NodeInfo& b) { return a.number < b.number; });
    return out;
}

TouchKind touch_kind(const NodeInfo& n, std::string* why) {
    if (!n.direct) {
        if (why) *why = "it is not a direct-touch device (a touchpad, mouse or keyboard?)";
        return TouchKind::none;
    }
    if (n.mt_slots && n.mt_xy) return TouchKind::multitouch;
    if (n.abs_xy && n.btn_touch) return TouchKind::single;
    if (why) *why = "it reports no touch positions";
    return TouchKind::none;
}

bool matches(const DeviceConfig& d, const NodeInfo& n) {
    if (d.has_usb && (n.vendor != d.vendor || n.product != d.product)) return false;
    if (!d.serial.empty() && printable(n.usb_serial) != d.serial) return false;
    if (!d.port.empty() && (n.usb_port.empty() ? n.phys : n.usb_port) != d.port) return false;
    return true;
}

std::string printable(const std::string& s) {
    std::string out;
    for (unsigned char c : s) out += (c >= 0x20 && c < 0x7f) ? char(c) : '?';
    return out;
}

std::string usb_identity(const NodeInfo& n) {
    std::string s = usb_text(n.vendor, n.product);
    if (!n.usb_serial.empty()) s += " " + printable(n.usb_serial);
    return s;
}

}
