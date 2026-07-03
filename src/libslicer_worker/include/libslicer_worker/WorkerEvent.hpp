#pragma once

#include <filesystem>
#include <functional>
#include <string>

namespace libslicer::worker {

enum class WorkerEventType {
    Hello,
    Listening,
    Accepted,
    Progress,
    Artifact,
    Warning,
    Error,
    Result,
    CancelAccepted,
    Stopping
};

struct WorkerEvent {
    WorkerEventType type { WorkerEventType::Progress };
    std::string job_id;
    int percent { -1 };
    std::string stage;
    std::string kind;
    std::string code;
    std::string message;
    std::filesystem::path path;
    bool success { false };
    bool recoverable { false };
    long long elapsed_ms { -1 };
    std::string phase;
    std::string schema;
    std::string format;
    std::string section;
    long long offset { -1 };
    long long count { -1 };
    long long record_size { -1 };
    bool complete { false };
};

using EventCallback = std::function<void(const WorkerEvent&)>;

} // namespace libslicer::worker
