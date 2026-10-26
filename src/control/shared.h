#pragma once

#include "config/config.h"
#include "core/clock.h"
#include "core/fd.h"
#include "core/spsc_ring.h"
#include "core/triple_buffer.h"
#include "input/tracker.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <netinet/in.h>

namespace tto {

constexpr size_t kMaxLeases = 8;
constexpr size_t kMaxSendTargets = kMaxDestinations + kMaxLeases;

struct DestSet {
    uint32_t n = 0;
    sockaddr_in addr[kMaxSendTargets] = {};
    bool plain[kMaxSendTargets] = {};
};

enum class DevState : uint8_t { starting, ok, not_connected, disabled, no_permission, grab_refused, read_error, not_touchscreen, restarting };

inline const char* state_name(DevState s) {
    switch (s) {
    case DevState::starting: return "starting";
    case DevState::restarting: return "restarting";
    case DevState::ok: return "ok";
    case DevState::not_connected: return "not connected";
    case DevState::disabled: return "disabled";
    case DevState::no_permission: return "no permission";
    case DevState::grab_refused: return "grab refused";
    case DevState::read_error: return "read error";
    case DevState::not_touchscreen: return "not a touchscreen";
    }
    return "?";
}

struct DevInfo {
    uint32_t cfg_gen = 0;
    DevState state = DevState::starting;
    bool connected = false;
    bool grabbed = false;
    bool enabled = true;
    int32_t slots = 0;
    int32_t matching = 0;
    float w_mm = 0;
    float h_mm = 0;
    float aspect = 0;
    char node[64] = {};
    char name[128] = {};
    char usb[192] = {};
    char port[64] = {};
    char usb_port[64] = {};
    char reason[320] = {};
};

struct ActiveSnap {
    int32_t frame = 0;
    int32_t n = 0;
    int32_t ids[kMaxSlots] = {};
};

enum class EventKind : uint8_t { device, resync, palm, grab, silence, enabled };

inline const char* event_kind_name(EventKind k) {
    switch (k) {
    case EventKind::device: return "device";
    case EventKind::resync: return "resync";
    case EventKind::palm: return "palm";
    case EventKind::grab: return "grab";
    case EventKind::silence: return "silence";
    case EventKind::enabled: return "enabled";
    }
    return "?";
}

enum class ReadMode : uint8_t { touches, identify, calibrate };

// a touch seen while touches are paused: where it landed, for identify and guided calibration
struct ModeTouch {
    int32_t contact = 0;
    bool down = true;
    float x = 0, y = 0;
    float u = 0, v = 0;
};

struct ReaderEvent {
    EventKind kind = EventKind::device;
    uint8_t level = 6;
    char text[320] = {};
};

struct DeviceShared {
    TripleBuffer<DestSet> dests;
    TripleBuffer<DevInfo> info;
    TripleBuffer<ActiveSnap> active;
    SpscRing<ReaderEvent, 64> events;
    SpscRing<uint32_t, 2048> latency_us;
    SpscRing<ModeTouch, 128> mode_touches;
    std::atomic<uint8_t> mode{0};
    std::atomic<bool> want_enabled{true};
    std::atomic<int32_t> silence_ms{0};
    std::atomic<bool> stop{false};
    std::atomic<bool> restarting{false};
    std::mutex cfg_mutex;
    DeviceConfig next_cfg;
    std::atomic<uint32_t> cfg_gen{0};
    std::atomic<int32_t> contacts{0};
    std::atomic<int64_t> oldest_down_ns{0};
    std::atomic<int64_t> last_frame_ns{0};
    std::atomic<uint64_t> frames{0};
    std::atomic<uint64_t> touches{0};
    std::atomic<uint64_t> resyncs{0};
    std::atomic<uint64_t> send_drops{0};
    std::atomic<uint64_t> cancels{0};
    std::atomic<uint64_t> silences{0};
    std::atomic<uint64_t> losses{0};
    std::atomic<int64_t> last_loss_ns{0};
    std::atomic<int64_t> last_back_ns{0};
    std::atomic<uint64_t> lost_events{0};
    std::atomic<uint64_t> touched_frames{0};
    std::atomic<int64_t> touched_ns{0};
    UniqueFd wake;
};

}
