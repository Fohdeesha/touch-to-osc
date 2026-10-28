#include "config/atomic_file.h"
#include "config/config.h"
#include "config/settings.h"
#include "config/state.h"
#include "control/server.h"
#include "core/log.h"
#include "core/process.h"
#include "tools/tools.h"

#include "tto_version.h"

#include <toml++/toml.hpp>

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <map>
#include <optional>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

constexpr const char* kDefaultConfig = "/etc/touch-to-osc/touch-to-osc.toml";

void usage(FILE* f) {
    std::fprintf(f,
                 "usage: touch-to-osc [--config=FILE]       run (default %s)\n"
                 "       touch-to-osc --check-config[=FILE]  check a config file and its saved settings, and exit\n"
                 "       touch-to-osc --list [--all] [--config=FILE]  list the touchscreens (--all: every input device) and\n"
                 "                                           what the config makes of them\n"
                 "       touch-to-osc --list --touch[=SECONDS]  wait for a touch on a screen no device reads, and name it\n"
                 "       touch-to-osc --dump[=ID] [--config=FILE]  print each touch frame of a configured device\n"
                 "       touch-to-osc --status [--config=FILE]  show the running service's health, screens and settings\n"
                 "       touch-to-osc [--config=FILE] --send ADDRESS [VALUE...]  send it one command, such as\n"
                 "                                           --send device/screen/rotation 90 (s:TEXT sends a number as text)\n"
                 "       touch-to-osc --version | --help\n"
                 "\n"
                 "Exit codes: 0 good or a clean stop, 1 wrong command line (--dump: the screen is not there or cannot be\n"
                 "read), 2 config refused, 3 the listen port cannot be used, 4 internal error, 5 (--check-config) the config\n"
                 "is good but its saved settings are refused, 6 (--status, --send) no answer, or the command was refused.\n"
                 "More in /usr/share/doc/touch-to-osc/README.md.\n",
                 kDefaultConfig);
}

struct Loaded {
    std::optional<tto::Config> cfg;
    std::string text;
    std::string refused;
    std::string problem;
};

std::string state_path_named_in(const std::string& text) {
    try {
        toml::table t = toml::parse(text);
        if (auto s = t["paths"]["state"].value<std::string>()) return *s;
    } catch (const std::exception&) {
    }
    return tto::Config().paths.state;
}

Loaded load(const std::string& path) {
    Loaded out;
    auto text = tto::read_file(path, 1 << 20);
    tto::Result<tto::Config> cfg = text ? tto::parse_config(*text, path)
                                        : tto::Result<tto::Config>(tto::err(text.error() + " (check the path, and that the file is readable)"));
    if (cfg) {
        out.cfg = std::move(*cfg);
        out.text = std::move(*text);
        return out;
    }
    out.refused = cfg.error();
    std::string note;
    tto::State st = tto::load_state(state_path_named_in(text ? *text : std::string()), &note);
    if (st.config_path != path) return out;
    auto last = tto::parse_config(st.config_text, path + " (the copy that last worked)");
    if (!last) return out;
    out.cfg = std::move(*last);
    out.text = st.config_text;
    out.problem = "the config file is refused (" + out.refused + "): touch-to-osc runs on the copy of it that last worked" +
                  (st.config_saved.empty() ? std::string() : ", saved " + st.config_saved) + "; fix the file, then restart touch-to-osc";
    return out;
}

}

int run_main(int argc, char** argv);

int main(int argc, char** argv) {
    try {
        return run_main(argc, argv);
    } catch (const std::exception& e) {
        TTO_CRIT("internal error: %s", e.what());
    } catch (...) {
        TTO_CRIT("internal error");
    }
    return 4;
}

