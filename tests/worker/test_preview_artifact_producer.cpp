#include "preview/PreviewArtifactProducer.hpp"

#include "libslicer_worker/PreviewReader.hpp"
#include "libslicer_worker/SliceJob.hpp"
#include "libslicer_worker/WorkerProtocol.hpp"

#include <catch2/catch_all.hpp>
#include <nlohmann/json.hpp>

#include <array>
#include <cerrno>
#include <cmath>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace {

using namespace libslicer::worker;
using namespace libslicer::worker::preview;

artifacts::PreviewArtifactOutput preview_output(const std::filesystem::path& path)
{
    artifacts::PreviewArtifactOutput output;
    output.enabled = true;
    output.required = true;
    output.path = path;
    output.format = std::string(binary_format_name);
    output.publish = "final";
    return output;
}

artifacts::PreviewArtifactOutput shared_memory_preview_output(const std::filesystem::path& path)
{
    artifacts::PreviewArtifactOutput output = preview_output(path);
    output.transport = "shared_memory";
    return output;
}

artifacts::PreviewArtifactOutput shared_memory_fd_preview_output(const std::filesystem::path& path)
{
    artifacts::PreviewArtifactOutput output = preview_output(path);
    output.transport = "shared_memory_fd";
    return output;
}

PreviewProducerContext preview_context(const std::filesystem::path& root)
{
    PreviewProducerContext context;
    context.job_id = "preview-producer-test";
    context.working_dir = root;
    context.data_dir = root / "data";
    context.artifacts_dir = root;
    context.input_type = "stl";
    context.input_path = root / "input.stl";
    return context;
}

Slic3r::GCodeProcessorResult::PreviewColorFact color_fact(std::uint16_t id,
                                                           std::uint16_t filament_id,
                                                           Slic3r::GCodeProcessorResult::PreviewColorSource source,
                                                           std::array<float, 4> rgba,
                                                           std::string name)
{
    Slic3r::GCodeProcessorResult::PreviewColorFact fact;
    fact.id = id;
    fact.filament_id = filament_id;
    fact.source = source;
    fact.color_rgba = rgba;
    fact.name = std::move(name);
    return fact;
}

Slic3r::ToolpathMoveVertex move(unsigned int gcode_id,
                                unsigned int layer_id,
                                std::uint8_t filament_id,
                                std::uint16_t color_id,
                                Slic3r::EMoveType type,
                                const Slic3r::Vec3f& position)
{
    Slic3r::ToolpathMoveVertex out;
    out.gcode_id = gcode_id;
    out.layer_id = layer_id;
    out.extruder_id = filament_id;
    out.cp_color_id = static_cast<unsigned char>(color_id);
    out.type = type;
    out.extrusion_role = Slic3r::erPerimeter;
    out.position = position;
    out.feedrate = 30.0f;
    out.width = 0.42f;
    out.height = 0.2f;
    out.mm3_per_mm = type == Slic3r::EMoveType::Extrude ? 0.08f : 0.0f;
    out.delta_extruder = type == Slic3r::EMoveType::Extrude ? 0.1f : 0.0f;
    out.print_z = position.z();
    return out;
}

