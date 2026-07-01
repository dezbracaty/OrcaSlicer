#pragma once

#include "libslicer_worker/SliceJob.hpp"
#include "libslicer_worker/WorkerEvent.hpp"

#include <chrono>
#include <filesystem>
#include <string>

namespace libslicer::worker {

struct WorkerOptions {
    std::filesystem::path executable_path;
    std::filesystem::path working_dir;
    std::filesystem::path socket_path;
    std::chrono::milliseconds startup_timeout { 5000 };
};

class WorkerClient {
public:
    explicit WorkerClient(EventCallback on_event);
    ~WorkerClient();

    WorkerClient(const WorkerClient&) = delete;
    WorkerClient& operator=(const WorkerClient&) = delete;
    WorkerClient(WorkerClient&& other) noexcept;
    WorkerClient& operator=(WorkerClient&& other) noexcept;

    bool start(const WorkerOptions& options);
    bool connect();
    bool submit(const SliceJob& job);
    bool cancel(const std::string& job_id);
    bool stop();
    void kill();

    bool running() const;
    std::string last_error() const;

private:
    struct Impl;
    Impl* m_impl { nullptr };
};

} // namespace libslicer::worker
