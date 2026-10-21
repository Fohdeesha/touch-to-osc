#pragma once

#include "config/config.h"
#include "config/settings.h"
#include "control/requests.h"
#include "control/shared.h"
#include "core/fd.h"
#include "core/log.h"
#include "input/probe.h"
#include "input/reader.h"
#include "osc/osc.h"

#include <deque>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace tto {

constexpr int32_t kOscApi = 2;

class Server {
public:
    Server(Config base, SettingsLoad settings, std::string config_path, SysPaths paths = SysPaths());
    ~Server();
    int run();

private:
    struct Target {
        Peer peer;
        bool touches = false;
        bool status = false;
    };
    struct DestHealth {
        bool failing = false;
        std::string error;
        uint64_t failures = 0;
        uint64_t reported = 0;
        Nanos reported_at = 0;
    };
    struct Sample {
        Nanos at;
        uint64_t resyncs;
        uint64_t silences;
        uint64_t drops;
    };
    struct DevStats {
        std::deque<std::pair<Nanos, uint32_t>> latency;
        uint64_t prev_touched_frames = 0;
        int64_t prev_touched_ns = 0;
        float rate = 0;
        std::deque<Sample> samples;
        uint64_t lost_events_seen = 0;
    };
    struct Health {
        std::string level;
        std::vector<std::string> reasons;
    };
    struct Limit {
        double tokens = 0;
        Nanos last = 0;
        uint64_t suppressed = 0;
        std::string kind;
    };

    bool open_sockets();
    void banner();
    void on_osc();
    void on_tick();
    void every_second(Nanos now);
    void drain_reader_events();
    void drain_latency(Nanos now);
    void sample(Nanos now);

    bool command(const osc::Message& m, const Peer& peer);
    bool request(const osc::Message& m, const Peer& peer);
    Status apply(const osc::Message& m, const Peer& peer);
    Status subscribe(const osc::Message& m, const Peer& peer);
    Status unsubscribe(const osc::Message& m, const Peer& peer);
    Status set_device(size_t index, const std::string& key, const std::vector<osc::Arg>& args, const Peer& peer);
    Status set_global(const std::vector<std::string>& parts, const std::vector<osc::Arg>& args, const Peer& peer);
    Status commit(Overrides next, const Peer& peer, const std::string& what);
    void apply_effects(const Config& old);
    void save_settings();
    void refuse(const Peer& peer, const std::string& address, const std::string& why, bool limited);

    std::vector<Target> targets() const;
    void publish_dests();
    bool send_bytes(const Peer& to, const std::vector<uint8_t>& bytes);
    void send_to(const Peer& to, const std::vector<osc::Message>& msgs);
    void send_target(const Target& t, const std::vector<uint8_t>& bytes);
    void send_status_changes(bool full);
    void send_alive(Nanos now);
    void send_event(const std::string& kind, const std::string& text);
    void emit(const std::string& key, log::Level level, const std::string& kind, const std::string& text);
    void report_suppressed(Nanos now);
    void expire_leases(Nanos now);
    void prune(Nanos now);
    void send_bundled(const Peer& to, const std::vector<osc::Message>& msgs);
    std::string disabled_reason(size_t index) const;
    void send_bye();
    void report_destinations(Nanos now);

    std::vector<osc::Message> status_messages();
    std::vector<osc::Message> counter_messages(Nanos now);
    Health health(Nanos now);
    std::string addr(const std::string& rest) const { return prefix_ + "/" + rest; }

    Config base_;
    Overrides over_;
    Config cfg_;
    std::vector<std::string> settings_notes_;
    std::string settings_load_problem_;
    std::string settings_problem_;
    std::string settings_file_;
    std::string config_path_;
    SysPaths paths_;
    std::string prefix_;
    std::string host_;
    int32_t run_ = 0;
    Nanos started_ = 0;
    NodeClaims claims_;
    std::vector<std::unique_ptr<DeviceShared>> shared_;
    std::vector<std::unique_ptr<DeviceReader>> readers_;
    std::vector<DevStats> stats_;
    std::vector<uint32_t> cfg_gen_;
    std::vector<uint32_t> restart_gen_;
    std::vector<uint8_t> rx_;
    std::map<std::string, Limit> limits_;
    double refusal_tokens_ = 0;
    Nanos refusal_last_ = 0;
    uint64_t refusals_dropped_ = 0;
    Nanos next_second_ = 0;
    Nanos last_save_ = 0;
    Nanos dirty_since_ = 0;
    bool settings_dirty_ = false;
    bool status_dirty_ = true;
    UniqueFd osc_fd_;
    UniqueFd send_fd_;
    UniqueFd notify_fd_;
    UniqueFd timer_fd_;
    UniqueFd signal_fd_;
    Leases leases_;
    KeptReplies kept_;
    std::map<std::string, std::vector<uint8_t>> last_status_;
    std::map<std::string, DestHealth> dest_health_;
    std::map<std::string, Nanos> refused_at_;
    int32_t alive_ = 0;
    int ticks_ = 0;
    Nanos full_at_ = 0;
    Nanos status_reply_at_ = 0;
};

}