void make_result(Slic3r::GCodeProcessorResult& result,
                 const std::vector<int>& filament_to_tool_map,
                 bool include_color_change = false)
{
    result.filaments_count = filament_to_tool_map.size();
    result.filament_to_tool_map = filament_to_tool_map;
    result.filament_maps = filament_to_tool_map;
    result.extruder_colors.resize(filament_to_tool_map.size());
    result.filament_diameters.assign(filament_to_tool_map.size(), 1.75f);
    result.filament_densities.assign(filament_to_tool_map.size(), 1.24f);
    result.filament_costs.assign(filament_to_tool_map.size(), 20.0f);

    for (std::size_t i = 0; i < filament_to_tool_map.size(); ++i) {
        const float channel = static_cast<float>(i + 1) / static_cast<float>(filament_to_tool_map.size() + 1);
        result.extruder_colors[i] = i == 0 ? "#112233" : "#445566";
        result.preview_colors[static_cast<std::uint16_t>(i)] = color_fact(
            static_cast<std::uint16_t>(i),
            static_cast<std::uint16_t>(i),
            Slic3r::GCodeProcessorResult::PreviewColorSource::Filament,
            { channel, 0.25f, 0.75f, 1.0f },
            result.extruder_colors[i]);
    }

    result.moves.push_back(move(0, 0, 0, 0, Slic3r::EMoveType::Travel, { 0.0f, 0.0f, 0.2f }));
    result.moves.push_back(move(1, 0, 0, 0, Slic3r::EMoveType::Extrude, { 1.0f, 0.0f, 0.2f }));
    if (filament_to_tool_map.size() > 1) {
        const std::uint16_t color_id = include_color_change ? 42 : 1;
        if (include_color_change) {
            result.preview_colors[color_id] = color_fact(
                color_id,
                1,
                Slic3r::GCodeProcessorResult::PreviewColorSource::ColorChange,
                { 0.9f, 0.1f, 0.2f, 1.0f },
                "#E61933");
        }
        result.moves.push_back(move(2, 0, 1, color_id, Slic3r::EMoveType::Tool_change, { 1.0f, 0.0f, 0.2f }));
        result.moves.push_back(move(3, 0, 1, color_id, Slic3r::EMoveType::Extrude, { 2.0f, 0.0f, 0.2f }));
    }
}

PreviewArtifactView write_and_read(const std::vector<int>& filament_to_tool_map,
                                   bool include_color_change,
                                   PreviewArtifactStorage& storage)
{
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "libslicer-preview-producer-test";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);

    Slic3r::DynamicPrintConfig config = Slic3r::DynamicPrintConfig::full_print_config();
    Slic3r::GCodeProcessorResult result;
    make_result(result, filament_to_tool_map, include_color_change);
    CancellationToken cancellation;
    const std::filesystem::path path = root / "preview.orcapv";
    PreviewProducerResult produced = produce_preview_artifact(
        preview_output(path), preview_context(root), config, result, cancellation);
    REQUIRE(produced.success);
    REQUIRE(produced.path == path);

    storage.load(path);
    return storage.view();
}

void assert_mapping(const PreviewArtifactView& view, const std::vector<int>& expected_map)
{
    std::map<std::uint16_t, std::uint16_t> filament_to_tool;
    for (const WireFilamentRecord& filament : view.records<WireFilamentRecord>(WireSectionType::Filaments))
        filament_to_tool[filament.id] = filament.tool_id;

    REQUIRE(filament_to_tool.size() == expected_map.size());
    for (std::size_t filament_id = 0; filament_id < expected_map.size(); ++filament_id)
        CHECK(filament_to_tool[static_cast<std::uint16_t>(filament_id)] == expected_map[filament_id]);

    for (const WireMoveRecord& move : view.records<WireMoveRecord>(WireSectionType::Moves)) {
        REQUIRE(move.filament_id < expected_map.size());
        CHECK(move.tool_id == expected_map[move.filament_id]);
    }
}

void setup_single_filament(Slic3r::GCodeProcessorResult& result)
{
    result.filaments_count = 1;
    result.filament_to_tool_map = { 0 };
    result.filament_maps = { 0 };
    result.extruder_colors = { "#112233" };
    result.filament_diameters = { 1.75f };
    result.filament_densities = { 1.24f };
    result.filament_costs = { 20.0f };
    result.preview_colors[0] = color_fact(
        0, 0, Slic3r::GCodeProcessorResult::PreviewColorSource::Filament,
        { 0.5f, 0.25f, 0.75f, 1.0f }, "#112233");
}

std::optional<float> joint_angle_for_gcode(const PreviewArtifactView& view, unsigned int gcode_id)
{
    for (const WireMoveRecord& move : view.records<WireMoveRecord>(WireSectionType::Moves)) {
        if (move.gcode_id == gcode_id)
            return move.joint_angle_end_rad;
    }
    return std::nullopt;
}

} // namespace

