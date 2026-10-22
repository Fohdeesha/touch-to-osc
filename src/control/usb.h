#pragma once

#include "core/clock.h"
#include "core/result.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <thread>

namespace tto {

struct UsbPaths {
    std::string devices = "/sys/bus/usb/devices";
    std::string hub_driver = "/sys/bus/usb/drivers/hub";
    std::string dev = "/dev/bus/usb";
};

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
Status rebind_hub(const std::string& hub_interface, const UsbPaths& paths);

class UsbRecovery {
public:
    bool observe(Nanos now, PortState state);
    void reset_done(Nanos now);
    void connected();
    int attempts() const { return attempts_; }
    static Nanos wait_for(int attempts);

private:
    bool stuck_ = false;
    bool empty_ = false;
    Nanos stuck_since_ = 0;
    Nanos empty_since_ = 0;
    int attempts_ = 0;
};

class UsbResetter {
public:
    UsbResetter() = default;
    ~UsbResetter();
    UsbResetter(const UsbResetter&) = delete;
    UsbResetter& operator=(const UsbResetter&) = delete;

    void begin(std::string hub_interface, UsbPaths paths);
    bool busy() const { return job_ != nullptr; }
    bool finished(Status& result, Nanos& took);
    bool wait(Nanos max);

private:
    struct Job {
        Status result;
        Nanos took = 0;
        std::atomic<bool> done{false};
    };
    bool running() const { return job_ && !job_->done.load(); }

    std::shared_ptr<Job> job_;
    std::thread thread_;
};

}