int run_main(int argc, char** argv) {
    std::string exe = tto::self_exe_path();
    std::map<std::string, std::string> opts;
    std::vector<std::string> send_words;
    for (int i = 1; i < argc; ++i) {
        std::string s = argv[i];
        if (s == "-h") s = "--help";
        if (s == "--send") {
            opts["send"] = "";
            send_words.assign(argv + i + 1, argv + argc);
            break;
        }
        if (s.rfind("--", 0) != 0) {
            std::fprintf(stderr, "unexpected argument \"%s\"\n", s.c_str());
            usage(stderr);
            return 1;
        }
        auto eq = s.find('=');
        std::string key = eq == std::string::npos ? s.substr(2) : s.substr(2, eq - 2);
        std::string value = eq == std::string::npos ? std::string() : s.substr(eq + 1);
        bool takes_value = key == "config" || key == "check-config" || key == "dump" || key == "touch";
        if (eq == std::string::npos && takes_value && i + 1 < argc && std::string(argv[i + 1]).rfind("-", 0) != 0) value = argv[++i];
        opts[key] = value;
    }
    for (const auto& [k, v] : opts) {
        (void)v;
        if (k != "config" && k != "check-config" && k != "list" && k != "all" && k != "touch" && k != "dump" && k != "status" && k != "send" && k != "version" &&
            k != "help") {
            std::fprintf(stderr, "unknown option --%s\n", k.c_str());
            usage(stderr);
            return 1;
        }
    }
    if (opts.count("help")) {
        usage(stdout);
        return 0;
    }
    if (opts.count("version")) {
        std::printf("touch-to-osc %s (osc_api %d)\n", TTO_VERSION, tto::kOscApi);
        return 0;
    }
    std::string path = opts.count("config") && !opts["config"].empty() ? opts["config"] : kDefaultConfig;

    if (opts.count("check-config")) {
        if (!opts["check-config"].empty()) path = opts["check-config"];
        Loaded l = load(path);
        if (!l.refused.empty()) {
            std::fprintf(stderr, "%s\n", l.refused.c_str());
            if (l.cfg) std::fprintf(stderr, "until it is fixed, touch-to-osc runs on the copy of it that last worked\n");
            return 2;
        }
        const tto::Config& cfg = *l.cfg;
        std::printf("%s is good: %zu device%s, %zu saved destination%s, OSC on %s:%d\n", path.c_str(), cfg.devices.size(), cfg.devices.size() == 1 ? "" : "s",
                    cfg.destinations.size(), cfg.destinations.size() == 1 ? "" : "s", cfg.osc.listen.host.c_str(), cfg.osc.listen.port);
        for (const auto& w : tto::overlap_warnings(cfg)) std::printf("warning: %s\n", w.c_str());
        auto settings = tto::load_settings(cfg, false);
        for (const auto& n : settings.notes) std::printf("%s\n", n.c_str());
        if (settings.no_permission) {
            std::printf("the saved settings in %s can only be read by root: run it with sudo to check them too\n", cfg.paths.settings.c_str());
            return 0;
        }
        if (!settings.problem.empty()) {
            std::fprintf(stderr, "%s\n", settings.problem.c_str());
            return tto::kCheckSettingsRefused;
        }
        if (cfg.paths.settings.empty()) {
            std::printf("settings changed over OSC are kept until a restart (paths.settings is empty)\n");
        } else if (settings.overrides.empty()) {
            std::printf("no saved settings in %s\n", cfg.paths.settings.c_str());
        } else {
            std::string names;
            for (const auto& n : tto::override_names(settings.overrides)) names += (names.empty() ? "" : ", ") + n;
            std::printf("%s is good and wins over the config for: %s\n", cfg.paths.settings.c_str(), names.c_str());
        }
        return 0;
    }
    if (opts.count("status") || opts.count("send")) {
        Loaded l = load(path);
        tto::Config cfg;
        if (l.cfg) cfg = *l.cfg;
        else std::fprintf(stderr, "(%s; asking touch-to-osc on 127.0.0.1:%d)\n", l.refused.c_str(), cfg.osc.listen.port);
        return opts.count("status") ? tto::status_command(cfg) : tto::send_command(cfg, send_words);
    }
    if (opts.count("list")) {
        bool explicit_config = opts.count("config") != 0;
        bool all = opts.count("all") != 0;
        int touch = 0;
        if (opts.count("touch")) {
            const std::string& v = opts["touch"];
            touch = v.empty() ? 30 : std::atoi(v.c_str());
            if (touch < 1 || touch > 600 || v.find_first_not_of("0123456789") != std::string::npos) {
                std::fprintf(stderr, "--touch takes the seconds to wait, 1-600 (30 by default)\n");
                return 1;
            }
        }
        auto list = [&](const tto::Config* c) { return touch ? tto::touch_to_find(c, tto::SysPaths(), touch) : tto::list_devices(c, tto::SysPaths(), all); };
        if (!explicit_config && access(path.c_str(), F_OK) != 0) return list(nullptr);
        auto cfg = tto::load_config(path);
        if (!cfg) {
            std::fprintf(stderr, "(config not used: %s)\n", cfg.error().c_str());
            return list(nullptr);
        }
        tto::Config effective = tto::apply_overrides(*cfg, tto::load_settings(*cfg, false).overrides);
        return list(&effective);
    }
    Loaded l = load(path);
    if (!l.cfg || (opts.count("dump") && !l.refused.empty())) {
        TTO_CRIT("config refused: %s", l.refused.c_str());
        return 2;
    }
    if (opts.count("dump")) {
        tto::Config effective = tto::apply_overrides(*l.cfg, tto::load_settings(*l.cfg, false).overrides);
        return tto::dump_device(effective, opts["dump"], tto::SysPaths());
    }
    auto settings = tto::load_settings(*l.cfg, true);
    // every thread inherits the mask it was started with: a stop signal must only ever reach the server's signalfd
    sigset_t stop_signals;
    sigemptyset(&stop_signals);
    sigaddset(&stop_signals, SIGTERM);
    sigaddset(&stop_signals, SIGINT);
    pthread_sigmask(SIG_BLOCK, &stop_signals, nullptr);
    int rc = 0;
    {
        tto::Server server(std::move(*l.cfg), std::move(settings), tto::Launch{path, l.text, l.problem, exe});
        rc = server.run();
    }
    if (rc != tto::kRestartExit) return rc;
    // the same process starts again, so systemd keeps watching the same PID; a newly installed binary takes over
    sigset_t none;
    sigemptyset(&none);
    sigprocmask(SIG_SETMASK, &none, nullptr);
    execv(exe.c_str(), argv);
    TTO_CRIT("restarting failed: cannot run %s again (%s)", exe.c_str(), std::strerror(errno));
    return 4;
}