TEST_CASE("Preview producer computes faithful per-move joint angles", "[worker][preview_artifact]")
{
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "libslicer-preview-producer-joint-test";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);

    Slic3r::DynamicPrintConfig config = Slic3r::DynamicPrintConfig::full_print_config();
    Slic3r::GCodeProcessorResult result;
    setup_single_filament(result);

    // gcode_id doubles as an intent label for the assertions below.
    result.moves.push_back(move(0, 0, 0, 0, Slic3r::EMoveType::Travel, { 0.0f, 0.0f, 0.2f }));
    result.moves.push_back(move(1, 0, 0, 0, Slic3r::EMoveType::Extrude, { 1.0f, 0.0f, 0.2f })); // straight run
    result.moves.push_back(move(2, 0, 0, 0, Slic3r::EMoveType::Extrude, { 2.0f, 0.0f, 0.2f })); // corner ahead
    result.moves.push_back(move(3, 0, 0, 0, Slic3r::EMoveType::Extrude, { 2.0f, 1.0f, 0.2f })); // extrude -> travel
    result.moves.push_back(move(4, 0, 0, 0, Slic3r::EMoveType::Travel, { 5.0f, 5.0f, 0.2f }));  // travel start
    result.moves.push_back(move(5, 0, 0, 0, Slic3r::EMoveType::Extrude, { 6.0f, 5.0f, 0.2f })); // extrude -> seam
    result.moves.push_back(move(6, 0, 0, 0, Slic3r::EMoveType::Seam, { 6.0f, 5.0f, 0.2f }));    // seam between coincident extrudes
    result.moves.push_back(move(7, 0, 0, 0, Slic3r::EMoveType::Extrude, { 7.0f, 5.0f, 0.2f })); // zero-length next
    result.moves.push_back(move(8, 0, 0, 0, Slic3r::EMoveType::Extrude, { 7.0f, 5.0f, 0.2f })); // coincident endpoint

    CancellationToken cancellation;
    const std::filesystem::path path = root / "preview.orcapv";
    PreviewProducerResult produced = produce_preview_artifact(
        preview_output(path), preview_context(root), config, result, cancellation);
    REQUIRE(produced.success);

    PreviewArtifactStorage storage;
    storage.load(path);
    const PreviewArtifactView view = storage.view();

    // Wire schema was bumped to 2 alongside the new field.
    CHECK(view.header.version == 2);
    CHECK(schema_version == 2);

    constexpr float half_pi = 1.57079632679f;

    // Straight extrude->extrude run: no turn.
    CHECK(joint_angle_for_gcode(view, 1).value() == Catch::Approx(0.0f).margin(1e-5f));
    // Real +90 degree corner (+x into +y) at the shared vertex.
    CHECK(joint_angle_for_gcode(view, 2).value() == Catch::Approx(half_pi).margin(1e-5f));
    // extrude -> travel is a path break: pointy cap, angle 0.
    CHECK(joint_angle_for_gcode(view, 3).value() == Catch::Approx(0.0f).margin(1e-5f));
    // travel move itself is never a continuous extrusion joint.
    CHECK(joint_angle_for_gcode(view, 4).value() == Catch::Approx(0.0f).margin(1e-5f));
    // extrude -> seam (between two coincident extrudes) is a break: angle 0.
    CHECK(joint_angle_for_gcode(view, 5).value() == Catch::Approx(0.0f).margin(1e-5f));
    // seam move itself: angle 0.
    CHECK(joint_angle_for_gcode(view, 6).value() == Catch::Approx(0.0f).margin(1e-5f));
    // Next extrude is zero-length: no defined direction, angle 0.
    CHECK(joint_angle_for_gcode(view, 7).value() == Catch::Approx(0.0f).margin(1e-5f));

    // The only nonzero angle in the whole stream is the genuine corner.
    int nonzero = 0;
    for (const WireMoveRecord& m : view.records<WireMoveRecord>(WireSectionType::Moves)) {
        if (std::abs(m.joint_angle_end_rad) > 1e-5f)
            ++nonzero;
    }
    CHECK(nonzero == 1);
}

