#include "config/atomic_file.h"
#include "config/file_writer.h"
#include "config/state.h"

#include <doctest.h>

#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

using namespace tto;

namespace {

struct TempDir {
    std::string path;
    TempDir() {
        char tmpl[] = "/tmp/tto-state-XXXXXX";
        char* d = mkdtemp(tmpl);
        REQUIRE(d);
        path = d;
    }
    ~TempDir() {
        int rc = std::system(("rm -rf " + path).c_str());
        (void)rc;
    }
};

std::string slurp(const std::string& path) {
    std::ifstream in(path);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

}

TEST_CASE("what touch-to-osc keeps between runs comes back the same") {
    State s;
    s.usb_ports["screen"] = "3-3";
    s.usb_ports["back"] = "1-1.4";
    s.config_path = "/etc/touch-to-osc/touch-to-osc.toml";
    s.config_text = "# a \"quoted\" comment\n[osc]\nallowed_ips = ['127.0.0.1'] # tab\there, a backslash \\ and \xc3\xa9\n";
    s.config_saved = "2026-10-25 10:00:00";
    std::string problem;
    State back = parse_state(state_text(s), "state.toml", &problem);
    CHECK(problem.empty());
    CHECK(back.usb_ports == s.usb_ports);
    CHECK(back.config_path == s.config_path);
    CHECK(back.config_text == s.config_text);
    CHECK(back.config_saved == s.config_saved);
}

TEST_CASE("a damaged state file is set aside with a note, and odd entries are dropped") {
    std::string problem;
    State s = parse_state("[usb_ports\nface = ", "state.toml", &problem);
    CHECK(s.usb_ports.empty());
    CHECK(problem.find("state.toml cannot be read back") == 0);
    problem.clear();
    s = parse_state("[usb_ports]\nface = 3\nback = \"3-4\"\n\"bad\\u0001id\" = \"3-5\"\n[config_that_worked]\npath = \"/x.toml\"\n", "state.toml", &problem);
    CHECK(problem.empty());
    CHECK(s.usb_ports.size() == 1);
    CHECK(s.usb_ports["back"] == "3-4");
    CHECK(s.config_path.empty());
}

TEST_CASE("the file writer writes, replaces a pending write, removes, and reports each outcome") {
    TempDir dir;
    FileWriter w;
    std::string a = dir.path + "/a.toml";
    w.write(a, "one");
    w.write(a, "two");
    REQUIRE(w.wait_idle(5 * kSec));
    auto out = w.outcomes();
    REQUIRE(!out.empty());
    CHECK(out.back().path == a);
    CHECK(out.back().result);
    CHECK(slurp(a) == "two");
    w.remove(a);
    REQUIRE(w.wait_idle(5 * kSec));
    out = w.outcomes();
    REQUIRE(out.size() == 1);
    CHECK(out[0].removed);
    CHECK(out[0].result);
    CHECK(access(a.c_str(), F_OK) != 0);
    w.write(dir.path + "/no/such/dir/b.toml", "x");
    REQUIRE(w.wait_idle(5 * kSec));
    out = w.outcomes();
    REQUIRE(out.size() == 1);
    CHECK_FALSE(out[0].result);
    CHECK(out[0].result.error().find("cannot create") == 0);
}

TEST_CASE("a write stuck on the disk does not hold up the writer's owner going away") {
    TempDir dir;
    std::string a = dir.path + "/stuck.toml";
    std::string tmp = a + ".tmp." + std::to_string(getpid());
    REQUIRE(mkfifo(tmp.c_str(), 0600) == 0);
    Nanos t0 = monotonic_ns();
    {
        FileWriter w;
        w.write(a, "content");
        CHECK_FALSE(w.wait_idle(200 * kMs));
    }
    CHECK(monotonic_ns() - t0 < 2 * kSec);
    int fd = open(tmp.c_str(), O_RDONLY | O_NONBLOCK);
    usleep(300000);
    if (fd >= 0) close(fd);
    usleep(200000);
}

TEST_CASE("a config with quotes in threes, blank first lines or Windows line ends comes back from the state file byte for byte") {
    for (const char* text : {"# see ''' below\n[osc]\n", "label = '''Face'''\n", "\n\n[osc]\nallowed_ips = ['127.0.0.1']\n", "[osc]\r\nprefix = \"/t\"\r\n",
                             "a = \"\"\"x\"\"\"\n", "# ends without a newline", "'''", "\t# \\ \" ' \xc3\xa9\n"}) {
        State s;
        s.config_path = "/etc/touch-to-osc/touch-to-osc.toml";
        s.config_text = text;
        s.config_saved = "2026-11-25 09:00:00";
        s.usb_ports["screen"] = "3-1.4";
        std::string problem;
        State back = parse_state(state_text(s), "state.toml", &problem);
        INFO(text);
        CHECK(problem.empty());
        CHECK(back.config_text == s.config_text);
        CHECK(back.usb_ports == s.usb_ports);
    }
}

TEST_CASE("removing a file is an outcome like a write, and a missing file is not a failure") {
    TempDir dir;
    std::string a = dir.path + "/a.toml";
    std::ofstream(a) << "x";
    CHECK(remove_file_durable(a));
    CHECK(access(a.c_str(), F_OK) != 0);
    CHECK(remove_file_durable(a));
    CHECK_FALSE(remove_file_durable(dir.path));
}

TEST_CASE("the leftovers of saves cut short are removed, and nothing else") {
    TempDir dir;
    std::string a = dir.path + "/settings.toml";
    for (const char* name : {"/settings.toml", "/settings.toml.tmp.123", "/settings.toml.tmp.99999", "/settings.toml.tmp.", "/settings.toml.tmp.12a",
                             "/settings.toml.refused", "/state.toml.tmp.5", "/xsettings.toml.tmp.5"})
        std::ofstream(dir.path + name) << "x";
    CHECK(remove_stale_temp_files(a) == 2);
    CHECK(access((a + ".tmp.123").c_str(), F_OK) != 0);
    CHECK(access((a + ".tmp.99999").c_str(), F_OK) != 0);
    for (const char* kept : {"/settings.toml", "/settings.toml.tmp.", "/settings.toml.tmp.12a", "/settings.toml.refused", "/state.toml.tmp.5", "/xsettings.toml.tmp.5"})
        CHECK_MESSAGE(access((dir.path + kept).c_str(), F_OK) == 0, kept);
    CHECK(remove_stale_temp_files(a) == 0);
    CHECK(remove_stale_temp_files("") == 0);
    CHECK(remove_stale_temp_files(dir.path + "/no/such/dir/x.toml") == 0);
}

TEST_CASE("a save through a symlink planted at the temporary name is refused, not followed") {
    TempDir dir;
    std::string a = dir.path + "/a.toml", victim = dir.path + "/victim";
    std::ofstream(victim) << "keep";
    REQUIRE(symlink(victim.c_str(), (a + ".tmp." + std::to_string(getpid())).c_str()) == 0);
    CHECK_FALSE(write_file_atomic(a, "new"));
    CHECK(slurp(victim) == "keep");
}
