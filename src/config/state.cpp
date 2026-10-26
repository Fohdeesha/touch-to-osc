#include "config/state.h"

#include "config/atomic_file.h"
#include "config/config.h"

#include <toml++/toml.hpp>

#include <cerrno>
#include <ctime>
#include <sstream>
#include <sys/stat.h>

namespace tto {

namespace {

bool plain(const std::string& s, size_t max_len) { return check_text(s, max_len).empty(); }

std::string text_of(const toml::node* n) {
    if (!n || !n->is_string()) return {};
    return std::string(n->as_string()->get());
}

}

State parse_state(const std::string& text, const std::string& source, std::string* problem) {
    State s;
    toml::table tbl;
    try {
        tbl = toml::parse(text, source);
    } catch (const toml::parse_error& e) {
        if (problem) *problem = source + " cannot be read back (" + std::string(e.description()) + "): touch-to-osc starts without what it kept there";
        return s;
    }
    if (const toml::table* ports = tbl["usb_ports"].as_table()) {
        for (auto&& [k, v] : *ports) {
            std::string id(k.str()), port = text_of(&v);
            if (plain(id, 32) && !port.empty() && plain(port, 63)) s.usb_ports[id] = port;
        }
    }
    if (const toml::table* c = tbl["config_that_worked"].as_table()) {
        std::string path = text_of(c->get("path")), config = text_of(c->get("text"));
        if (!path.empty() && !config.empty()) {
            s.config_path = path;
            s.config_text = config;
            s.config_saved = text_of(c->get("saved"));
        }
    }
    return s;
}

std::string state_text(const State& s) {
    toml::table t;
    if (!s.usb_ports.empty()) {
        toml::table ports;
        for (const auto& [id, port] : s.usb_ports) ports.insert(id, port);
        t.insert("usb_ports", std::move(ports));
    }
    if (!s.config_path.empty()) {
        t.insert("config_that_worked", toml::table{{"path", s.config_path}, {"saved", s.config_saved}, {"text", s.config_text}});
    }
    std::ostringstream os;
    os << "# Written by touch-to-osc: what it keeps between runs (the USB port each screen was last read on,\n"
       << "# and a copy of the last config file that started). Not settings; it may be deleted while touch-to-osc is stopped.\n\n"
       << t << "\n";
    return os.str();
}

State load_state(const std::string& path, std::string* problem) {
    struct stat st {};
    if (path.empty() || stat(path.c_str(), &st) != 0) return State();
    auto text = read_file(path, 1 << 20);
    if (!text) {
        if (problem) *problem = "what touch-to-osc kept between runs cannot be read (" + text.error() + "): it starts without it";
        return State();
    }
    return parse_state(*text, path, problem);
}

std::string usb_reset_marker(const std::string& state_path) { return state_path.empty() ? std::string() : state_path + ".usb-reset"; }

std::string local_time_text() {
    std::time_t now = std::time(nullptr);
    std::tm t{};
    localtime_r(&now, &t);
    char b[32];
    std::strftime(b, sizeof b, "%Y-%m-%d %H:%M:%S", &t);
    return b;
}

}
