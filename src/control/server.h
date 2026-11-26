#pragma once

#include "config/config.h"
#include "config/file_writer.h"
#include "config/settings.h"
#include "config/state.h"
#include "control/requests.h"
#include "control/shared.h"
#include "control/stall.h"
#include "control/usb.h"
#include "core/budget.h"
#include "core/fd.h"
#include "core/log.h"
#include "input/probe.h"
#include "input/reader.h"
#include "osc/osc.h"

#include <atomic>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace tto {

constexpr int32_t kOscApi = 6;
constexpr int kRestartExit = 100;
constexpr int kCheckSettingsRefused = 5;
constexpr const char* kRestartedBy = "TTO_RESTARTED_BY";
constexpr const char* kKeptLeases = "TTO_LEASES";
constexpr const char* kRestartBudget = "TTO_RESTARTS";

struct Launch {
    std::string config_path;
    std::string config_text;
    std::string config_problem;
    std::string exe;
    std::string start_file;
};

class Server {
public:
    Server(Config base, SettingsLoad settings, Launch launch, SysPaths paths = SysPaths());
    ~Server();
    int run();

private:
    struct Target {
        Peer peer;
        bool touches = false;
        bool status = false;
        bool plain = false;
    };
    struct DestHealth {
        bool failing = false;
        std::string error;
        uint64_t failures = 0;
        uint64_t reported = 0;
        bool told = false;
        Budget log{10 * kSec, 6, 300 * kSec};
    };
    struct Sample {
        Nanos at;
        uint64_t resyncs;
        uint64_t silences;
        uint64_t drops;
        uint64_t losses;
    };
    struct UsbAsk {
        size_t device;
        UsbPortRef ref;
        std::string by;
    };
    struct Asked {
        Peer peer;
        int32_t seq = 0;
        std::vector<uint8_t> command;
        std::string note;
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
        std::vector<std::pair<std::string, std::vector<std::string>>> devices;
    };
    struct Limit {
        double tokens = 0;
        Nanos last = 0;
        uint64_t suppressed = 0;
        std::string kind;
        log::Level level = log::Level::notice;
        std::string device;
        Budget log{0, 60, 60 * kSec};
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
    Status commit(Overrides next, const Peer& peer, const std::string& what, std::string device = std::string());
    void apply_effects(const Config& old);
    void save_settings();
    Status save_settings_now(const Overrides& o, const Overrides& values);
    Status lockout(const Config& eff, const Peer& peer) const;
    void save_state();
    void collect_writes(Nanos now);
    void note_write(const FileWriter::Outcome& o, Nanos now);
    std::string write_hint(const Status& st) const;
    void watch_readers(Nanos now);
    [[noreturn]] void end_on_stalled_reader(size_t index, Nanos now, Nanos last, int32_t contacts);
    void clear_usb_problem(const std::string& why);
    void forget_usb_problem();
    void remember_port(size_t index);
    void repair_usb_hub();
    void keep_restart_reply();
    void restore_leases();
    Status restart_budget(Nanos now) const;
    void count_restart(Nanos now);
    void settings_saved(const FileWriter::Outcome& o);
    bool wait_usb_worker(Nanos max, bool stopping);
    Status restart_check(const Overrides* pending = nullptr, const Overrides* pending_values = nullptr);
    void refuse(const Peer& peer, const std::string& address, const std::string& why, bool limited, uint16_t also_port = 0);
    void reply_to(const Peer& to, const std::vector<osc::Message>& msgs);
    std::string outsider_reason(const Peer& peer) const;
    void hello(const osc::Message& m, const Peer& peer, bool allowed);
    void update_modes();
    void expire_modes(Nanos now);
    void mode_touch(size_t index, const ModeTouch& t);
    Status calibration_fit(size_t index, const std::vector<osc::Arg>& args, const Peer& peer);
    std::string calibrating_id() const;
    const DeviceConfig* file_device(const std::string& id) const;
    Status apply_by_restart(Overrides next, const Peer& peer, const std::string& what, const std::string& then);
    Status test_bind(const HostPort& to) const;
    std::vector<std::string> describe_items() const;
    void send_description(const Peer& to);
    void watch_screens();
    void stop_screens();
    void collect_screens();

