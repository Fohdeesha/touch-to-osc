#pragma once

#include "config/config.h"
#include "control/shared.h"
#include "core/fd.h"
#include "core/log.h"
#include "input/mapping.h"
#include "input/probe.h"
#include "input/tracker.h"

#include <linux/input.h>
#include <mutex>
#include <set>
#include <string>
#include <thread>

struct libevdev;

namespace tto {

class NodeClaims {
public:
    bool claim(const std::string& sysname);
    void release(const std::string& sysname);
    bool claimed(const std::string& sysname);

private:
    std::mutex m_;
    std::set<std::string> held_;
};

class DeviceReader {
public:
    DeviceReader(DeviceConfig cfg, const std::string& prefix, DeviceShared& shared, int notify_fd, NodeClaims& claims, SysPaths paths);
    ~DeviceReader();
    DeviceReader(const DeviceReader&) = delete;
    DeviceReader& operator=(const DeviceReader&) = delete;

    void start();
    void join();

private:
    void run();
    void wait_for_change(int timeout_ms);
    bool open_device();
    void session();
    int drain();
    void on_frame(const input_event& ev);
    void on_resync();
    void on_released();
    void read_raw();
    int cancel(const char* reason);
    void send(int n, Nanos frame_t);
    void publish_active();
    void apply_enabled();
    void retry_grab();
    void set_state(DevState st, const std::string& reason);
    void publish_info();
    void event(EventKind kind, log::Level level, const char* fmt, ...) __attribute__((format(printf, 4, 5)));
    void notify();
    void close_device();
    bool silent(Nanos now) const;

    DeviceConfig cfg_;
    std::string prefix_;
    DeviceShared& sh_;
    int notify_fd_;
    NodeClaims& claims_;
    SysPaths paths_;
    std::thread thread_;
    UniqueFd sock_;
    UniqueFd inotify_;

    UniqueFd fd_;
    libevdev* dev_ = nullptr;
    std::string sysname_;
    std::string node_;
    TouchKind kind_ = TouchKind::none;
    bool has_tool_ = false;
    bool single_down_ = false;
    int32_t single_seq_ = 0;
    ContactTracker tracker_;
    Mapping mapping_;
    bool enabled_ = true;
    bool raw_touched_ = false;
    Nanos prev_frame_ = 0;
    Nanos last_frame_ = 0;
    int32_t frame_ = 0;
    DevState base_state_ = DevState::starting;
    std::string base_reason_;
    DevInfo info_;
    bool warned_ambiguous_ = false;
    std::string addr_down_, addr_move_, addr_up_, addr_cancel_;
    TouchEvent evs_[kMaxFrameEvents];
    uint8_t buf_[16384];
};

}
