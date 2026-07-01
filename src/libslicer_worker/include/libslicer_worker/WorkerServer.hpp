#pragma once

#include "libslicer_worker/SliceJob.hpp"
#include "libslicer_worker/WorkerEvent.hpp"

#include <filesystem>

namespace libslicer::worker {

struct ServerOptions {
    std::filesystem::path socket_path;
};

int run_slice_job_from_request(const std::filesystem::path& request_path,
                               const EventCallback& events,
                               CancellationToken& cancellation);

int run_worker_server(const ServerOptions& options);

} // namespace libslicer::worker

