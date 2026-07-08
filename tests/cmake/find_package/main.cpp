#include <filesystem>
#include <algorithm>
#include <iostream>
#include <string>
#include <type_traits>
#include <vector>

#include <libslic3r/ConfigSDK.hpp>
#include <libslic3r/OrcaToolpathRecords.hpp>
#include <libslic3r/OrcaToolpathTypes.hpp>
#include <libslicer_worker/PreviewProtocol.hpp>
#include <libslicer_worker/WorkerClient.hpp>
#include <libslicer_worker/WorkerEvent.hpp>
#include <libslicer_worker/WorkerProtocol.hpp>

int main()
{
    static_assert(!std::is_copy_constructible_v<libslicer::worker::WorkerClient>);
    static_assert(!std::is_copy_assignable_v<libslicer::worker::WorkerClient>);
    static_assert(std::is_move_constructible_v<libslicer::worker::WorkerClient>);
    static_assert(std::is_move_assignable_v<libslicer::worker::WorkerClient>);

    libslicer::worker::WorkerEvent event;
    event.type = libslicer::worker::WorkerEventType::Progress;
    event.job_id = "find-package-smoke";
    event.percent = 50;

    const std::string line = libslicer::worker::event_to_json_line(event);
    if (line.find("\"type\":\"progress\"") == std::string::npos) {
        std::cerr << "worker protocol did not serialize a progress event\n";
        return 1;
    }

    if (!std::filesystem::exists(LIBSLICER_WORKER_EXE)) {
        std::cerr << "worker executable does not exist: " << LIBSLICER_WORKER_EXE << '\n';
        return 2;
    }

    libslicer::worker::preview::WireMoveRecord move;
    move.move_type = libslicer::worker::preview::MoveType::Travel;
    move.path_kind = libslicer::worker::preview::PathKind::Linear_move;
    move.flags = static_cast<std::uint32_t>(
        libslicer::worker::preview::MoveFlags::Drawable |
        libslicer::worker::preview::MoveFlags::ValidStartPosition |
        libslicer::worker::preview::MoveFlags::ValidEndPosition);
    const auto segment = libslicer::worker::preview::make_render_segment_view(move);
    if (segment.move_type != libslicer::worker::preview::MoveType::Travel) {
        std::cerr << "preview protocol segment view lost move type\n";
        return 4;
    }
    static_assert(std::is_same_v<libslicer::worker::preview::MoveType, Slic3r::EMoveType>);
    static_assert(std::is_same_v<libslicer::worker::preview::PathKind, Slic3r::EMovePathType>);
    static_assert(std::is_same_v<libslicer::worker::preview::ExtrusionRole, Slic3r::ExtrusionRole>);
    Slic3r::ToolpathMoveVertex public_move;
    public_move.type = Slic3r::EMoveType::Travel;
    if (Slic3r::move_type_name(public_move.type) != "travel") {
        std::cerr << "public toolpath move type helper failed\n";
        return 5;
    }

    libslicer::worker::WorkerClient client([](const libslicer::worker::WorkerEvent&) {});
    if (client.running()) {
        std::cerr << "new worker client should not be running\n";
        return 3;
    }

    // Exercise the DTO-only ConfigSDK through the shared library. This TU only
    // includes <libslic3r/ConfigSDK.hpp> — no Orca internal headers such as
    // PrintConfig.hpp are installed, so this also proves the header is DTO-clean.
    if (Slic3r::libslicer::config_sdk_version().empty()) {
        std::cerr << "config_sdk_version() returned empty\n";
        return 6;
    }
    if (Slic3r::libslicer::config_sdk_capabilities().empty()) {
        std::cerr << "config_sdk_capabilities() returned empty\n";
        return 7;
    }
    const std::vector<std::string> capabilities = Slic3r::libslicer::config_sdk_capabilities();
    if (std::find(capabilities.begin(), capabilities.end(), "preset_catalog.v1") == capabilities.end()) {
        std::cerr << "preset_catalog.v1 capability is missing\n";
        return 10;
    }
    if (std::find(capabilities.begin(), capabilities.end(), "project_3mf_config.v1") == capabilities.end()) {
        std::cerr << "project_3mf_config.v1 capability is missing\n";
        return 12;
    }
    const std::vector<Slic3r::libslicer::ConfigDefinition> definitions =
        Slic3r::libslicer::list_config_definitions();
    if (definitions.empty()) {
        std::cerr << "list_config_definitions() returned empty\n";
        return 8;
    }
    // Empty requests must fail through structured issues, never by throwing or
    // silently selecting defaults.
    const Slic3r::libslicer::ConfigResolutionResult resolution =
        Slic3r::libslicer::resolve_fff_config(Slic3r::libslicer::ConfigResolutionRequest {});
    if (!Slic3r::libslicer::has_config_errors(resolution.issues)) {
        std::cerr << "resolve_fff_config() did not report missing required fields\n";
        return 9;
    }
    const Slic3r::libslicer::PresetCatalogResult catalog =
        Slic3r::libslicer::load_preset_catalog(Slic3r::libslicer::PresetCatalogRequest {});
    if (!Slic3r::libslicer::has_config_errors(catalog.issues)) {
        std::cerr << "load_preset_catalog() did not report missing required fields\n";
        return 11;
    }
    const Slic3r::libslicer::Project3mfExtractionResult project_3mf =
        Slic3r::libslicer::extract_project_3mf_config(Slic3r::libslicer::Project3mfExtractionRequest {});
    if (!Slic3r::libslicer::has_config_errors(project_3mf.issues)) {
        std::cerr << "extract_project_3mf_config() did not report missing required fields\n";
        return 13;
    }

    std::cout << "worker=" << LIBSLICER_WORKER_EXE << '\n';
    std::cout << "config_sdk=" << Slic3r::libslicer::config_sdk_version()
              << " definitions=" << definitions.size() << '\n';
    return 0;
}
