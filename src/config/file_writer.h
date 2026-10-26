#pragma once

#include "core/clock.h"
#include "core/result.h"

#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace tto {

class FileWriter {
public:
    struct Outcome {
        std::string path;
        bool removed = false;
        Status result;
    };

    FileWriter();
    ~FileWriter();
    FileWriter(const FileWriter&) = delete;
    FileWriter& operator=(const FileWriter&) = delete;

    void write(const std::string& path, std::string contents);
    void remove(const std::string& path);
    std::vector<Outcome> outcomes();
    bool idle() const;
    bool wait_idle(Nanos max) const;

private:
    struct Job {
        bool remove = false;
        std::string contents;
    };
    struct Shared {
        mutable std::mutex m;
        std::condition_variable cv;
        std::map<std::string, Job> pending;
        std::vector<Outcome> done;
        bool busy = false;
        bool stop = false;
    };
    void start();
    static void loop(std::shared_ptr<Shared> sh);

    std::shared_ptr<Shared> sh_;
    std::thread thread_;
};

}
