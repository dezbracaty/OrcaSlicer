#pragma once

#include "libslicer_worker/WorkerEvent.hpp"

#include <optional>
#include <string>

namespace libslicer::worker {

inline constexpr int WORKER_PROTOCOL_VERSION = 1;
inline constexpr int WORKER_JOB_REQUEST_VERSION = 1;

std::string event_to_json_line(const WorkerEvent& event);
std::optional<WorkerEvent> event_from_json_line(const std::string& line);

} // namespace libslicer::worker
