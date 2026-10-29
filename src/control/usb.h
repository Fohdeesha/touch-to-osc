#pragma once

#include "core/clock.h"
#include "core/result.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace tto {

struct UsbPaths {
    std::string devices = "/sys/bus/usb/devices";
    std::string hub_driver = "/sys/bus/usb/drivers/hub";
    std::string dev = "/dev/bus/usb";
    std::string port_status;
};

UsbPaths usb_paths_under(const std::string& root);

struct UsbPortRef {
    std::string path;
    std::string hub;
    std::string hub_interface;
    int bus = 0;
    int port = 0;
};

std::optional<UsbPortRef> parse_usb_port(const std::string& path);

enum class PortState { unknown, empty, setting_up, enumerated, stuck };

const char* port_state_text(PortState s);
PortState port_state_from(uint16_t port_status, bool device_present);
PortState read_port_state(const UsbPortRef& ref, const UsbPaths& paths, std::string* why);
Status rebind_hub(const std::string& hub_interface, const UsbPaths& paths, const std::string& marker = std::string());
Status bind_hub_left_unbound(const std::string& marker, const UsbPaths& paths, std::string* done);
std::vector<std::string> usb_neighbours(const UsbPortRef& ref, const UsbPaths& paths);
std::string usb_target_text(const UsbPortRef& ref);
std::string usb_reset_reach(const UsbPortRef& ref, const UsbPaths& paths);

constexpr int kUsbResetsPerSpell = 5;
constexpr int kUsbResetsUntilBack = 10;

class UsbRecovery {
public:
    enum class Step { wait, reset, give_up };

    explicit UsbRecovery(int speedup = 1) : speedup_(speedup) {}
    Step observe(Nanos now, PortState state);
    void reset_done(Nanos now);
    void hold();
    void connected();
    int attempts() const { return attempts_; }
    int tries() const { return tries_; }
    bool held() const { return held_; }
    bool gave_up() const { return gave_up_; }
    Nanos wait_for(int attempts) const;

private:
    int speedup_ = 1;
    bool stuck_ = false;
    bool empty_ = false;
    bool held_ = false;
    bool gave_up_ = false;
    Nanos stuck_since_ = 0;
    Nanos empty_since_ = 0;
    int attempts_ = 0;
    int tries_ = 0;
};

// a reset of a hub resets every port on it: each screen watched there (hub interface in `watched`, "" for none) waits as
// if it had asked
void reset_done_on_hub(std::vector<UsbRecovery>& recs, const std::vector<std::string>& watched, size_t asked, const std::string& hub, Nanos now);

constexpr Nanos kSlowPortCheck = 200 * kMs;

struct UsbReading {
    size_t device = 0;
    std::string port;
    PortState state = PortState::unknown;
    std::string why;
};

using PortReader = std::function<PortState(const UsbPortRef&, std::string*)>;

class UsbWorker {
public:
    struct Done {
        bool reset = false;
        bool held = false;
        std::vector<UsbReading> readings;
        std::vector<std::string> dropped;
        Status result;
        Nanos took = 0;
    };

    explicit UsbWorker(UsbPaths paths = UsbPaths(), PortReader reader = nullptr);
    ~UsbWorker();
    UsbWorker(const UsbWorker&) = delete;
    UsbWorker& operator=(const UsbWorker&) = delete;

    const UsbPaths& paths() const { return paths_; }
    void set_marker(std::string marker) { marker_ = std::move(marker); }
    void check(std::vector<std::pair<size_t, UsbPortRef>> ports, Nanos now);
    void reset(const UsbPortRef& ref, Nanos now, bool only_if_alone = false);
    bool busy() const { return job_ != nullptr; }
    bool resetting() const { return job_ && job_->reset; }
    Nanos busy_since() const { return job_ ? job_->started : 0; }
    bool finished(Done& out);
    bool wait(Nanos max);

private:
    struct Job {
        bool reset = false;
        Nanos started = 0;
        Done out;
        std::atomic<bool> done{false};
    };
    bool running() const { return job_ && !job_->done.load(); }
    void launch(bool reset, Nanos now, std::function<void(Done&)> work);

    UsbPaths paths_;
    PortReader reader_;
    std::string marker_;
    std::shared_ptr<Job> job_;
    std::thread thread_;
};

}
