#include <iostream>
#include <string>
#include <type_traits>

#include <libslic3r/GCode/GCodeProcessor.hpp>
#include <libslic3r/ConfigSDK.hpp>
#include <libslic3r/OrcaToolpathRecords.hpp>
#include <libslic3r/OrcaToolpathTypes.hpp>
#include <libslic3r/PrintConfig.hpp>
#include <libslic3r/Utils.hpp>
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

    Slic3r::set_resources_dir(TEST_LIBSLICER_RESOURCES_DIR);
    Slic3r::set_data_dir(".");

    const auto &defs = Slic3r::print_config_def;
    const Slic3r::libslicer::ConfigDefinition printer_model =
        Slic3r::libslicer::get_config_definition("printer_model");
    if (printer_model.key != "printer_model" ||
        printer_model.scope != Slic3r::libslicer::ConfigScope::Printer)
        return 7;

    libslicer::worker::WorkerEvent event;
    event.type = libslicer::worker::WorkerEventType::Progress;
    event.job_id = "add-subdirectory-smoke";
    event.percent = 1;
    const std::string line = libslicer::worker::event_to_json_line(event);
    if (line.find("\"type\":\"progress\"") == std::string::npos)
        return 2;

    libslicer::worker::preview::WireMoveRecord move;
    move.move_type = libslicer::worker::preview::MoveType::Extrude;
    move.path_kind = libslicer::worker::preview::PathKind::Linear_move;
    move.extrusion_role = Slic3r::erExternalPerimeter;
    move.flags = static_cast<std::uint32_t>(
        libslicer::worker::preview::MoveFlags::Drawable |
        libslicer::worker::preview::MoveFlags::ValidStartPosition |
        libslicer::worker::preview::MoveFlags::ValidEndPosition);
    if (!libslicer::worker::preview::move_is_drawable_segment(move))
        return 4;
    if (libslicer::worker::preview::extrusion_role_name(move) != "external_perimeter")
        return 5;
    static_assert(std::is_same_v<libslicer::worker::preview::MoveType, Slic3r::EMoveType>);
    static_assert(std::is_same_v<libslicer::worker::preview::PathKind, Slic3r::EMovePathType>);
    static_assert(std::is_same_v<libslicer::worker::preview::ExtrusionRole, Slic3r::ExtrusionRole>);
    static_assert(std::is_same_v<Slic3r::GCodeProcessorResult::MoveVertex, Slic3r::ToolpathMoveVertex>);
    Slic3r::ToolpathMoveVertex public_move;
    public_move.type = Slic3r::EMoveType::Extrude;
    public_move.extrusion_role = Slic3r::erExternalPerimeter;
    if (Slic3r::move_type_name(public_move.type) != "extrude")
        return 6;

    libslicer::worker::WorkerClient client([](const libslicer::worker::WorkerEvent&) {});
    if (client.running())
        return 3;

    std::cout << "options=" << defs.options.size() << '\n';
    return defs.options.empty() ? 1 : 0;
}
