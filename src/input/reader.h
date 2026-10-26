#pragma once

#include "config/config.h"
#include "control/shared.h"
#include "core/fd.h"
#include "core/log.h"
#include "input/mapping.h"
#include "input/probe.h"
#include "input/tracker.h"
#include "osc/osc.h"

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
    void set_devices(std::vector<DeviceConfig> devices);
    void set_device(size_t index, const DeviceConfig& d);
    const NodeInfo* pick(size_t index, const std::vector<NodeInfo>& nodes);

private:
    std::mutex m_;
    std::set<std::string> held_;
    std::vector<DeviceConfig> devices_;
};

bool port_order(const NodeInfo& a, const NodeInfo& b);

class DeviceReader {
public:
    DeviceReader(DeviceConfig cfg, size_t index, const std::string& prefix, DeviceShared& shared, int notify_fd, NodeClaims& claims, SysPaths paths);
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
    int discard_events();
    void on_frame(const input_event& ev);
    void on_resync();
    void on_released();
    void read_raw();
    int cancel(const char* reason);
    void send(int n, Nanos frame_t);
    bool encode(osc::Writer& w, int n, int32_t frame, int64_t t_us, bool plain);
    void publish_active();
    void apply_enabled();
    void apply_mode();
    void mode_frame(int n);
    bool retry_grab();
    bool foreign_grab();
    void pull_config();
    bool reconfigure();
    void recover(const char* what);
    void set_state(DevState st, const std::string& reason);
    void publish_info();
    void event(EventKind kind, log::Level level, const char* fmt, ...) __attribute__((format(printf, 4, 5)));
    void notify();
    void close_device();
    bool silent(Nanos now) const;

    DeviceConfig cfg_;
    size_t index_;
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
    Axis ax_;
    Axis ay_;
    bool clock_monotonic_ = true;
    uint64_t session_frames_ = 0;
    Nanos eacces_since_ = 0;
    bool enabled_ = true;
    bool raw_touched_ = false;
    Nanos prev_frame_ = 0;
    Nanos last_frame_ = 0;
    int32_t frame_ = 0;
    uint32_t cfg_gen_ = 0;
    DevState base_state_ = DevState::starting;
    std::string base_reason_;
    DevInfo info_;
    bool warned_ambiguous_ = false;
    bool quiet_check_ = false;
    bool hinted_quiet_ = false;
    bool hinted_aspect_ = false;
    ReadMode mode_ = ReadMode::touches;
    int32_t quiet_tracking_[kMaxSlots] = {};
    std::string addr_down_, addr_move_, addr_up_, addr_cancel_;
    TouchEvent evs_[kMaxFrameEvents];
    // a full frame of the largest message (64-character prefix, 32-character id, a cancel) fits: 128 x 152 bytes + the header
    uint8_t buf_[32768];
    uint8_t plain_buf_[32768];
};

}