TEST_CASE("Preview producer serializes explicit filament-to-tool mappings", "[worker][preview_artifact]")
{
    const std::vector<std::vector<int>> maps {
        { 0 },
        { 0, 0 },
        { 0, 1 },
        { 1, 0 },
        { 0, 0, 0, 0 },
    };

    for (const std::vector<int>& map : maps) {
        PreviewArtifactStorage storage;
        const PreviewArtifactView view = write_and_read(map, false, storage);
        assert_mapping(view, map);
    }
}

TEST_CASE("Preview producer serializes exact color ids referenced by moves", "[worker][preview_artifact]")
{
    PreviewArtifactStorage storage;
    const PreviewArtifactView view = write_and_read({ 0, 0 }, true, storage);

    std::set<std::uint16_t> color_ids;
    for (const WireColorRecord& color : view.records<WireColorRecord>(WireSectionType::Colors)) {
        color_ids.insert(color.id);
        if (color.id == 42) {
            CHECK(color.filament_id == 1);
            CHECK(color.source == ColorSource::ColorChange);
            CHECK(color.color_rgba[0] == Catch::Approx(0.9f));
            CHECK(color.color_rgba[1] == Catch::Approx(0.1f));
            CHECK(color.color_rgba[2] == Catch::Approx(0.2f));
            CHECK(color.color_rgba[3] == Catch::Approx(1.0f));
        }
    }
    REQUIRE(color_ids.count(42) == 1);

    bool saw_referenced_color_change = false;
    for (const WireMoveRecord& move : view.records<WireMoveRecord>(WireSectionType::Moves)) {
        if (move.cp_color_id == 42) {
            saw_referenced_color_change = true;
            CHECK(has_flag(move.flags, MoveFlags::HasCpColor));
        }
        CHECK(color_ids.count(move.cp_color_id) == 1);
    }
    CHECK(saw_referenced_color_change);
}

TEST_CASE("Output request parses preview transport modes", "[worker][preview_artifact]")
{
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "libslicer-preview-output-request-test";
    artifacts::OutputRequest output;
    std::string error;

    const nlohmann::json shared_memory_request = {
        { "artifacts_dir", "artifacts" },
        { "preview", {
            { "enabled", true },
            { "transport", "shared_memory" },
            { "format", std::string(binary_format_name) },
            { "publish", "final" }
        } }
    };
    REQUIRE(artifacts::parse_output_request(shared_memory_request, root, output, error));
    CHECK(output.preview.enabled);
    CHECK(output.preview.transport == "shared_memory");
    CHECK(output.preview.path == root / "artifacts" / "preview.orcapv");

    const nlohmann::json shared_memory_fd_request = {
        { "artifacts_dir", "artifacts" },
        { "preview", {
            { "enabled", true },
            { "transport", "shared_memory_fd" },
            { "format", std::string(binary_format_name) },
            { "publish", "final" }
        } }
    };
    REQUIRE(artifacts::parse_output_request(shared_memory_fd_request, root, output, error));
    CHECK(output.preview.enabled);
    CHECK(output.preview.transport == "shared_memory_fd");
    CHECK(output.preview.path == root / "artifacts" / "preview.orcapv");

    const nlohmann::json file_request = {
        { "artifacts_dir", "artifacts" },
        { "preview", {
            { "enabled", true },
            { "format", std::string(binary_format_name) },
            { "publish", "final" }
        } }
    };
    REQUIRE(artifacts::parse_output_request(file_request, root, output, error));
    CHECK(output.preview.transport == "file");

    const nlohmann::json invalid_request = {
        { "artifacts_dir", "artifacts" },
        { "preview", {
            { "enabled", true },
            { "transport", "pipe" },
            { "format", std::string(binary_format_name) },
            { "publish", "final" }
        } }
    };
    CHECK_FALSE(artifacts::parse_output_request(invalid_request, root, output, error));
    CHECK(error.find("unsupported output.preview transport") != std::string::npos);
}

