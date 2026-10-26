#include "control/usb.h"

#include <doctest.h>

#include <atomic>
#include <cstdlib>
#include <fcntl.h>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

using namespace tto;

namespace {

struct TempDir {
    std::string path;
    TempDir() {
        char tmpl[] = "/tmp/tto-usb-XXXXXX";
        char* d = mkdtemp(tmpl);
        REQUIRE(d);
        path = d;
    }
    ~TempDir() {
        int rc = std::system(("rm -rf " + path).c_str());
        (void)rc;
    }
};

void touch(const std::string& path) { std::ofstream(path).put('\0'); }

std::string slurp(const std::string& path) {
    std::ifstream in(path);
    std::string s((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return s;
}

}

TEST_CASE("USB port paths name the hub that holds the port") {
    auto a = parse_usb_port("3-3");
    REQUIRE(a);
    CHECK(a->bus == 3);
    CHECK(a->port == 3);
    CHECK(a->hub == "usb3");
    CHECK(a->hub_interface == "3-0:1.0");
    auto b = parse_usb_port("3-1.2");
    REQUIRE(b);
    CHECK(b->hub == "3-1");
    CHECK(b->hub_interface == "3-1:1.0");
    CHECK(b->port == 2);
    auto c = parse_usb_port("1-1.4.3");
    REQUIRE(c);
    CHECK(c->hub == "1-1.4");
    CHECK(c->hub_interface == "1-1.4:1.0");
    CHECK(c->port == 3);
    for (const char* bad : {"", "3", "3-", "-3", "03-3", "3-0", "3-256", "a-b", "3-1..2", "3-1.", "tto-pa-123", "usb3", "3-1-2", "1-1.1.1.1.1.1.1.1"})
        CHECK_MESSAGE(!parse_usb_port(bad), bad);
}

TEST_CASE("recovery waits 10 s on a stuck port, then backs off, and never acts on an empty one") {
    UsbRecovery r;
    const Nanos s = kSec;
    CHECK_FALSE(r.observe(0, PortState::stuck));
    CHECK_FALSE(r.observe(9 * s, PortState::stuck));
    CHECK(r.observe(10 * s, PortState::stuck));
    r.reset_done(10 * s);
    CHECK(r.attempts() == 1);
    CHECK_FALSE(r.observe(29 * s, PortState::stuck));
    CHECK(r.observe(30 * s, PortState::stuck));
    r.reset_done(30 * s);
    CHECK_FALSE(r.observe(69 * s, PortState::stuck));
    CHECK(r.observe(70 * s, PortState::stuck));
    for (int i = 0; i < 10; ++i) r.reset_done(100 * s);
    CHECK(UsbRecovery::wait_for(r.attempts()) == 300 * s);
    CHECK(UsbRecovery::wait_for(2) == 40 * s);
    CHECK(UsbRecovery::wait_for(4) == 160 * s);
    for (Nanos t = 0; t < 1000 * s; t += s) CHECK_FALSE(r.observe(2000 * s + t, PortState::empty));
}

TEST_CASE("a moment of an empty port keeps the backoff; a real unplug forgets it; the screen coming back forgets it") {
    UsbRecovery r;
    const Nanos s = kSec;
    r.observe(0, PortState::stuck);
    r.reset_done(10 * s);
    r.reset_done(11 * s);
    CHECK(r.attempts() == 2);
    r.observe(12 * s, PortState::empty);
    r.observe(16 * s, PortState::empty);
    CHECK(r.attempts() == 2);
    r.observe(17 * s, PortState::empty);
    CHECK(r.attempts() == 0);
    r.reset_done(20 * s);
    r.observe(21 * s, PortState::enumerated);
    r.observe(22 * s, PortState::unknown);
    CHECK(r.attempts() == 1);
    r.connected();
    CHECK(r.attempts() == 0);
    CHECK_FALSE(r.observe(30 * s, PortState::stuck));
    CHECK(r.observe(40 * s, PortState::stuck));
}

TEST_CASE("another state between stuck readings starts the wait again") {
    UsbRecovery r;
    const Nanos s = kSec;
    r.observe(0, PortState::stuck);
    r.observe(8 * s, PortState::enumerated);
    CHECK_FALSE(r.observe(9 * s, PortState::stuck));
    CHECK_FALSE(r.observe(18 * s, PortState::stuck));
    CHECK(r.observe(19 * s, PortState::stuck));
    UsbRecovery k;
    k.observe(0, PortState::stuck);
    CHECK_FALSE(k.observe(9 * s, PortState::setting_up));
    CHECK_FALSE(k.observe(10 * s, PortState::stuck));
    CHECK_FALSE(k.observe(19 * s, PortState::stuck));
    CHECK(k.observe(20 * s, PortState::stuck));
}

TEST_CASE("the port status bits: a port the kernel is still setting up is not stuck") {
    CHECK(port_state_from(0x0000, false) == PortState::empty);
    CHECK(port_state_from(0x0100, false) == PortState::empty);
    CHECK(port_state_from(0x0101, false) == PortState::stuck);
    CHECK(port_state_from(0x0103, false) == PortState::setting_up);
    CHECK(port_state_from(0x0103, true) == PortState::enumerated);
    CHECK(port_state_from(0x0101, true) == PortState::enumerated);
    CHECK(port_state_from(0x0000, true) == PortState::enumerated);
}

TEST_CASE("port state from sysfs: a set-up device, and a hub that is not there") {
    TempDir dir;
    const std::string& root = dir.path;
    UsbPaths p;
    p.devices = root;
    p.dev = root + "/dev";
    auto ref = parse_usb_port("3-3");
    REQUIRE(ref);
    std::string why;
    CHECK(read_port_state(*ref, p, &why) == PortState::unknown);
    CHECK(why.find("usb3") != std::string::npos);
    mkdir((root + "/3-3").c_str(), 0755);
    CHECK(read_port_state(*ref, p, &why) == PortState::enumerated);
}

TEST_CASE("rebinding writes unbind then bind, and says so when the hub is not bound again") {
    TempDir dir;
    const std::string& root = dir.path;
    UsbPaths p;
    p.devices = root + "/devices";
    p.hub_driver = root + "/hub";
    mkdir(p.devices.c_str(), 0755);
    mkdir(p.hub_driver.c_str(), 0755);
    touch(p.hub_driver + "/unbind");
    touch(p.hub_driver + "/bind");
    mkdir((p.devices + "/3-0:1.0").c_str(), 0755);
    mkdir((p.devices + "/3-0:1.0/driver").c_str(), 0755);
    CHECK(rebind_hub("3-0:1.0", p));
    CHECK(slurp(p.hub_driver + "/unbind").find("3-0:1.0") == 0);
    CHECK(slurp(p.hub_driver + "/bind").find("3-0:1.0") == 0);
    rmdir((p.devices + "/3-0:1.0/driver").c_str());
    Status st = rebind_hub("3-0:1.0", p);
    REQUIRE_FALSE(st);
    CHECK(st.error().find("could not be bound again (the kernel did not take it)") != std::string::npos);
    CHECK(st.error().find("echo 3-0:1.0 > " + p.hub_driver + "/bind") != std::string::npos);
}

TEST_CASE("a reset leaves a marker naming the hub only while the hub is not bound again") {
    TempDir dir;
    const std::string& root = dir.path;
    UsbPaths p;
    p.devices = root + "/devices";
    p.hub_driver = root + "/hub";
    mkdir(p.devices.c_str(), 0755);
    mkdir(p.hub_driver.c_str(), 0755);
    touch(p.hub_driver + "/unbind");
    touch(p.hub_driver + "/bind");
    mkdir((p.devices + "/3-0:1.0").c_str(), 0755);
    mkdir((p.devices + "/3-0:1.0/driver").c_str(), 0755);
    std::string marker = root + "/state.toml.usb-reset";
    CHECK(rebind_hub("3-0:1.0", p, marker));
    CHECK(access(marker.c_str(), F_OK) != 0);
    rmdir((p.devices + "/3-0:1.0/driver").c_str());
    CHECK_FALSE(rebind_hub("3-0:1.0", p, marker));
    CHECK(slurp(marker) == "3-0:1.0\n");
}

TEST_CASE("a hub a cut-short reset left unbound is bound at start, and a bound one is left alone") {
    TempDir dir;
    const std::string& root = dir.path;
    UsbPaths p;
    p.devices = root + "/devices";
    p.hub_driver = root + "/hub";
    mkdir(p.devices.c_str(), 0755);
    mkdir(p.hub_driver.c_str(), 0755);
    mkdir((p.devices + "/3-0:1.0").c_str(), 0755);
    std::string marker = root + "/state.toml.usb-reset";
    std::string done;
    CHECK(bind_hub_left_unbound(marker, p, &done));
    CHECK(done.empty());
    std::ofstream(marker) << "3-0:1.0\n";
    touch(p.hub_driver + "/bind");
    Status st = bind_hub_left_unbound(marker, p, &done);
    REQUIRE_FALSE(st);
    CHECK(st.error().find("left unbound by a reset cut short") != std::string::npos);
    CHECK(st.error().find("echo 3-0:1.0 > " + p.hub_driver + "/bind") != std::string::npos);
    CHECK(slurp(p.hub_driver + "/bind").find("3-0:1.0") == 0);
    CHECK(access(marker.c_str(), F_OK) == 0);
    mkdir((p.devices + "/3-0:1.0/driver").c_str(), 0755);
    std::ofstream(p.hub_driver + "/bind", std::ios::trunc).flush();
    CHECK(bind_hub_left_unbound(marker, p, &done));
    CHECK(done.empty());
    CHECK(slurp(p.hub_driver + "/bind").empty());
    CHECK(access(marker.c_str(), F_OK) != 0);
    std::ofstream(marker) << "3-0:1.0 > /etc/x\n";
    CHECK(bind_hub_left_unbound(marker, p, &done));
    CHECK(slurp(p.hub_driver + "/bind").empty());
    CHECK(access(marker.c_str(), F_OK) != 0);
}

TEST_CASE("an unbind that fails while the hub stays bound is an error and nothing is bound") {
    TempDir dir;
    const std::string& root = dir.path;
    UsbPaths p;
    p.devices = root + "/devices";
    p.hub_driver = root + "/hub";
    mkdir(p.devices.c_str(), 0755);
    mkdir(p.hub_driver.c_str(), 0755);
    touch(p.hub_driver + "/bind");
    mkdir((p.devices + "/3-0:1.0").c_str(), 0755);
    mkdir((p.devices + "/3-0:1.0/driver").c_str(), 0755);
    Status st = rebind_hub("3-0:1.0", p);
    REQUIRE_FALSE(st);
    CHECK_MESSAGE(st.error().find("the USB hub 3-0:1.0 could not be reset (cannot open " + p.hub_driver + "/unbind") == 0, st.error());
    CHECK(slurp(p.hub_driver + "/bind").find("3-0:1.0") == std::string::npos);
}

TEST_CASE("a reset advances the wait of every screen watched on that hub, and only there") {
    constexpr Nanos s = kSec;
    std::vector<UsbRecovery> recs(4);
    std::vector<std::string> watched = {"3-0:1.0", "3-0:1.0", "1-0:1.0", ""};
    reset_done_on_hub(recs, watched, 0, "3-0:1.0", 10 * s);
    CHECK(recs[0].attempts() == 1);
    CHECK(recs[1].attempts() == 1);
    CHECK(recs[2].attempts() == 0);
    CHECK(recs[3].attempts() == 0);
    CHECK_FALSE(recs[1].observe(11 * s, PortState::stuck));
    CHECK_FALSE(recs[1].observe(29 * s, PortState::stuck));
    CHECK(recs[1].observe(30 * s, PortState::stuck));
    reset_done_on_hub(recs, watched, 3, "", 40 * s);
    CHECK(recs[3].attempts() == 1);
    CHECK(recs[0].attempts() == 1);
}

TEST_CASE("a finished reset counts as busy until its result is collected") {
    TempDir dir;
    const std::string& root = dir.path;
    UsbPaths p;
    p.devices = root + "/devices";
    p.hub_driver = root + "/hub";
    mkdir(p.devices.c_str(), 0755);
    mkdir(p.hub_driver.c_str(), 0755);
    touch(p.hub_driver + "/unbind");
    touch(p.hub_driver + "/bind");
    mkdir((p.devices + "/3-0:1.0").c_str(), 0755);
    mkdir((p.devices + "/3-0:1.0/driver").c_str(), 0755);
    UsbWorker w(p);
    CHECK_FALSE(w.busy());
    w.reset(*parse_usb_port("3-3"), 5);
    CHECK(w.resetting());
    CHECK(w.busy_since() == 5);
    CHECK(w.wait(2 * kSec));
    CHECK(w.busy());
    UsbWorker::Done done;
    done.took = -1;
    REQUIRE(w.finished(done));
    CHECK(done.reset);
    CHECK(done.result);
    CHECK(done.took >= 0);
    CHECK_FALSE(w.busy());
    CHECK_FALSE(w.finished(done));
}

TEST_CASE("a port check that had to wait for the kernel reads as being set up") {
    auto ref = parse_usb_port("3-3");
    REQUIRE(ref);
    auto run = [&](PortState found, Nanos delay) {
        UsbWorker w(UsbPaths(), [found, delay](const UsbPortRef&, std::string*) {
            usleep(useconds_t(delay / 1000));
            return found;
        });
        w.check({{size_t(1), *ref}}, 0);
        REQUIRE(w.wait(3 * kSec));
        UsbWorker::Done done;
        REQUIRE(w.finished(done));
        CHECK_FALSE(done.reset);
        REQUIRE(done.readings.size() == 1);
        CHECK(done.readings[0].device == 1);
        CHECK(done.readings[0].port == "3-3");
        return done.readings[0].state;
    };
    CHECK(run(PortState::stuck, 0) == PortState::stuck);
    CHECK(run(PortState::stuck, 300 * kMs) == PortState::setting_up);
    CHECK(run(PortState::empty, 300 * kMs) == PortState::empty);
    CHECK(run(PortState::enumerated, 300 * kMs) == PortState::enumerated);
}

TEST_CASE("a reset held up in the kernel does not hold up stopping") {
    TempDir dir;
    const std::string& root = dir.path;
    UsbPaths p;
    p.devices = root + "/devices";
    p.hub_driver = root + "/hub";
    mkdir(p.devices.c_str(), 0755);
    mkdir(p.hub_driver.c_str(), 0755);
    std::string unbind = p.hub_driver + "/unbind";
    REQUIRE(mkfifo(unbind.c_str(), 0600) == 0);
    std::atomic<bool> waited{true}, gone{false};
    std::thread stopping([&] {
        {
            UsbWorker w(p);
            w.reset(*parse_usb_port("3-3"), 0);
            waited = w.wait(100 * kMs);
        }
        gone = true;
    });
    for (int i = 0; i < 200 && !gone; ++i) usleep(10000);
    CHECK_FALSE(waited.load());
    CHECK(gone.load());
    int fd = open(unbind.c_str(), O_RDONLY | O_NONBLOCK);
    stopping.join();
    usleep(800000);
    if (fd >= 0) close(fd);
}
