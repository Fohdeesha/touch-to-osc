#include "config/file_writer.h"

#include "config/atomic_file.h"
#include "core/process.h"

#include <cerrno>
#include <unistd.h>

namespace tto {

FileWriter::FileWriter() : sh_(std::make_shared<Shared>()) {}

void FileWriter::start() {
    if (!thread_.joinable()) thread_ = std::thread(loop, sh_);
}

FileWriter::~FileWriter() {
    if (!thread_.joinable()) return;
    bool busy;
    {
        std::lock_guard<std::mutex> lock(sh_->m);
        sh_->stop = true;
        busy = sh_->busy || !sh_->pending.empty();
    }
    sh_->cv.notify_all();
    if (busy) thread_.detach();
    else thread_.join();
}

void FileWriter::write(const std::string& path, std::string contents) {
    start();
    {
        std::lock_guard<std::mutex> lock(sh_->m);
        sh_->pending[path] = Job{false, std::move(contents)};
    }
    sh_->cv.notify_all();
}

void FileWriter::remove(const std::string& path) {
    start();
    {
        std::lock_guard<std::mutex> lock(sh_->m);
        sh_->pending[path] = Job{true, std::string()};
    }
    sh_->cv.notify_all();
}

std::vector<FileWriter::Outcome> FileWriter::outcomes() {
    std::lock_guard<std::mutex> lock(sh_->m);
    std::vector<Outcome> out;
    out.swap(sh_->done);
    return out;
}

bool FileWriter::idle() const {
    std::lock_guard<std::mutex> lock(sh_->m);
    return sh_->pending.empty() && !sh_->busy;
}

bool FileWriter::wait_idle(Nanos max) const {
    Nanos until = monotonic_ns() + max;
    while (!idle()) {
        if (monotonic_ns() >= until) return false;
        usleep(10000);
    }
    return true;
}

void FileWriter::loop(std::shared_ptr<Shared> sh) {
    set_thread_name("files");
    std::unique_lock<std::mutex> lock(sh->m);
    for (;;) {
        sh->cv.wait(lock, [&] { return sh->stop || !sh->pending.empty(); });
        if (sh->pending.empty()) return;
        auto it = sh->pending.begin();
        std::string path = it->first;
        Job job = std::move(it->second);
        sh->pending.erase(it);
        sh->busy = true;
        lock.unlock();
        Outcome o;
        o.path = path;
        o.removed = job.remove;
        if (job.remove) {
            if (unlink(path.c_str()) != 0 && errno != ENOENT) o.result = err("removing " + path + " failed: " + errno_str());
        } else {
            o.result = write_file_atomic(path, job.contents);
        }
        lock.lock();
        sh->busy = false;
        sh->done.push_back(std::move(o));
    }
}

}
