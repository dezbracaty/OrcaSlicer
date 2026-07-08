#pragma once

#include "artifacts/OutputRequest.hpp"
#include "libslicer_worker/WorkerEvent.hpp"

#include <libslic3r/GCode/GCodeProcessor.hpp>
#include <libslic3r/PrintConfig.hpp>

#include <cstdint>
#include <cstddef>
#include <filesystem>
#include <string>

namespace libslicer::worker {
class CancellationToken;
}

namespace libslicer::worker::preview {

struct PreviewProducerContext {
    std::string job_id;
    std::filesystem::path request_path;
    std::filesystem::path working_dir;
    std::filesystem::path resources_dir;
    std::filesystem::path data_dir;
    std::filesystem::path artifacts_dir;
    std::string input_type;
    std::filesystem::path input_path;
    int plate_index { -1 };
    bool gcode_requested { false };
    std::filesystem::path gcode_path;
};

struct PreviewProducerResult {
    bool success { false };
    std::string code;
    std::string message;
    std::string transport { "file" };
    std::filesystem::path path;
    std::string shm_name;
    std::uint64_t size { 0 };
    int file_descriptor { -1 };
};

PreviewProducerResult produce_preview_artifact(const artifacts::PreviewArtifactOutput& request,
                                               const PreviewProducerContext& context,
                                               const Slic3r::DynamicPrintConfig& config,
                                               const Slic3r::GCodeProcessorResult& gcode_result,
                                               CancellationToken& cancellation);

WorkerEvent make_preview_ready_event(const std::string& job_id, const PreviewProducerResult& result);

void unlink_preview_shared_memory(const std::string& shm_name);

std::size_t sweep_stale_preview_shared_memory();

} // namespace libslicer::worker::preview