    std::vector<Target> targets() const;
    void publish_dests();
    bool send_bytes(const Peer& to, const std::vector<uint8_t>& bytes);
    void send_to(const Peer& to, const std::vector<osc::Message>& msgs);
    void send_target(const Target& t, const std::vector<uint8_t>& bytes);
    void send_status_changes(bool full);
    void send_alive(Nanos now, const Peer* only = nullptr);
    void send_event(const std::string& kind, const std::string& text, log::Level level, const std::string& device);
    void emit(const std::string& key, log::Level level, const std::string& kind, const std::string& text, const std::string& device = std::string());
    void log_limited(Limit& l, log::Level level, const std::string& line, Nanos now);
    void report_suppressed(Nanos now);
    void expire_leases(Nanos now);
    void prune(Nanos now);
    void send_full_status(const Peer& to, bool plain, bool with_counters);
    std::vector<std::vector<uint8_t>> pack_status(const std::vector<osc::Message>& msgs, bool numbered);
    bool health_part(const std::string& address) const;
    bool is_plain(const Peer& p) const;
    std::string disabled_reason(size_t index) const;
    std::optional<UsbPortRef> usb_port_of(size_t index) const;
    bool usb_watchable(size_t index, const DevInfo& info) const;
    void watch_usb(Nanos now);
    void note_port(size_t index, const std::string& port, PortState state, const std::string& why, Nanos now);
    void collect_usb(Nanos now, bool start_next);
    void finish_usb_reset(Nanos now, const UsbWorker::Done& done);
    bool start_usb_reset(size_t index, const UsbPortRef& ref, const std::string& asked_by);
    std::string usb_reason(size_t index) const;
    std::string usb_port_text(size_t index) const;
    void send_bye();
    void report_destinations(Nanos now);

    std::vector<osc::Message> status_messages();
    std::vector<osc::Message> counter_messages(Nanos now, bool plain = false);
    Health health(Nanos now);
    std::string addr(const std::string& rest) const { return prefix_ + "/" + rest; }

    Config base_;
    Overrides over_;
    Overrides over_values_;
    Config cfg_;
    std::vector<std::string> settings_notes_;
    std::string settings_load_problem_;
    std::string settings_problem_;
    std::string settings_file_;
    std::string config_path_;
    std::string config_text_;
    std::string config_problem_;
    std::string exe_;
    std::string state_path_;
    State state_;
    std::string state_problem_;
    FileWriter writer_;
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
    Budget unanswered_log_{10 * kSec, 6, 60 * kSec};
    Budget outsider_log_{5 * kSec, 12, 60 * kSec};
    Budget refused_log_{5 * kSec, 12, 60 * kSec};
    Budget malformed_log_{30 * kSec, 10, 600 * kSec};
    Budget oversize_log_{30 * kSec, 10, 600 * kSec};
    Budget debug_log_{0, 200, 100 * kMs};
    Budget usb_check_log_{60 * kSec, 5, 600 * kSec};
    double restart_tokens_ = 0;
    Nanos restart_last_ = 0;
    std::string start_file_;
    Nanos next_second_ = 0;
    Nanos last_save_ = 0;
    Nanos dirty_since_ = 0;
    bool settings_dirty_ = false;
    bool status_dirty_ = true;
    bool restart_requested_ = false;
    std::string usb_root_;
    UsbWorker usb_worker_;
    std::vector<size_t> usb_checking_;
    std::optional<UsbAsk> usb_asked_;
    std::optional<size_t> usb_due_;
    size_t usb_worker_dev_ = 0;
    bool usb_worker_manual_ = false;
    std::string usb_worker_target_;
    std::string usb_worker_hub_;
    std::optional<Asked> asking_;
    std::optional<Asked> restart_asked_;
    std::vector<std::pair<Peer, int>> carried_leases_;
    std::string usb_problem_;
    std::vector<UsbRecovery> usb_rec_;
    std::vector<std::string> usb_last_port_;
    std::vector<PortState> usb_state_;
    std::vector<bool> usb_watching_;
    std::vector<std::string> usb_why_;
    std::vector<uint64_t> usb_resets_;
    std::vector<std::string> usb_held_by_;
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
    int32_t status_gen_ = 0;
    Nanos identify_until_ = 0;
    std::vector<Nanos> calibrate_until_;
    std::string reply_note_;
    Nanos describe_at_ = 0;
    std::thread screens_thread_;
    UniqueFd screens_stop_;
    std::mutex screens_m_;
    std::vector<std::string> screens_found_;
    std::atomic<uint32_t> screens_gen_{0};
    uint32_t screens_seen_ = 0;
    bool screens_first_ = true;
    std::vector<std::string> unused_screens_;
    size_t usb_problem_dev_ = SIZE_MAX;
    enum class HubFault { other, unbound, absent };
    std::string usb_problem_hub_;
    HubFault usb_problem_kind_ = HubFault::other;
    Nanos usb_repair_at_ = 0;
    Nanos usb_reset_asked_at_ = 0;
    Nanos state_retry_at_ = 0;
    std::vector<StallWatch> reader_stall_;
    uint64_t libevdev_seen_ = 0;
    Nanos libevdev_logged_ = 0;
    int ticks_ = 0;
    Nanos full_at_ = 0;
    Nanos status_reply_at_ = 0;
};

}
