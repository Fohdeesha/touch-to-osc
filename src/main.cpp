#include "config/config.h"
#include "config/settings.h"
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
        auto settings = tto::load_settings(*cfg, false);
        for (const auto& n : settings.notes) std::printf("%s\n", n.c_str());
        if (!settings.problem.empty()) {
            std::fprintf(stderr, "%s\n", settings.problem.c_str());
            return 2;
        }
        if (cfg->paths.settings.empty()) {
            std::printf("settings changed over OSC are kept until a restart (paths.settings is empty)\n");
        } else if (settings.overrides.empty()) {
            std::printf("no saved settings in %s\n", cfg->paths.settings.c_str());
        } else {
            std::string names;
            for (const auto& n : tto::override_names(settings.overrides)) names += (names.empty() ? "" : ", ") + n;
            std::printf("%s is good and wins over the config for: %s\n", cfg->paths.settings.c_str(), names.c_str());
        }
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
        tto::Config effective = tto::apply_overrides(*cfg, tto::load_settings(*cfg, false).overrides);
        return tto::list_devices(&effective, tto::SysPaths());
    }
    auto cfg = tto::load_config(path);
    if (!cfg) {
        TTO_CRIT("config refused: %s", cfg.error().c_str());
        return 2;
    }
    if (opts.count("dump")) {
        tto::Config effective = tto::apply_overrides(*cfg, tto::load_settings(*cfg, false).overrides);
        return tto::dump_device(effective, opts["dump"], tto::SysPaths());
    }
    auto settings = tto::load_settings(*cfg, true);
    tto::Server server(std::move(*cfg), std::move(settings), path);
    return server.run();
}
