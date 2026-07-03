#include <filesystem>
#include <iostream>
#include <type_traits>

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

    std::cout << "worker=" << LIBSLICER_WORKER_EXE << '\n';
    return 0;
}