#ifndef _WIN32
TEST_CASE("Preview producer writes shared memory transport", "[worker][preview_artifact]")
{
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "libslicer-preview-producer-shm-test";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);

    Slic3r::DynamicPrintConfig config = Slic3r::DynamicPrintConfig::full_print_config();
    Slic3r::GCodeProcessorResult result;
    make_result(result, { 0, 0 }, true);
    CancellationToken cancellation;

    const std::filesystem::path file_path = root / "preview.orcapv";
    PreviewProducerResult produced = produce_preview_artifact(
        shared_memory_preview_output(file_path), preview_context(root), config, result, cancellation);
    REQUIRE(produced.success);
    CHECK(produced.transport == "shared_memory");
    CHECK(produced.path.empty());
    REQUIRE_FALSE(produced.shm_name.empty());
    REQUIRE(produced.size > 0);
    CHECK_FALSE(std::filesystem::exists(file_path));

    PreviewArtifactStorage storage;
    storage.load_shared_memory(produced.shm_name, static_cast<std::size_t>(produced.size), false);
    const PreviewArtifactView view = storage.view();
    const nlohmann::json metadata = nlohmann::json::parse(view.metadata_json);
    CHECK(metadata.at("output").at("preview_transport") == "shared_memory");
    CHECK(view.records<WireMoveRecord>(WireSectionType::Moves).size() == result.moves.size());

    const int write_fd = ::shm_open(produced.shm_name.c_str(), O_RDWR, 0);
    REQUIRE(write_fd >= 0);
    void* writable = ::mmap(nullptr, static_cast<std::size_t>(produced.size), PROT_READ | PROT_WRITE, MAP_SHARED, write_fd, 0);
    REQUIRE(writable != MAP_FAILED);
    constexpr std::uint64_t marker = 0x5d4b3a291807f6e5ULL;
    reinterpret_cast<WireFileHeader*>(writable)->reserved[0] = marker;
    CHECK(::munmap(writable, static_cast<std::size_t>(produced.size)) == 0);
    ::close(write_fd);
    CHECK(storage.view().header.reserved[0] == marker);

    unlink_preview_shared_memory(produced.shm_name);
    const int missing_fd = ::shm_open(produced.shm_name.c_str(), O_RDONLY, 0);
    CHECK(missing_fd < 0);
    CHECK(errno == ENOENT);
    if (missing_fd >= 0)
        ::close(missing_fd);

    const WorkerEvent event = make_preview_ready_event("preview-shm-job", produced);
    const std::optional<WorkerEvent> parsed = event_from_json_line(event_to_json_line(event));
    REQUIRE(parsed.has_value());
    CHECK(parsed->transport == "shared_memory");
    CHECK(parsed->shm_name == produced.shm_name);
    CHECK(parsed->size == produced.size);
    CHECK(parsed->path.empty());

    std::filesystem::remove_all(root);
}

TEST_CASE("Preview artifact storage maps anonymous shared memory file descriptors", "[worker][preview_artifact]")
{
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "libslicer-preview-fd-reader-test";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);

    Slic3r::DynamicPrintConfig config = Slic3r::DynamicPrintConfig::full_print_config();
    Slic3r::GCodeProcessorResult result;
    make_result(result, { 0, 0 }, true);
    CancellationToken cancellation;

    PreviewProducerResult produced = produce_preview_artifact(
        shared_memory_preview_output(root / "preview.orcapv"), preview_context(root), config, result, cancellation);
    REQUIRE(produced.success);
    REQUIRE_FALSE(produced.shm_name.empty());
    REQUIRE(produced.size > 0);

    const int fd = ::shm_open(produced.shm_name.c_str(), O_RDONLY, 0);
    REQUIRE(fd >= 0);
    CHECK(::shm_unlink(produced.shm_name.c_str()) == 0);

    PreviewArtifactStorage storage;
    storage.load_file_descriptor(fd, static_cast<std::size_t>(produced.size));
    errno = 0;
    CHECK(::fcntl(fd, F_GETFD) < 0);
    CHECK(errno == EBADF);

    const PreviewArtifactView view = storage.view();
    const nlohmann::json metadata = nlohmann::json::parse(view.metadata_json);
    CHECK(metadata.at("output").at("preview_transport") == "shared_memory");
    CHECK(view.records<WireMoveRecord>(WireSectionType::Moves).size() == result.moves.size());

    const int missing_fd = ::shm_open(produced.shm_name.c_str(), O_RDONLY, 0);
    CHECK(missing_fd < 0);
    CHECK(errno == ENOENT);
    if (missing_fd >= 0)
        ::close(missing_fd);

    std::filesystem::remove_all(root);
}

