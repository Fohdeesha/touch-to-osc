#include "config/config.h"
#include "control/server.h"
#include "core/log.h"
#include "tools/tools.h"

#include "tto_version.h"

#include <cstdio>
#include <exception>
#include <map>
#include <string>
#include <unistd.h>

namespace {

constexpr const char* kDefaultConfig = "/etc/touch-to-osc/touch-to-osc.toml";

void usage(FILE* f) {
    std::fprintf(f,
                 "usage: touch-to-osc [--config=FILE]       run (default %s)\n"
                 "       touch-to-osc --check-config[=FILE]  check a config file and exit (0 good, 2 refused)\n"
                 "       touch-to-osc --list [--config=FILE] list every input device and what the config makes of it\n"
                 "       touch-to-osc --dump[=ID] [--config=FILE]  print each touch frame of a configured device\n"
                 "       touch-to-osc --version\n",
                 kDefaultConfig);
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
    std::map<std::string, std::string> opts;
    for (int i = 1; i < argc; ++i) {
        std::string s = argv[i];
        if (s.rfind("--", 0) != 0) {
            std::fprintf(stderr, "unexpected argument \"%s\"\n", s.c_str());
            usage(stderr);
            return 1;
        }
        auto eq = s.find('=');
        if (eq == std::string::npos) opts[s.substr(2)] = "";
        else opts[s.substr(2, eq - 2)] = s.substr(eq + 1);
    }
    for (const auto& [k, v] : opts) {
        (void)v;
        if (k != "config" && k != "check-config" && k != "list" && k != "dump" && k != "version" && k != "help") {
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
        auto cfg = tto::load_config(path);
        if (!cfg) {
            std::fprintf(stderr, "%s\n", cfg.error().c_str());
            return 2;
        }
        std::printf("%s is good: %zu device%s, %zu saved destination%s, OSC on %s:%d\n", path.c_str(), cfg->devices.size(), cfg->devices.size() == 1 ? "" : "s",
                    cfg->destinations.size(), cfg->destinations.size() == 1 ? "" : "s", cfg->osc.listen.host.c_str(), cfg->osc.listen.port);
        return 0;
    }
    if (opts.count("list")) {
        bool explicit_config = opts.count("config") != 0;
        if (!explicit_config && access(path.c_str(), F_OK) != 0) return tto::list_devices(nullptr, tto::SysPaths());
        auto cfg = tto::load_config(path);
        if (!cfg) {
            std::fprintf(stderr, "(config not used: %s)\n", cfg.error().c_str());
            return tto::list_devices(nullptr, tto::SysPaths());
        }
        return tto::list_devices(&*cfg, tto::SysPaths());
    }
    auto cfg = tto::load_config(path);
    if (!cfg) {
        TTO_CRIT("config refused: %s", cfg.error().c_str());
        return 2;
    }
    if (opts.count("dump")) return tto::dump_device(*cfg, opts["dump"], tto::SysPaths());
    tto::log::Level level = tto::log::Level::info;
    tto::log::parse_level(cfg->log.level.c_str(), level);
    tto::log::set_min_level(level);
    tto::Server server(std::move(*cfg), path);
    return server.run();
}