TEST_CASE("Preview producer sends anonymous shared memory fd preview events", "[worker][preview_artifact][protocol]")
{
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "libslicer-preview-producer-shm-fd-test";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);

    Slic3r::DynamicPrintConfig config = Slic3r::DynamicPrintConfig::full_print_config();
    Slic3r::GCodeProcessorResult result;
    make_result(result, { 0, 0 }, true);
    CancellationToken cancellation;

    const std::filesystem::path file_path = root / "preview.orcapv";
    PreviewProducerResult produced = produce_preview_artifact(
        shared_memory_fd_preview_output(file_path), preview_context(root), config, result, cancellation);
    INFO(produced.code << ": " << produced.message);
    REQUIRE(produced.success);
    CHECK(produced.transport == "shared_memory_fd");
    CHECK(produced.path.empty());
    CHECK(produced.shm_name.empty());
    REQUIRE(produced.size > 0);
    REQUIRE(produced.file_descriptor >= 0);
    CHECK_FALSE(std::filesystem::exists(file_path));

    int sockets[2] { -1, -1 };
    REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);

    WorkerEvent event = make_preview_ready_event("preview-shm-fd-job", produced);
    REQUIRE(event.file_descriptor == produced.file_descriptor);
    REQUIRE(send_worker_event(sockets[0], event));
    ::close(produced.file_descriptor);
    produced.file_descriptor = -1;

    WorkerProtocolReader reader;
    std::optional<WorkerEvent> received = receive_worker_event(sockets[1], reader);
    REQUIRE(received.has_value());
    CHECK(received->type == WorkerEventType::Artifact);
    CHECK(received->kind == "preview");
    CHECK(received->transport == "shared_memory_fd");
    CHECK(received->shm_name.empty());
    CHECK(received->path.empty());
    CHECK(received->size == produced.size);
    REQUIRE(received->file_descriptor >= 0);

    PreviewArtifactStorage storage;
    storage.load_file_descriptor(received->file_descriptor, static_cast<std::size_t>(received->size));
    received->file_descriptor = -1;
    const PreviewArtifactView view = storage.view();
    const nlohmann::json metadata = nlohmann::json::parse(view.metadata_json);
    CHECK(metadata.at("output").at("preview_transport") == "shared_memory_fd");
    CHECK(view.records<WireMoveRecord>(WireSectionType::Moves).size() == result.moves.size());

    ::close(sockets[0]);
    ::close(sockets[1]);
    std::filesystem::remove_all(root);
}

#if defined(__linux__)
TEST_CASE("Preview producer sweeps stale Linux shared memory names", "[worker][preview_artifact]")
{
    constexpr const char* stale_name = "/ocpv7fffffff00000000";
    ::shm_unlink(stale_name);

    int fd = ::shm_open(stale_name, O_CREAT | O_EXCL | O_RDWR, 0600);
    REQUIRE(fd >= 0);
    ::close(fd);

    CHECK(sweep_stale_preview_shared_memory() >= 1);

    errno = 0;
    fd = ::shm_open(stale_name, O_RDONLY, 0);
    CHECK(fd < 0);
    CHECK(errno == ENOENT);
    if (fd >= 0)
        ::close(fd);

    ::shm_unlink(stale_name);
}
#endif
#endif

TEST_CASE("Preview producer rejects missing mapping facts", "[worker][preview_artifact]")
{
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "libslicer-preview-producer-invalid-test";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);

    Slic3r::DynamicPrintConfig config = Slic3r::DynamicPrintConfig::full_print_config();
    Slic3r::GCodeProcessorResult result;
    make_result(result, { 0, 0 }, false);
    result.filament_to_tool_map.pop_back();
    CancellationToken cancellation;

    PreviewProducerResult produced = produce_preview_artifact(
        preview_output(root / "preview.orcapv"), preview_context(root), config, result, cancellation);
    CHECK_FALSE(produced.success);
    CHECK(produced.code == "preview_mapping_invalid");
}
