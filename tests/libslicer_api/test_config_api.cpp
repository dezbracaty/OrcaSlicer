#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <libslicer/Config.hpp>
#include <libslicer/Library.hpp>

#include <miniz.h>
#include <nlohmann/json.hpp>
#include <png.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string_view>
#include <vector>

namespace {
// Real-model validation must use the same explicit mesh placement as the App.
// Compatibility file input does not apply SliceObjectInput::transform.
libslicer::SliceObjectInput binary_stl_on_bed(const std::string& path)
{
    std::ifstream input(path, std::ios::binary);
    REQUIRE(input.good());
    char header[80]; input.read(header, sizeof(header));
    std::uint32_t triangles = 0;
    input.read(reinterpret_cast<char*>(&triangles), sizeof(triangles));
    REQUIRE(std::filesystem::file_size(path) == 84ull + 50ull*triangles);
    REQUIRE(triangles > 0);
    libslicer::SliceObjectInput result;
    result.name = std::filesystem::path(path).stem().string();
    libslicer::SliceVolumeInput volume;
    std::map<std::array<float,3>,std::uint32_t> indices;
    double bottom = std::numeric_limits<double>::max();
    for (std::uint32_t i = 0; i < triangles; ++i) {
        char facet[50]; input.read(facet, sizeof(facet));
        REQUIRE(input.good());
        std::array<std::uint32_t,3> ids;
        for (size_t j = 0; j < 3; ++j) {
            std::array<float,3> xyz;
            std::memcpy(xyz.data(), facet+12+j*12, 12);
            for (float value : xyz) REQUIRE(std::isfinite(value));
            bottom = std::min(bottom, double(xyz[2]));
            auto entry = indices.emplace(xyz, static_cast<std::uint32_t>(volume.vertices.size()));
            if (entry.second) volume.vertices.push_back({xyz[0],xyz[1],xyz[2]});
            ids[j] = entry.first->second;
        }
        volume.triangles.push_back({ids[0],ids[1],ids[2]});
    }
    result.transform[11] = -bottom;
    result.volumes.push_back(std::move(volume));
    return result;
}

// Independently integrate owned feed from exported decimal E commands.
struct ActualFiberFeed {
    double total_mm {0.0};
    std::size_t command_count {0};
    std::size_t path_count {0};
};
ActualFiberFeed actual_fiber_feed(const std::string& gcode)
{
    ActualFiberFeed result;
    long double e_position=0, total=0, units=1;
    bool relative_e=false, relative_xyz=false, inside=false, owned_feed=false;
    std::istringstream input(gcode);
    for(std::string line;std::getline(input,line);) {
        if(line.rfind(";FIBER_BEGIN ",0)==0) {
            REQUIRE_FALSE(inside);
            inside=true;owned_feed=false;++result.path_count;
            const auto start=line.find(" e_units_per_mm=");
            REQUIRE(start!=std::string::npos);
            units=std::stold(line.substr(start+16));
            REQUIRE(std::isfinite(units));REQUIRE(units>0);
        } else if(line==";FIBER_PREFEED_BEGIN" || line==";FIBER_START") {
            REQUIRE(inside);owned_feed=true;
        } else if(line==";FIBER_PREFEED_END" || line==";FIBER_CUT") {
            REQUIRE(inside);owned_feed=false;
        } else if(line==";FIBER_END") {
            REQUIRE(inside);inside=false;owned_feed=false;
        }
        std::istringstream command(line.substr(0,line.find(';')));
        std::string opcode;command>>opcode;
        if(opcode=="M82")relative_e=false;
        if(opcode=="M83")relative_e=true;
        if(opcode=="G90")relative_xyz=false;
        if(opcode=="G91")relative_xyz=true;
        const bool move=opcode=="G0" || opcode=="G1" || opcode=="G2" || opcode=="G3";
        if(!move && opcode!="G92")continue;
        std::optional<long double> e_word;
        bool axis_found=false;
        for(std::string word;command>>word;) {
            if(word.size()<2)continue;
            if(std::string("XYZE").find(word[0])!=std::string::npos)axis_found=true;
            if(word[0]=='E')e_word=std::stold(word.substr(1));
        }
        if(opcode=="G92") {
            if(e_word)e_position=*e_word;
            else if(!axis_found)e_position=0;
            continue;
        }
        if(!e_word)continue;
        const long double target=relative_e || relative_xyz ? e_position+*e_word : *e_word;
        const long double delta=target-e_position;
        if(inside) {
            REQUIRE(owned_feed);
            REQUIRE(delta>0);
            total+=delta/units;
            ++result.command_count;
        }
        e_position=target;
    }
    REQUIRE_FALSE(inside);
    result.total_mm=static_cast<double>(total);
    return result;
}

const libslicer::SettingItem* find_item(const std::vector<libslicer::SettingItem>& items, std::string_view key)
{
    for (const auto& item : items) {
        if (item.key == key)
            return &item;
    }
    return nullptr;
}

std::vector<unsigned char> read_zip_entry(const std::filesystem::path& archive_path,
                                          const char* entry_name)
{
    mz_zip_archive archive{};
    if (!mz_zip_reader_init_file(&archive, archive_path.string().c_str(), 0)) {
        return {};
    }
    std::size_t size = 0;
    void* data = mz_zip_reader_extract_file_to_heap(
        &archive, entry_name, &size, 0);
    std::vector<unsigned char> result;
    if (data != nullptr && size > 0) {
        const auto* begin = static_cast<const unsigned char*>(data);
        result.assign(begin, begin + size);
    }
    mz_free(data);
    mz_zip_reader_end(&archive);
    return result;
}

struct DecodedPng
{
    unsigned int width{0};
    unsigned int height{0};
    std::vector<unsigned char> rgba;
};

std::optional<DecodedPng> decode_png(const std::vector<unsigned char>& encoded)
{
    if (encoded.empty()) {
        return std::nullopt;
    }
    png_image image{};
    image.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_memory(&image, encoded.data(), encoded.size())) {
        return std::nullopt;
    }
    image.format = PNG_FORMAT_RGBA;
    DecodedPng result;
    result.width = image.width;
    result.height = image.height;
    result.rgba.resize(PNG_IMAGE_SIZE(image));
    const bool decoded = png_image_finish_read(
        &image, nullptr, result.rgba.data(), 0, nullptr) != 0;
    png_image_free(&image);
    return decoded ? std::optional<DecodedPng>(std::move(result)) : std::nullopt;
}
} // namespace

namespace {
struct P3ExtrusionCheck {
    std::size_t xy_extrusions {0};
    double max_cross_section {0};
    std::vector<std::string> violations;
};

// Independent final-G-code check for the P3 thermoplastic coupons below.
// The machine shares a 1.75 mm feed channel, with Klipper's default limit of
// 4 * nozzle_diameter^2. Firmware tool macros are outside this static check.
P3ExtrusionCheck check_p3_linear_extrusion(const std::string& gcode)
{
    constexpr double nozzle = 0.4;
    const double filament_area = std::acos(-1.0) * 1.75 * 1.75 / 4;
    constexpr double max_area = 4 * nozzle * nozzle;
    P3ExtrusionCheck result;
    std::array<double, 4> position {};
    bool absolute_xyz = true, absolute_e = true;
    double flow = 1;
    std::istringstream input(gcode);
    std::size_t line_number = 0;
    for (std::string line; std::getline(input, line);) {
        ++line_number;
        std::istringstream words(line.substr(0, line.find(';')));
        std::string command;
        words >> command;
        if (command.empty()) continue;
        if (command == "G90") absolute_xyz = true;
        if (command == "G91") absolute_xyz = false;
        if (command == "M82") absolute_e = true;
        if (command == "M83") absolute_e = false;
        // Arc fitting is disabled in the fixture; never silently skip an arc.
        REQUIRE(command != "G2");
        REQUIRE(command != "G3");
        if (command != "G0" && command != "G1" && command != "G92" && command != "M221") continue;
        std::map<char, double> parameters;
        for (std::string word; words >> word;) {
            if (word.size() > 1 && std::string("XYZES").find(word.front()) != std::string::npos)
                parameters[word.front()] = std::stod(word.substr(1));
        }
        if (command == "M221" && parameters.count('S')) flow = parameters.at('S') / 100;
        if (command == "G92") {
            if (parameters.empty()) position.fill(0);
            for (std::size_t axis = 0; axis < 4; ++axis)
                if (parameters.count("XYZE"[axis])) position[axis] = parameters.at("XYZE"[axis]);
        }
        if (command != "G0" && command != "G1") continue;
        auto target = position;
        for (std::size_t axis = 0; axis < 4; ++axis) {
            const char name = "XYZE"[axis];
            if (!parameters.count(name)) continue;
            const bool absolute = absolute_xyz && (axis != 3 || absolute_e);
            target[axis] = parameters.at(name) + (absolute ? 0 : position[axis]);
        }
        const double dx = target[0] - position[0], dy = target[1] - position[1];
        const double dz = target[2] - position[2], de = (target[3] - position[3]) * flow;
        // Like Klipper check_move, retractions and E-only moves do not use
        // the XY deposition cross-section limit. Tiny positive E is allowed.
        if (de > 0 && (dx != 0 || dy != 0)) {
            ++result.xy_extrusions;
            const double cross_section = filament_area * de / std::sqrt(dx*dx + dy*dy + dz*dz);
            result.max_cross_section = std::max(result.max_cross_section, cross_section);
            if (cross_section > max_area && de > nozzle * max_area / filament_area)
                result.violations.push_back("line " + std::to_string(line_number) + ": " + line +
                    " (" + std::to_string(cross_section) + " mm^2 > 0.640 mm^2)");
        }
        position = target;
    }
    return result;
}

std::set<unsigned> require_p3_heat_before_extrusion(const std::string& gcode)
{
    std::set<unsigned> tools;
    bool waiting_for_heat = false;
    bool saw_extrusion = false;
    std::istringstream input(gcode);
    std::size_t line_number = 0;
    for (std::string line; std::getline(input, line);) {
        ++line_number;
        const auto comment = line.find(';');
        if (comment != std::string::npos) line.resize(comment);
        std::istringstream command_input(line);
        std::string command;
        command_input >> command;
        if (command.size() > 1 && command.front() == 'T' &&
            std::all_of(command.begin() + 1, command.end(), [](unsigned char c) { return std::isdigit(c); })) {
            tools.insert(static_cast<unsigned>(std::stoul(command.substr(1))));
            waiting_for_heat = true; // P3 dock selection clears the heater target.
        } else if (command == "M109") {
            for (std::string parameter; command_input >> parameter;)
                if (parameter.size() > 1 && parameter.front() == 'S' && std::stod(parameter.substr(1)) > 0)
                    waiting_for_heat = false;
        } else if (command == "G0" || command == "G1" || command == "G2" || command == "G3") {
            for (std::string parameter; command_input >> parameter;)
                if (parameter.size() > 1 && parameter.front() == 'E' && std::stod(parameter.substr(1)) != 0) {
                    INFO("P3 extrusion before temperature wait, line " << line_number << ": " << line);
                    REQUIRE_FALSE(waiting_for_heat);
                    saw_extrusion = true;
                }
        }
    }
    REQUIRE(saw_extrusion);
    REQUIRE_FALSE(tools.empty());
    return tools;
}

void require_p3_retraction_lengths(const std::string& gcode, const std::set<unsigned>& fiber_tools = {})
{
    // P3 exports physical millimetres in relative E mode. Inspect executable
    // moves, so a material override or change script cannot hide behind the
    // machine preset's configured value.
    std::istringstream input(gcode);
    unsigned tool = 0;
    std::size_t retractions = 0, line_number = 0;
    for (std::string line; std::getline(input, line);) {
        ++line_number;
        std::istringstream words(line.substr(0, line.find(';')));
        std::string command;
        words >> command;
        REQUIRE(command != "M82");
        if (command.size() > 1 && command.front() == 'T' &&
            std::all_of(command.begin()+1, command.end(), [](unsigned char c) { return std::isdigit(c); }))
            tool = unsigned(std::stoul(command.substr(1)));
        if (command != "G0" && command != "G1") continue;
        for (std::string word; words >> word;)
            if (word.size() > 1 && word.front() == 'E' && std::stod(word.substr(1)) < 0) {
                INFO("T" << tool << " retraction at line " << line_number << ": " << line);
                CHECK(fiber_tools.count(tool) == 0);
                CHECK(std::stod(word.substr(1)) == Catch::Approx(-0.4));
                ++retractions;
            }
    }
    CHECK(retractions > 0);
}
} // namespace

TEST_CASE("P3 extrusion checker catches the MMU tower load", "[libslicer_api][p3][extrusion-check]")
{
    const std::string legacy_load = "G90\nM83\nG1 X60.918 Y66.5 Z0.2\n"
        "G1 E18 F600\nG1 X110.375 E63 F2826\nG1 X60.918 E9 F1978\n";
    const auto rejected = check_p3_linear_extrusion(legacy_load);
    REQUIRE(rejected.violations.size() == 1);
    CHECK(rejected.violations.front().find("line 5:") != std::string::npos);
    CHECK(rejected.max_cross_section == Catch::Approx(3.0639294367));

    SECTION("absolute E and G92 must not hide excess feed") {
        const auto absolute = check_p3_linear_extrusion(
            "G90\nM82\nG1 X60.918 Y66.5\nG92 E100\nG1 X110.375 E163\n");
        REQUIRE(absolute.violations.size() == 1);
        CHECK(absolute.max_cross_section == Catch::Approx(rejected.max_cross_section));
    }
    SECTION("relative XYZ and flow override affect the limit") {
        const auto relative = check_p3_linear_extrusion(
            "G91\nM82\nM221 S200\nG1 X10 E2\nG1 X10 E2\n");
        CHECK(relative.xy_extrusions == 2);
        CHECK(relative.violations.size() == 2);
    }
    SECTION("normal deposition and Klipper exceptions remain accepted") {
        const auto accepted = check_p3_linear_extrusion(
            "G90\nM83\nG1 X10 E0.4\nG1 X20 E-1\nG1 E18\nG1 X20.01 E0.05\n");
        CHECK(accepted.xy_extrusions == 2);
        CHECK(accepted.violations.empty());
    }
}

TEST_CASE("imported single material plate follows P3 material bindings", "[libslicer_api][plate-material-sync][slice]")
{
    libslicer::LibraryOptions options;
    options.resource_directory = LIBSLICER_TEST_RESOURCE_DIR;
    options.vendors = {"P3"};
    auto library = libslicer::Library::open(options);
    REQUIRE(library);
    libslicer::ConfigSelection selection;
    selection.machine_model_id = "P3";
    selection.machine_variant_id = "0.4";
    REQUIRE(library->activate_config(selection).success);
    libslicer::ProjectPlate plate;
    plate.settings = {{"filament_map", "1"}, {"filament_map_mode", "Auto For Flush"}};
    libslicer::SliceRequest request;
    request.config = *library->active_config_snapshot();
    request.project_semantics = libslicer::encode_project_plate_semantics(plate);
    std::ostringstream evidence;
    for (const auto* key : {"filament_map", "filament_diameter", "filament_process_type"})
        evidence << key << "=" << request.config.value(key).value_or("<missing>") << '\n';
    evidence << "plate filament_map=1 (overrides global at slice time)\n";
    libslicer::SliceObjectInput object;
    object.name = "plate-material-coupon";
    libslicer::SliceVolumeInput volume;
    volume.vertices = {{100,100,0},{110,100,0},{110,110,0},{100,110,0},
                       {100,100,0.6f},{110,100,0.6f},{110,110,0.6f},{100,110,0.6f}};
    volume.triangles = {{0,2,1},{0,3,2},{4,5,6},{4,6,7},{0,1,5},{0,5,4},
                        {1,2,6},{1,6,5},{2,3,7},{2,7,6},{3,0,4},{3,4,7}};
    volume.default_filament_slot = 1;
    object.volumes = {volume};
    request.objects = {object};
    // The unsynchronized request must remain invalid: no repair at slice time.
    const auto stale = library->slice(request);
    REQUIRE_FALSE(stale.success);
    REQUIRE(std::any_of(stale.diagnostics.begin(), stale.diagnostics.end(), [](const auto& d) {
        return d.message.find("Every material must have a process type and tool mapping") != std::string::npos;
    }));
    std::vector<libslicer::ConfigDiagnostic> sync_diagnostics;
    request.project_semantics = libslicer::synchronize_project_plate_materials(
        request.project_semantics, request.config, &sync_diagnostics);
    const auto settings = nlohmann::json::parse(request.project_semantics)["settings"];
    CHECK_FALSE(settings.contains("filament_map"));
    CHECK_FALSE(settings.contains("filament_map_mode"));
    CHECK_FALSE(sync_diagnostics.empty());
    const auto sliced = library->slice(request);
    for (const auto& issue : sliced.diagnostics) evidence << issue.message << '\n';
    INFO(evidence.str());
    REQUIRE(sliced.success);
    REQUIRE(sliced.preview);
    CHECK(std::any_of(sliced.preview->segments.begin(), sliced.preview->segments.end(), [](const auto& segment) {
        return segment.tool_id == 0 && segment.extrusion_delta_mm > 0;
    }));
    CHECK(std::none_of(sliced.preview->segments.begin(), sliced.preview->segments.end(), [](const auto& segment) {
        return segment.tool_id != 0 && segment.extrusion_delta_mm > 0;
    }));
    std::ifstream input(sliced.output.path);
    const std::string gcode((std::istreambuf_iterator<char>(input)), {});
    CHECK(gcode.find("; filament_map = 1,2,3,4,5,6") != std::string::npos);
    for (const std::string illegal : {"7,2,3,4,5,6", "1,1,3,4,5,6", "1,2,3,4,6,5"}) {
        plate.settings = {{"filament_map", illegal}, {"filament_map_mode", "Manual"}};
        request.project_semantics = libslicer::encode_project_plate_semantics(plate);
        CHECK_FALSE(library->slice(request).success);
    }
    if (sliced.output.ownership == libslicer::OutputArtifactOwnership::LibraryTemporary)
        std::filesystem::remove(sliced.output.path);
}

TEST_CASE("plate material synchronization preserves local semantics and rejects stale owners",
          "[libslicer_api][plate-material-sync]")
{
    libslicer::LibraryOptions options;
    options.resource_directory = LIBSLICER_TEST_RESOURCE_DIR;
    options.vendors = {"P3", "CFSYS", "Flashforge"};
    auto library = libslicer::Library::open(options);
    REQUIRE(library);
    libslicer::ConfigSelection selection;
    selection.machine_model_id = "P3";
    selection.machine_variant_id = "0.4";
    REQUIRE(library->activate_config(selection).success);
    const auto target = *library->active_config_snapshot();
    libslicer::ProjectPlate plate;
    plate.native_plate_index = 2;
    plate.custom_gcode_mode = 1;
    plate.custom_gcodes = {{1,4,0,"",";keep-custom-code"}};
    plate.settings = {{"filament_map", "1,2,3,4,5,6"}, {"filament_map_mode", "Manual"},
                      {"sparse_infill_density", "23%"}, {"wipe_tower_x", "30,40,50"}};
    const auto original = libslicer::encode_project_plate_semantics(plate);
    CHECK(libslicer::synchronize_project_plate_materials(original, target) == original);
    for (const std::string bad : {"1", "7,2,3,4,5,6", "1,1,3,4,5,6", "1,2,3,4,6,5"}) {
        plate.settings[0].second = bad;
        const auto source = nlohmann::json::parse(libslicer::encode_project_plate_semantics(plate));
        std::vector<libslicer::ConfigDiagnostic> diagnostics;
        const auto synced = nlohmann::json::parse(libslicer::synchronize_project_plate_materials(source.dump(), target, &diagnostics));
        CHECK_FALSE(diagnostics.empty());
        auto expected = source;
        expected["settings"].erase("filament_map");
        expected["settings"].erase("filament_map_mode");
        CHECK(synced == expected);
    }
    // Native empty-map/absent-mode inheritance does not become an override.
    plate.settings = {{"filament_map", ""}};
    const auto inherited = nlohmann::json::parse(libslicer::synchronize_project_plate_materials(
        libslicer::encode_project_plate_semantics(plate), target));
    CHECK(inherited["settings"].empty());
    CHECK(libslicer::synchronize_project_plate_materials("", target).empty());
    CHECK_THROWS(libslicer::synchronize_project_plate_materials("{}", target));

    selection.machine_model_id = "Flashforge AD5X";
    REQUIRE(library->activate_config(selection).success);
    REQUIRE(library->resize_active_filament_slots(2).success);
    plate.settings = {{"filament_map", "1,1"}, {"filament_map_mode", "Manual"}, {"wall_loops", "4"}};
    auto payload = libslicer::encode_project_plate_semantics(plate);
    REQUIRE(library->resize_active_filament_slots(4).success);
    auto synced = libslicer::synchronize_project_plate_materials(payload, *library->active_config_snapshot());
    CHECK(nlohmann::json::parse(synced)["settings"]["filament_map"] == "1,1,1,1");
    CHECK(nlohmann::json::parse(synced)["settings"]["filament_map_mode"] == "Manual");
    REQUIRE(library->resize_active_filament_slots(1).success);
    synced = libslicer::synchronize_project_plate_materials(synced, *library->active_config_snapshot());
    CHECK(nlohmann::json::parse(synced)["settings"]["filament_map"] == "1");
    CHECK(nlohmann::json::parse(synced)["settings"]["wall_loops"] == "4");

    selection.machine_model_id = "Flashforge Creator 5";
    REQUIRE(library->activate_config(selection).success);
    REQUIRE(library->resize_active_filament_slots(4).success);
    plate.settings = {{"filament_map", "2,1,4,3"}, {"filament_map_mode", "Manual"}};
    payload = libslicer::encode_project_plate_semantics(plate);
    CHECK(libslicer::synchronize_project_plate_materials(payload, *library->active_config_snapshot()) == payload);
}

TEST_CASE("P3 PLA TPU dock changes stay within extrusion limits with and without tower",
          "[libslicer_api][p3][wipe-tower][slice]")
{
    const bool tower = GENERATE(false, true);
    const bool infill_only = GENERATE(false, true);
    INFO("enable_prime_tower=" << tower);
    INFO("single object with TPU infill=" << infill_only);
    libslicer::LibraryOptions options;
    options.resource_directory = LIBSLICER_TEST_RESOURCE_DIR;
    options.vendors = {"P3"};
    auto library = libslicer::Library::open(options);
    REQUIRE(library);
    libslicer::ConfigSelection selection;
    selection.machine_model_id = "P3";
    selection.machine_variant_id = "0.4";
    selection.process_preset_id = "0.20mm Standard @P3";
    selection.filament_preset_ids.assign(6, "Generic PLA @P3");
    selection.filament_preset_ids[3] = "CFSYS TPU 95A";
    selection.filament_physical_tools = {0, 1, 2, 3, 4, 5};
    auto created = library->create_config(selection);
    for (const auto& issue : created.diagnostics) UNSCOPED_INFO(issue.message);
    REQUIRE(created.success);
    // Load the real preset: overriding this value here would mask a regression.
    CHECK(created.config->snapshot().value("single_extruder_multi_material") == "0");
    REQUIRE(created.config->apply_patch({
        {"filament_colour", "#F28E2B;#4E79A7;#59A14F;#B07AA1;#EDC948;#E15759"},
        {"enable_prime_tower", tower ? "1" : "0"}, {"wipe_tower_type", "type2"},
        {"wipe_tower_x", "60"}, {"wipe_tower_y", "60"}, {"prime_tower_width", "60"},
        {"wipe_tower_wall_type", "rectangle"}, {"enable_arc_fitting", "0"},
        {"enable_support", "0"}, {"generate_reinforced_perimeters", "0"},
        {"generate_reinforced_infills", "0"}
    }).success);
    if (infill_only)
        REQUIRE(created.config->apply_patch({
            {"outer_wall_filament_id", "1"}, {"inner_wall_filament_id", "1"},
            {"top_surface_filament_id", "1"}, {"bottom_surface_filament_id", "1"},
            {"sparse_infill_filament_id", "4"}, {"internal_solid_filament_id", "4"},
            {"top_shell_layers", "1"}, {"bottom_shell_layers", "1"}
        }).success);
    libslicer::SliceRequest request;
    request.config = created.config->snapshot();
    // Two three-layer coupons force repeated T0/T3 changes in both directions.
    for (const unsigned tool : {0u, 3u}) {
        if (infill_only && tool == 3u) continue;
        const float x = tool == 0 ? 160.f : 180.f;
        libslicer::SliceObjectInput object;
        object.name = "P3-extrusion-T" + std::to_string(tool);
        libslicer::SliceVolumeInput volume;
        volume.vertices = {{x,150,0},{x+10,150,0},{x+10,160,0},{x,160,0},
                           {x,150,0.6f},{x+10,150,0.6f},{x+10,160,0.6f},{x,160,0.6f}};
        volume.triangles = {{0,2,1},{0,3,2},{4,5,6},{4,6,7},{0,1,5},{0,5,4},
                           {1,2,6},{1,6,5},{2,3,7},{2,7,6},{3,0,4},{3,4,7}};
        volume.default_filament_slot = int(tool + 1);
        object.volumes.push_back(std::move(volume));
        request.objects.push_back(std::move(object));
    }
    const auto output = std::filesystem::temp_directory_path() /
        (tower ? "libslicer-p3-pla-tpu-tower.gcode" : "libslicer-p3-pla-tpu-no-tower.gcode");
    request.output_gcode_path = output.string();
    const auto sliced = library->slice(request);
    for (const auto& issue : sliced.diagnostics) UNSCOPED_INFO(issue.message);
    REQUIRE(sliced.success);
    REQUIRE(sliced.summary.layer_count == 3);
    REQUIRE(sliced.preview);
    for (const unsigned tool : {0u, 3u})
        CHECK(std::any_of(sliced.preview->segments.begin(), sliced.preview->segments.end(),
            [tool](const auto& segment) { return segment.tool_id == tool && segment.extrusion_delta_mm > 0; }));
    std::ifstream input(output);
    REQUIRE(input.good());
    const std::string gcode((std::istreambuf_iterator<char>(input)), {});
    CHECK(require_p3_heat_before_extrusion(gcode) == std::set<unsigned>{0, 3});
    require_p3_retraction_lengths(gcode);
    CHECK((gcode.find(";TYPE:Prime tower") != std::string::npos) == tower);
    CHECK(gcode.find("; CP TOOLCHANGE LOAD") == std::string::npos);
    CHECK(gcode.find("; Retract(unload)") == std::string::npos);
    CHECK(gcode.find("; Cooling park") == std::string::npos);
    const auto checked = check_p3_linear_extrusion(gcode);
    REQUIRE(checked.xy_extrusions > 0);
    for (const auto& violation : checked.violations) UNSCOPED_INFO(violation);
    CHECK(checked.violations.empty());
}

TEST_CASE("P3 0.125 mm mixed preset shares layers between matrix and fiber", "[libslicer_api][p3][preset][slice]")
{
    const bool fiber = GENERATE(false, true);
    INFO("fiber=" << fiber);
    libslicer::LibraryOptions options;
    options.resource_directory = LIBSLICER_TEST_RESOURCE_DIR;
    options.vendors = {"P3"};
    auto library = libslicer::Library::open(options);
    REQUIRE(library);
    libslicer::ConfigSelection selection;
    selection.machine_model_id = "P3";
    selection.machine_variant_id = "0.4";
    selection.process_preset_id = "0.125mm CCF + Matrix @P3";
    auto created = library->create_config(selection);
    for (const auto& issue : created.diagnostics) UNSCOPED_INFO(issue.message);
    REQUIRE(created.success);
    CHECK(created.selection.process_preset_id == selection.process_preset_id);
    const auto defaults = created.config->snapshot();
    CHECK(defaults.value("layer_height") == "0.125");
    CHECK(defaults.value("initial_layer_print_height") == "0.125");
    CHECK(defaults.value("fiber_layer_height_ratio") == "1");
    CHECK(defaults.value("fiber_width") == "1");
    CHECK(defaults.value("outer_wall_filament_id") == "5");
    CHECK(defaults.value("reinforced_perimeters_filament") == "6");
    CHECK(defaults.value("retraction_length") == "0.4,0.4,0.4,0.4,0.4,0.4");
    CHECK(defaults.value("retract_length_toolchange") == "0.4,0.4,0.4,0.4,0.4,0.4");
    CHECK(defaults.value("filament_diameter") == "1.75,1.75,1.75,1.75,1.75,0.4");
    CHECK(defaults.value("nozzle_temperature_initial_layer") == "230,230,230,210,280,235");
    CHECK(defaults.value("nozzle_temperature") == "230,230,230,210,270,235");
    REQUIRE(created.config->set("filament_colour", "#F28E2B;#4E79A7;#59A14F;#B07AA1;#EDC948;#E15759").success);
    if (!fiber)
        REQUIRE(created.config->set("generate_reinforced_perimeters", "0").success);
    libslicer::SliceObjectInput object;
    object.name = "P3-0125-mixed-coupon";
    libslicer::SliceVolumeInput volume;
    volume.vertices = {{100,100,0},{120,100,0},{120,120,0},{100,120,0},
                       {100,100,2},{120,100,2},{120,120,2},{100,120,2}};
    volume.triangles = {{0,2,1},{0,3,2},{4,5,6},{4,6,7},{0,1,5},{0,5,4},
                       {1,2,6},{1,6,5},{2,3,7},{2,7,6},{3,0,4},{3,4,7}};
    volume.default_filament_slot = 5;
    object.volumes.push_back(std::move(volume));
    libslicer::SliceRequest request;
    request.objects = {object};
    request.config = created.config->snapshot();
    request.output_gcode_path = (std::filesystem::temp_directory_path() /
        (fiber ? "libslicer-p3-0125-mixed.gcode" : "libslicer-p3-0125-matrix.gcode")).string();
    const auto sliced = library->slice(request);
    for (const auto& issue : sliced.diagnostics) UNSCOPED_INFO(issue.message);
    REQUIRE(sliced.success);
    REQUIRE(sliced.summary.layer_count == 16);
    REQUIRE(sliced.preview);
    std::ifstream input(request.output_gcode_path);
    REQUIRE(input.good());
    const std::string gcode((std::istreambuf_iterator<char>(input)), {});
    const auto tools = require_p3_heat_before_extrusion(gcode);
    CHECK(tools == (fiber ? std::set<unsigned>{4, 5} : std::set<unsigned>{4}));
    CHECK((gcode.find(";FIBER_BEGIN ") != std::string::npos) == fiber);
    CHECK(gcode.find("; CP TOOLCHANGE LOAD") == std::string::npos);
    require_p3_retraction_lengths(gcode, fiber ? std::set<unsigned>{5} : std::set<unsigned>{});
    std::istringstream lines(gcode);
    unsigned layers = 0;
    for (std::string line; std::getline(lines, line);)
        if (line.rfind(";Z:", 0) == 0) {
            ++layers;
            CHECK(std::stod(line.substr(3)) == Catch::Approx(layers * 0.125));
        }
    CHECK(layers == 16);
}

TEST_CASE("P3 shares Alpha500 thermoplastic parameters and exports each material", "[libslicer_api][p3][slice]")
{
    const unsigned physical_tool = GENERATE(0u, 1u, 2u, 3u, 4u, 5u);
    INFO("physical_tool=" << physical_tool);
    libslicer::LibraryOptions options;
    options.resource_directory = LIBSLICER_TEST_RESOURCE_DIR;
    // P3 must work on its own, without loading the CFSYS machine bundle first.
    options.vendors = {"P3"};
    auto p3 = libslicer::Library::open(options);
    REQUIRE(p3);
    options.vendors = {"CFSYS"};
    auto alpha500 = libslicer::Library::open(options);
    REQUIRE(alpha500);

    const std::vector<std::string> materials = {
        "CFSYS PLA", "CFSYS PETG", "CFSYS ABS", "CFSYS ABS-CF", "CFSYS ABS-GF",
        "CFSYS ASA", "CFSYS ASA-CF", "CFSYS ASA-GF", "CFSYS PA", "CFSYS PA-CF",
        "CFSYS TPU 95A", "CFSYS PET-CF", "CFSYS PET-GF", "CFSYS PPS-CF",
        "CFSYS S-Multi", "CFSYS CIRON"
    };
    libslicer::ConfigSelection selection;
    selection.machine_model_id = "P3";
    selection.machine_variant_id = "0.4";
    const auto initial = p3->activate_config(selection);
    for (const auto& issue : initial.diagnostics) UNSCOPED_INFO(issue.message);
    REQUIRE(initial.success);
    REQUIRE(p3->active_config_snapshot());
    CHECK(p3->active_config_snapshot()->value("single_extruder_multi_material") == "0");
    REQUIRE(initial.view.filament_slots.size() == 6);
    for (const auto& slot : initial.view.filament_slots) {
        for (const auto& material : materials) {
            INFO("tool=" << slot.physical_tool_index << " material=" << material);
            CHECK(std::any_of(slot.compatible_presets.begin(), slot.compatible_presets.end(),
                [&](const auto& preset) { return preset.id == material; }));
        }
    }

    libslicer::SliceObjectInput object;
    object.name = "P3-material-coupon";
    libslicer::SliceVolumeInput volume;
    volume.vertices = {{100,100,0},{110,100,0},{110,110,0},{100,110,0},
                       {100,100,0.6f},{110,100,0.6f},{110,110,0.6f},{100,110,0.6f}};
    volume.triangles = {{0,2,1},{0,3,2},{4,5,6},{4,6,7},{0,1,5},{0,5,4},
                       {1,2,6},{1,6,5},{2,3,7},{2,7,6},{3,0,4},{3,4,7}};
    volume.default_filament_slot = int(physical_tool + 1);
    object.volumes.push_back(std::move(volume));
    const auto output_directory = std::filesystem::temp_directory_path() / "libslicer-p3-materials";
    std::filesystem::create_directories(output_directory);

    for (const auto& material : materials) {
        INFO(material);
        selection.filament_preset_ids.assign(6, "Generic PLA @P3");
        selection.filament_preset_ids[physical_tool] = material;
        const auto activated = p3->activate_config(selection);
        for (const auto& issue : activated.diagnostics) UNSCOPED_INFO(issue.message);
        REQUIRE(activated.success);
        const auto config = p3->active_config_snapshot();
        REQUIRE(config);

        libslicer::ConfigSelection reference;
        reference.machine_model_id = "CFSYS Alpha500 Printer";
        reference.machine_variant_id = "0.4";
        reference.filament_preset_ids = {material, "CFSYS CCF"};
        const auto expected = alpha500->create_config(reference);
        for (const auto& issue : expected.diagnostics) UNSCOPED_INFO(issue.message);
        REQUIRE(expected.success);
        const auto original = expected.config->snapshot();
        const auto material_value = [&](const std::string& serialized) {
            std::istringstream input(serialized);
            std::string value;
            for (unsigned index = 0; index <= physical_tool; ++index)
                REQUIRE(std::getline(input, value, ','));
            return value;
        };
        // Compare the actual loaded material, including inherited cooling and
        // flow settings, rather than just checking the catalog's JSON entries.
        for (const char* key : {"nozzle_temperature", "nozzle_temperature_initial_layer",
                "nozzle_temperature_range_low", "nozzle_temperature_range_high",
                "filament_flow_ratio", "filament_max_volumetric_speed", "filament_density",
                "filament_diameter", "fan_min_speed", "fan_max_speed",
                "close_fan_the_first_x_layers", "hot_plate_temp", "hot_plate_temp_initial_layer",
                "chamber_temperature", "pressure_advance", "enable_pressure_advance"}) {
            INFO(key);
            const auto actual_value = config->value(key);
            const auto original_value = original.value(key);
            REQUIRE(actual_value);
            REQUIRE(original_value);
            // P3 PLA has a machine-specific flow calibration; the remaining
            // material parameters continue to match the Alpha500 definition.
            const std::string expected_value = material == "CFSYS PLA" &&
                    std::string(key) == "filament_flow_ratio"
                ? "0.99" : original_value->substr(0, original_value->find(','));
            CHECK(material_value(*actual_value) == expected_value);
        }

        libslicer::SliceRequest request;
        request.config = *config;
        request.objects = {object};
        request.output_gcode_path = (output_directory / (material + "-T" + std::to_string(physical_tool) + ".gcode")).string();
        const auto sliced = p3->slice(request);
        for (const auto& issue : sliced.diagnostics) UNSCOPED_INFO(issue.message);
        REQUIRE(sliced.success);
        REQUIRE(sliced.preview);
        CHECK(sliced.preview->layers.size() == 3);
        CHECK(sliced.preview->statistics.total_fiber_feed_mm == 0);
        CHECK(std::any_of(sliced.preview->segments.begin(), sliced.preview->segments.end(),
            [](const auto& segment) { return segment.extrusion_delta_mm > 0; }));
        CHECK(std::all_of(sliced.preview->segments.begin(), sliced.preview->segments.end(),
            [&](const auto& segment) {
                return segment.extrusion_delta_mm <= 0 ||
                    (segment.tool_id == physical_tool && segment.filament_id == physical_tool);
            }));
        std::ifstream generated(request.output_gcode_path);
        REQUIRE(generated.good());
        const std::string gcode((std::istreambuf_iterator<char>(generated)), {});
        const auto selected_tools = require_p3_heat_before_extrusion(gcode);
        CHECK(selected_tools == std::set<unsigned>{physical_tool});
        const auto initial_temperature = config->value("nozzle_temperature_initial_layer");
        REQUIRE(initial_temperature);
        CHECK(gcode.find("M109 S" + material_value(*initial_temperature) + "\n") != std::string::npos);
        CHECK(gcode.find("\nT" + std::to_string(physical_tool) + "\n") != std::string::npos);
        CHECK(gcode.find("DF1004") == std::string::npos);
        CHECK(gcode.find("CF1004") == std::string::npos);
        CHECK(gcode.find(";FIBER_BEGIN") == std::string::npos);
    }
}

TEST_CASE("P3 waits after initial selection and later dock changes", "[libslicer_api][p3][tool-heat][slice]")
{
    const unsigned scene = GENERATE(0u, 5u, 6u); // T0, T5, or both in every layer.
    libslicer::LibraryOptions options;
    options.resource_directory = LIBSLICER_TEST_RESOURCE_DIR;
    options.vendors = {"P3"};
    auto library = libslicer::Library::open(options);
    REQUIRE(library);
    libslicer::ConfigSelection selection;
    selection.machine_model_id = "P3";
    selection.machine_variant_id = "0.4";
    selection.process_preset_id = "0.20mm Standard @P3";
    selection.filament_preset_ids.assign(6, "CFSYS TPU 95A");
    selection.filament_physical_tools = {0, 1, 2, 3, 4, 5};
    auto created = library->create_config(selection);
    REQUIRE(created.success);
    REQUIRE(created.config->apply_patch({
        {"filament_colour", "#F28E2B;#808080;#808080;#808080;#808080;#4E79A7"},
        {"nozzle_temperature_initial_layer", "210,210,210,210,210,220"},
        {"nozzle_temperature", "205,205,205,205,205,215"},
        {"enable_support", "0"}, {"enable_prime_tower", "0"},
        {"generate_reinforced_perimeters", "0"}, {"generate_reinforced_infills", "0"}
    }).success);
    const auto coupon = [](unsigned tool, float x) {
        libslicer::SliceObjectInput object;
        object.name = "P3-heat-T" + std::to_string(tool);
        libslicer::SliceVolumeInput volume;
        volume.vertices = {{x,100,0},{x+10,100,0},{x+10,110,0},{x,110,0},
                           {x,100,0.6f},{x+10,100,0.6f},{x+10,110,0.6f},{x,110,0.6f}};
        volume.triangles = {{0,2,1},{0,3,2},{4,5,6},{4,6,7},{0,1,5},{0,5,4},
                            {1,2,6},{1,6,5},{2,3,7},{2,7,6},{3,0,4},{3,4,7}};
        volume.default_filament_slot = int(tool + 1);
        object.volumes.push_back(std::move(volume));
        return object;
    };
    libslicer::SliceRequest request;
    request.config = created.config->snapshot();
    if (scene == 6) request.objects = {coupon(0, 100), coupon(5, 120)};
    else request.objects = {coupon(scene, 100)};
    const auto output = std::filesystem::temp_directory_path() /
        ("libslicer-p3-heat-" + std::to_string(scene) + ".gcode");
    request.output_gcode_path = output.string();
    const auto sliced = library->slice(request);
    for (const auto& issue : sliced.diagnostics) UNSCOPED_INFO(issue.message);
    REQUIRE(sliced.success);
    REQUIRE(sliced.summary.layer_count == 3);
    std::ifstream input(output);
    REQUIRE(input.good());
    const std::string gcode((std::istreambuf_iterator<char>(input)), {});
    const auto expected = scene == 6 ? std::set<unsigned>{0, 5} : std::set<unsigned>{scene};
    const auto selected_tools = require_p3_heat_before_extrusion(gcode);
    CHECK(selected_tools == expected);
    if (scene == 6) {
        CHECK(gcode.find("\nM109 S205\n") != std::string::npos);
        CHECK(gcode.find("\nM109 S215\n") != std::string::npos);
    }
}

TEST_CASE("P3 can select CCF on any tool without changing material ownership", "[libslicer_api][p3][tool-materials]")
{
    const unsigned fiber_tool = GENERATE(0u, 1u, 2u, 3u, 4u, 5u);
    INFO("fiber_tool=" << fiber_tool);
    libslicer::LibraryOptions options;
    options.resource_directory = LIBSLICER_TEST_RESOURCE_DIR;
    options.vendors = {"P3"};
    auto library = libslicer::Library::open(options);
    REQUIRE(library);
    libslicer::ConfigSelection selection;
    selection.machine_model_id = "P3";
    selection.machine_variant_id = "0.4";
    const auto initial = library->activate_config(selection);
    for (const auto& issue : initial.diagnostics) UNSCOPED_INFO(issue.message);
    REQUIRE(initial.success);
    REQUIRE(initial.view.filament_slots.size() == 6);
    for (const auto& slot : initial.view.filament_slots) {
        CHECK(std::any_of(slot.compatible_presets.begin(), slot.compatible_presets.end(),
            [](const auto& preset) { return preset.id == "CFSYS CCF"; }));
    }
    CHECK(initial.view.selection.filament_preset_ids == std::vector<std::string>{
        "CFSYS TPU 95A", "CFSYS TPU 95A", "CFSYS TPU 95A",
        "Generic PLA @P3", "CFSYS PA-CF", "CFSYS CCF"});
    CHECK(library->active_config_snapshot()->value("filament_process_type") ==
        "thermoplastic;thermoplastic;thermoplastic;thermoplastic;thermoplastic;continuous_fiber");
    // CCF is a 0.4 mm composite feedstock; its deposited fiber_width is a
    // separate process setting and must not replace the raw material diameter.
    CHECK(library->active_config_snapshot()->value("filament_diameter") ==
        "1.75,1.75,1.75,1.75,1.75,0.4");
    // Exercise reassignment from a thermoplastic baseline after checking the
    // configured mixed-material defaults.
    REQUIRE(library->set_active_filament_preset(5, "Generic PLA @P3").success);
    REQUIRE(library->set_active_config_value("layer_height", "0.15").success);
    REQUIRE(library->set_active_filament_color(fiber_tool, {20,40,60,255}).success);
    const auto selected = library->set_active_filament_preset(fiber_tool, "CFSYS CCF");
    for (const auto& issue : selected.diagnostics) UNSCOPED_INFO(issue.message);
    REQUIRE(selected.success);
    REQUIRE(selected.view.filament_slots.size() == 6);
    for (unsigned slot = 0; slot < 6; ++slot) {
        CHECK(selected.view.filament_slots[slot].physical_tool_index == slot);
    }
    std::string expected_processes;
    std::string expected_diameters;
    for (unsigned slot = 0; slot < 6; ++slot) {
        if (slot) expected_processes += ';';
        expected_processes += slot == fiber_tool ? "continuous_fiber" : "thermoplastic";
        if (slot) expected_diameters += ',';
        expected_diameters += slot == fiber_tool ? "0.4" : "1.75";
    }
    CHECK(library->active_config_snapshot()->value("filament_process_type") == expected_processes);
    CHECK(library->active_config_snapshot()->value("filament_diameter") == expected_diameters);
    CHECK(selected.view.filament_slots[fiber_tool].color.red == 20);
    CHECK(library->active_config_snapshot()->value("layer_height") == "0.15");
    CHECK(library->active_config_snapshot()->value("filament_map") == "1,2,3,4,5,6");
    CHECK_FALSE(library->add_active_filament(fiber_tool, "CFSYS CCF").success);
    CHECK(library->active_config()->filament_slots.size() == 6);

    // All six may carry CCF together; returning to plastic must also remain
    // possible, without leaving a sticky fiber role on any tool.
    for (unsigned slot = 0; slot < 6; ++slot)
        REQUIRE(library->set_active_filament_preset(slot, "CFSYS CCF").success);
    CHECK(library->active_config_snapshot()->value("filament_process_type") ==
        "continuous_fiber;continuous_fiber;continuous_fiber;continuous_fiber;continuous_fiber;continuous_fiber");
    for (unsigned slot = 0; slot < 6; ++slot)
        REQUIRE(library->set_active_filament_preset(slot, "CFSYS PLA").success);
    CHECK(library->active_config_snapshot()->value("filament_process_type") ==
        "thermoplastic;thermoplastic;thermoplastic;thermoplastic;thermoplastic;thermoplastic");
}

TEST_CASE("mixed-capability fiber exports preserve every physical tool ID", "[libslicer_api][p3][fiber][slice]")
{
    const unsigned fiber_tool = GENERATE(0u, 1u, 2u, 3u, 4u, 5u);
    INFO("fiber_tool=" << fiber_tool);
    libslicer::LibraryOptions options;
    options.resource_directory = LIBSLICER_TEST_RESOURCE_DIR;
    options.vendors = {"P3"};
    auto library = libslicer::Library::open(options);
    REQUIRE(library);
    libslicer::ConfigSelection selection;
    selection.machine_model_id = "P3";
    selection.machine_variant_id = "0.4";
    selection.filament_preset_ids.assign(6, "Generic PLA @P3");
    selection.filament_preset_ids[fiber_tool] = "CFSYS CCF";
    auto created = library->create_config(selection);
    for (const auto& issue : created.diagnostics) UNSCOPED_INFO(issue.message);
    REQUIRE(created.success);
    REQUIRE(created.config->apply_patch({
        {"filament_colour", "#F28E2B;#4E79A7;#59A14F;#B07AA1;#EDC948;#E15759"},
        {"generate_reinforced_perimeters", "1"}, {"outer_reinforced_perimeters_counts", "1"},
        {"reinforced_perimeters_filament", std::to_string(fiber_tool+1)},
        {"top_shell_layers", "1"}, {"bottom_shell_layers", "1"},
        {"top_shell_thickness", "0"}, {"bottom_shell_thickness", "0"},
        {"fiber_layer_height_ratio", "1"}, {"fiber_minimum_path_length", "2"}
    }).success);
    libslicer::SliceObjectInput object;
    object.name = "mixed-process-coupon";
    libslicer::SliceVolumeInput volume;
    volume.vertices = {{100,100,0},{160,100,0},{160,140,0},{100,140,0},
                       {100,100,2},{160,100,2},{160,140,2},{100,140,2}};
    volume.triangles = {{0,2,1},{0,3,2},{4,5,6},{4,6,7},{0,1,5},{0,5,4},
                       {1,2,6},{1,6,5},{2,3,7},{2,7,6},{3,0,4},{3,4,7}};
    volume.default_filament_slot = fiber_tool == 0 ? 2 : 1;
    object.volumes.push_back(std::move(volume));
    libslicer::SliceRequest request;
    request.objects = {object};
    request.config = created.config->snapshot();
    const auto output = std::filesystem::temp_directory_path() /
        ("libslicer-mixed-fiber-T" + std::to_string(fiber_tool) + ".gcode");
    std::error_code remove_error;
    std::filesystem::remove(output, remove_error);
    request.output_gcode_path = output.string();
    // P3 declares the shared-channel protocol on T5 only. Other docks may
    // select fiber material, but cannot export without an execution protocol.
    const auto unconfigured = library->slice(request);
    std::string readiness_diagnostics;
    for (const auto& issue : unconfigured.diagnostics) readiness_diagnostics += issue.message + "\n";
    INFO("unconfigured P3: " << readiness_diagnostics);
    if (fiber_tool == 5) {
        REQUIRE(unconfigured.success);
        REQUIRE(unconfigured.preview);
        size_t p3_fiber_segments = 0;
        for (const auto& segment : unconfigured.preview->segments)
            if (segment.deposition == libslicer::ToolpathDepositionKind::ContinuousFiberPowered ||
                segment.deposition == libslicer::ToolpathDepositionKind::ContinuousFiberPassive) {
                ++p3_fiber_segments;
                CHECK(segment.tool_id == 5);
                CHECK(segment.filament_id == 5);
            }
        REQUIRE(p3_fiber_segments > 0);
        CHECK(unconfigured.preview->statistics.total_fiber_feed_mm > 0);
        std::ifstream generated(output);
        REQUIRE(generated.good());
        const std::string gcode((std::istreambuf_iterator<char>(generated)), {});
        CHECK(gcode.find("; filament_diameter: 1.75,1.75,1.75,1.75,1.75,0.4\n") != std::string::npos);
        CHECK(gcode.find(";FIBER_CUT\nM400\nS0\nM400\n") != std::string::npos);
        CHECK(gcode.find("G1 E0 F0") == std::string::npos);
        CHECK(gcode.find(";P3_TOOLCHANGE_BEGIN\nM400\n;TOOL_BINDING filament=5 physical=5\nT5\nM109 S") != std::string::npos);
    } else {
        CHECK_FALSE(unconfigured.success);
        CHECK_FALSE(std::filesystem::exists(output));
        CHECK(std::any_of(unconfigured.diagnostics.begin(), unconfigured.diagnostics.end(), [](const auto& issue) {
            return issue.message.find("no fiber cut command") != std::string::npos ||
                issue.message.find("Missing continuous-fiber protocol") != std::string::npos;
        }));
    }

    // Explicit software fixture, not P3 firmware acceptance: independent
    // inventory, linear E, and a numeric test actuator, with no machine macros.
    std::vector<std::pair<std::string,std::string>> patch;
    for (const auto& [key,value] : request.config.values())
        if (key.size() >= 6 && key.compare(key.size()-6,6,"_gcode") == 0 &&
            key != "fiber_cut_gcode" && key != "emit_machine_limits_to_gcode")
            patch.emplace_back(key, "");
    patch.insert(patch.end(), {
        {"single_extruder_multi_material", "0"},
        {"toolhead_fiber_protocol_id", "linear-e-v1;linear-e-v1;linear-e-v1;linear-e-v1;linear-e-v1;linear-e-v1"},
        {"fiber_cut_gcode", "M400\nM42 P4 S255\nG4 P100\n"},
        {"fiber_cut_to_contact_length", "5"}, {"fiber_landing_length", "0"},
        {"use_relative_e_distances", "1"}
    });
    const auto patched = created.config->apply_patch(patch);
    for (const auto& issue : patched.diagnostics) UNSCOPED_INFO(issue.key << ": " << issue.message);
    REQUIRE(patched.success);
    request.config = created.config->snapshot();
    const auto sliced = library->slice(request);
    std::string execution_diagnostics;
    for (const auto& issue : sliced.diagnostics) execution_diagnostics += issue.message + "\n";
    INFO("software fixture: " << execution_diagnostics);
    REQUIRE(sliced.success);
    REQUIRE(sliced.preview);
    size_t fiber_segments = 0;
    for (const auto& segment : sliced.preview->segments) {
        if (segment.deposition == libslicer::ToolpathDepositionKind::ContinuousFiberPowered ||
            segment.deposition == libslicer::ToolpathDepositionKind::ContinuousFiberPassive) {
            ++fiber_segments;
            CHECK(segment.tool_id == fiber_tool);
            CHECK(segment.filament_id == fiber_tool);
        }
    }
    CHECK(fiber_segments > 0);
    CHECK(sliced.preview->statistics.total_fiber_feed_mm > 0);
    libslicer::GCodePreviewRequest import_request;
    import_request.gcode_path = output.string();
    const auto imported = library->load_gcode_preview(import_request);
    for (const auto& issue : imported.diagnostics) UNSCOPED_INFO(issue.message);
    REQUIRE(imported.success);
    REQUIRE(imported.preview);
    CHECK(imported.preview->statistics.total_fiber_feed_mm ==
        Catch::Approx(sliced.preview->statistics.total_fiber_feed_mm).margin(0.02));
    for (const auto& segment : imported.preview->segments)
        if (segment.deposition == libslicer::ToolpathDepositionKind::ContinuousFiberPowered ||
            segment.deposition == libslicer::ToolpathDepositionKind::ContinuousFiberPassive)
            CHECK(segment.tool_id == fiber_tool);
}

TEST_CASE("configuration exposes one grouped settings snapshot", "[libslicer_api][config]")
{
    const auto items = libslicer::Config::defaults().settings();
    REQUIRE(items.size() > 100);

    std::set<std::string> unique_keys;
    for (const auto& item : items) {
        INFO(item.key);
        CHECK(unique_keys.insert(item.key).second);
    }

    const auto* layer_height = find_item(items, "layer_height");
    REQUIRE(layer_height != nullptr);
    CHECK(layer_height->visible);
    CHECK(layer_height->group == libslicer::SettingGroup::Process);
    CHECK(layer_height->type == libslicer::SettingType::Float);
    CHECK(layer_height->element_type == libslicer::SettingType::Float);
    CHECK(layer_height->precision == 4);
    CHECK_FALSE(layer_height->default_value.empty());

    const auto* nozzle_temperature = find_item(items, "nozzle_temperature");
    REQUIRE(nozzle_temperature != nullptr);
    CHECK(nozzle_temperature->visible);
    CHECK(nozzle_temperature->group == libslicer::SettingGroup::Filament);

    const auto* nozzle_diameter = find_item(items, "nozzle_diameter");
    REQUIRE(nozzle_diameter != nullptr);
    CHECK(nozzle_diameter->visible);
    CHECK(nozzle_diameter->group == libslicer::SettingGroup::Printer);
    CHECK(nozzle_diameter->type == libslicer::SettingType::List);
    CHECK(nozzle_diameter->element_type == libslicer::SettingType::Float);
    CHECK_FALSE(nozzle_diameter->fixed_size.has_value());

    const auto* bed_type = find_item(items, "curr_bed_type");
    REQUIRE(bed_type != nullptr);
    CHECK(bed_type->group == libslicer::SettingGroup::Process);
    CHECK(bed_type->type == libslicer::SettingType::Enum);
    CHECK(bed_type->enum_items.size() == 6);
    CHECK_FALSE(bed_type->value.empty());
    // Preset-owned exclusions are hidden by default, unlike editable public settings.
    const auto* preset_owned = find_item(items, "filament_exclude_from_wipe_tower");
    REQUIRE(preset_owned != nullptr);
    CHECK_FALSE(preset_owned->visible);
    CHECK_FALSE(preset_owned->enabled);
    for(const char* key:{"fiber_infill_angle_sequence","fiber_corner_stabilization_length",
            "fiber_concentric_corner_stabilization_length","fiber_finish_extension_length",
            "fiber_finish_motion_speed","fiber_finish_overlap_length"}) {
        const auto* conditional=find_item(items,key);REQUIRE(conditional);
        CHECK_FALSE(conditional->visible);CHECK_FALSE(conditional->enabled);
    }
    CHECK(find_item(items, "missing_option") == nullptr);
}

TEST_CASE("configuration edits return only changed items and stay atomic", "[libslicer_api][config]")
{
    auto config         = libslicer::Config::defaults();
    const auto original = config.snapshot().value("layer_height");
    REQUIRE(original.has_value());

    const auto accepted = config.set("layer_height", "0.24");
    REQUIRE(accepted.success);
    REQUIRE(accepted.changed_items.size() == 1);
    CHECK(accepted.changed_items.front().key == "layer_height");
    CHECK(accepted.changed_items.front().value == "0.24");
    CHECK(accepted.changed_items.front().group == libslicer::SettingGroup::Process);
    CHECK(config.snapshot().value("layer_height") == "0.24");

    const auto no_change = config.set("layer_height", "0.24");
    REQUIRE(no_change.success);
    CHECK(no_change.changed_items.empty());

    const auto rejected = config.apply_patch({
        {"layer_height", "0.28"},
        {"missing_option", "1"},
    });
    CHECK_FALSE(rejected.success);
    CHECK(rejected.changed_items.empty());
    CHECK(config.snapshot().value("layer_height") == "0.24");

    CHECK_FALSE(config.set("layer_height", "not-a-number").success);
    CHECK(config.snapshot().value("layer_height") == "0.24");

    const auto plate_change = config.set("curr_bed_type", "Textured PEI Plate");
    REQUIRE(plate_change.success);
    REQUIRE(plate_change.changed_items.size() == 1);
    CHECK(plate_change.changed_items.front().key == "curr_bed_type");
    CHECK(config.snapshot().value("curr_bed_type") == "Textured PEI Plate");

    const auto reset = config.reset("layer_height");
    REQUIRE(reset.success);
    REQUIRE(reset.changed_items.size() == 1);
    CHECK(reset.changed_items.front().key == "layer_height");
    CHECK(reset.changed_items.front().value == original);

    const auto redundant_reset = config.reset("layer_height");
    REQUIRE(redundant_reset.success);
    CHECK(redundant_reset.changed_items.empty());

    const auto missing_reset = config.reset("missing_option");
    CHECK_FALSE(missing_reset.success);
    REQUIRE_FALSE(missing_reset.diagnostics.empty());
    CHECK_FALSE(config.snapshot().value("missing_option").has_value());
}

TEST_CASE("configuration snapshot is independent", "[libslicer_api][config]")
{
    CHECK_FALSE(libslicer::ConfigSnapshot{}.valid());
    auto config = libslicer::Config::defaults();
    REQUIRE(config.set("layer_height", "0.18").success);
    const auto snapshot = config.snapshot();
    CHECK(snapshot.valid());

    REQUIRE(config.set("layer_height", "0.22").success);
    CHECK(snapshot.value("layer_height") == "0.18");
    CHECK(config.snapshot().value("layer_height") == "0.22");
    CHECK_FALSE(snapshot.value("missing_option").has_value());
}

TEST_CASE("fiber width is one positive millimeter setting", "[libslicer_api][config][fiber-width]")
{
    auto config = libslicer::Config::defaults();
    auto items = config.settings();
    const auto* width = find_item(items, "fiber_width");
    REQUIRE(width != nullptr);
    CHECK(width->type == libslicer::SettingType::Float);
    CHECK(width->unit == "mm");
    CHECK(width->group == libslicer::SettingGroup::Process);
    CHECK(width->level == libslicer::SettingLevel::Simple);
    CHECK_FALSE(width->enabled);
    CHECK(find_item(items, "reinforced_perimeters_extrusion_width") == nullptr);
    CHECK(find_item(items, "reinforced_infill_extrusion_width") == nullptr);
    REQUIRE(config.set("generate_reinforced_perimeters", "1").success);
    items = config.settings();
    CHECK(find_item(items, "fiber_width")->enabled);
    REQUIRE(config.set("generate_reinforced_perimeters", "0").success);
    REQUIRE(config.set("generate_reinforced_infills", "1").success);
    items = config.settings();
    CHECK(find_item(items, "fiber_width")->enabled);
    REQUIRE(config.set("fiber_width", "1.2").success);
    for (const auto* invalid : {"0", "-1", "100%", "1mm", "nan", "inf"}) {
        INFO(invalid);
        CHECK_FALSE(config.set("fiber_width", invalid).success);
        CHECK(config.snapshot().value("fiber_width") == "1.2");
    }
}

TEST_CASE("fiber first approach speed is a global export setting", "[libslicer_api][config][fiber-approach]")
{
    auto config = libslicer::Config::defaults();
    auto items = config.settings();
    const auto* speed = find_item(items, "fiber_toolchange_approach_speed");
    REQUIRE(speed);
    CHECK(speed->type == libslicer::SettingType::Float);
    CHECK(speed->unit == "mm/s");
    CHECK(speed->group == libslicer::SettingGroup::Process);
    CHECK_FALSE(speed->enabled);
    CHECK(config.snapshot().value("fiber_toolchange_approach_speed") == "0");
    REQUIRE(config.set("generate_reinforced_infills", "1").success);
    items = config.settings();
    CHECK(find_item(items, "fiber_toolchange_approach_speed")->enabled);
    REQUIRE(config.set("fiber_toolchange_approach_speed", "60").success);
    for (const auto* invalid : {"-1", "nan", "inf", "60mm/s", "60%"}) {
        CAPTURE(invalid);
        CHECK_FALSE(config.set("fiber_toolchange_approach_speed", invalid).success);
        CHECK(config.snapshot().value("fiber_toolchange_approach_speed") == "60");
    }
    auto mask = libslicer::Config::for_overrides(config.snapshot(), libslicer::ConfigScope::FiberMask);
    CHECK_FALSE(mask.set("fiber_toolchange_approach_speed", "40").success);
    REQUIRE(config.set("fiber_toolchange_approach_speed", "0").success);
}

TEST_CASE("CFSYS process inheritance retains the first approach speed", "[libslicer_api][config][fiber-approach]")
{
    libslicer::LibraryOptions options;
    options.resource_directory = LIBSLICER_TEST_RESOURCE_DIR;
    options.vendors = {"CFSYS"};
    auto library = libslicer::Library::open(options);
    libslicer::ConfigSelection selection;
    selection.machine_model_id = "CFSYS Alpha500 Printer";
    selection.machine_variant_id = "0.4";
    selection.process_preset_id = "CCF&CIRON @CFSYS";
    selection.filament_preset_ids = {"CFSYS CIRON", "CFSYS CCF", "CFSYS CIRON", "CFSYS CIRON", "CFSYS CIRON"};
    selection.filament_physical_tools = {0, 1, 0, 0, 0};
    auto created = library->create_config(selection);
    REQUIRE(created);
    CHECK(created.config->snapshot().value("fiber_toolchange_approach_speed") == "60");
    REQUIRE(created.config->set("fiber_toolchange_approach_speed", "0").success);
    CHECK(created.config->snapshot().value("fiber_toolchange_approach_speed") == "0");
}

TEST_CASE("continuous fiber settings expose their UI dependencies", "[libslicer_api][config][fiber]")
{
    auto config = libslicer::Config::defaults();
    auto items = config.settings();

    const auto* contour_toggle = find_item(items, "generate_reinforced_perimeters");
    const auto* contour_count = find_item(items, "outer_reinforced_perimeters_counts");
    const auto* infill_density = find_item(items, "reinforced_infill_density");
    const auto* layer_interval = find_item(items, "fiber_layer_height_ratio");
    const auto* fill_debug = find_item(items, "fiber_fill_debug");
    REQUIRE(fill_debug != nullptr);
    CHECK(fill_debug->type == libslicer::SettingType::Boolean);
    CHECK(fill_debug->group == libslicer::SettingGroup::Process);
    CHECK(fill_debug->level == libslicer::SettingLevel::Simple);
    CHECK(fill_debug->category == "Continuous fiber");
    CHECK(fill_debug->visible);
    CHECK(fill_debug->default_value == "0");
    CHECK_FALSE(fill_debug->enabled);
    CHECK(find_item(items, "fiber_minimum_segment_length") == nullptr);
    CHECK(find_item(items, "fiber_maximum_turn_angle") == nullptr);
    const auto* contour_infill_clearance = find_item(items, "fiber_contour_infill_clearance");
    REQUIRE(contour_toggle != nullptr);
    REQUIRE(contour_count != nullptr);
    REQUIRE(infill_density != nullptr);
    REQUIRE(layer_interval != nullptr);
    REQUIRE(contour_infill_clearance != nullptr);
    CHECK(contour_toggle->enabled);
    CHECK_FALSE(contour_count->enabled);
    CHECK_FALSE(infill_density->enabled);
    CHECK_FALSE(layer_interval->enabled);
    CHECK_FALSE(contour_infill_clearance->enabled);

    const auto contour_enabled = config.set("generate_reinforced_perimeters", "1");
    REQUIRE(contour_enabled.success);
    CHECK(find_item(contour_enabled.changed_items, "generate_reinforced_perimeters") != nullptr);
    CHECK(find_item(contour_enabled.changed_items, "outer_reinforced_perimeters_counts") != nullptr);
    CHECK(find_item(contour_enabled.changed_items, "fiber_layer_height_ratio") != nullptr);

    items = config.settings();
    CHECK(find_item(items, "outer_reinforced_perimeters_counts")->enabled);
    CHECK(find_item(items, "fiber_layer_height_ratio")->enabled);
    CHECK(find_item(items, "fiber_fill_debug")->enabled);
    REQUIRE(config.set("fiber_fill_debug", "1").success);
    const auto debug_snapshot = config.snapshot();
    REQUIRE(config.reset("fiber_fill_debug").success);
    CHECK(debug_snapshot.value("fiber_fill_debug") == "1");
    CHECK(config.snapshot().value("fiber_fill_debug") == "0");
    CHECK_FALSE(find_item(items, "reinforced_infill_density")->enabled);
    CHECK_FALSE(find_item(items, "fiber_contour_infill_clearance")->enabled);

    const auto infill_enabled = config.set("generate_reinforced_infills", "1");
    REQUIRE(infill_enabled.success);
    CHECK(find_item(infill_enabled.changed_items, "reinforced_infill_density") != nullptr);
    CHECK(find_item(infill_enabled.changed_items, "fiber_contour_infill_clearance") != nullptr);

    items = config.settings();
    CHECK(find_item(items, "reinforced_infill_density")->enabled);
    CHECK(find_item(items, "fiber_contour_infill_clearance")->enabled);
}

TEST_CASE("CFSYS profiles expose the canonical continuous fiber contract", "[libslicer_api][config][fiber][cfsys]")
{
    libslicer::LibraryOptions options;
    options.resource_directory = LIBSLICER_TEST_RESOURCE_DIR;
    options.vendors = {"CFSYS"};
    auto library = libslicer::Library::open(options);
    REQUIRE(library != nullptr);

    libslicer::ConfigSelection selection;
    selection.machine_model_id = "CFSYS Alpha500 Printer";
    selection.machine_variant_id = "0.4";
    const auto activated = library->activate_config(selection);
    const std::string diagnostic = activated.diagnostics.empty()
        ? std::string{}
        : activated.diagnostics.front().message;
    INFO(diagnostic);
    REQUIRE(activated.success);

    const auto config = library->active_config_snapshot();
    REQUIRE(config.has_value());
    const auto cut_gcode = config->value("fiber_cut_gcode");
    const auto cut_to_contact = config->value("fiber_cut_to_contact_length");
    const auto prefeed_extra = config->value("fiber_prefeed_extra_length");
    const auto z_hop_height = config->value("fiber_z_hop_height");
    const auto landing_length = config->value("fiber_landing_length");
    const auto landing_speed = config->value("fiber_landing_speed");
    const auto start_speed = config->value("fiber_start_speed");
    const auto minimum_effective = config->value("fiber_minimum_effective_length");
    const auto contour_speed = config->value("fiber_contour_max_speed");
    const auto infill_speed = config->value("fiber_infill_max_speed");
    const auto contour_acceleration = config->value("fiber_contour_acceleration");
    const auto infill_acceleration = config->value("fiber_infill_acceleration");
    REQUIRE(cut_gcode.has_value());
    REQUIRE(cut_to_contact.has_value());
    REQUIRE(prefeed_extra.has_value());
    REQUIRE(z_hop_height.has_value());
    REQUIRE(landing_length.has_value());
    REQUIRE(landing_speed.has_value());
    REQUIRE(start_speed.has_value());
    REQUIRE(minimum_effective.has_value());
    REQUIRE(contour_speed.has_value());
    REQUIRE(infill_speed.has_value());
    REQUIRE(contour_acceleration.has_value());
    REQUIRE(infill_acceleration.has_value());
    // ConfigSnapshot exposes the serialized representation; multiline strings
    // therefore contain C-style escaped newlines until a slice config is built.
    CHECK(*cut_gcode == "M400\\nS0\\nM400\\n");
    CHECK(*cut_to_contact == "23");
    CHECK(*prefeed_extra == "0.05");
    CHECK(*z_hop_height == "2");
    CHECK(*landing_length == "0.2");
    CHECK_FALSE(config->value("fiber_start_length").has_value());
    CHECK(*landing_speed == "3");
    CHECK(*start_speed == "10");
    CHECK(*minimum_effective == "0.5");
    CHECK(config->value("fiber_minimum_path_length") == std::optional<std::string>{"0"});
    CHECK(config->value("fiber_contour_boundary_clearance") == "0.05");
    CHECK(config->value("fiber_contour_include_holes") == "0");
    CHECK(*contour_speed == "10");
    CHECK(*infill_speed == "10");
    CHECK(*contour_acceleration == "500");
    CHECK(*infill_acceleration == "500");
    CHECK(config->value("physical_extruder_map") == std::optional<std::string>{"0,1"});
    CHECK(config->value("toolhead_fiber_protocol_id") == std::optional<std::string>{";cfsys-v1"});

    // The new algorithm has one canonical configuration contract. Historical
    // CFSYS names must not survive as runtime aliases.
    CHECK_FALSE(config->value("fibercut_length").has_value());
    CHECK_FALSE(config->value("fiber_restart_extra_length").has_value());
    CHECK_FALSE(config->value("fiber_z_hop").has_value());
    CHECK_FALSE(config->value("fiber_start_min_length").has_value());
    CHECK_FALSE(config->value("fiber_normal_max_speed").has_value());
    CHECK_FALSE(config->value("fiber_perimeter_acceleration").has_value());
}

TEST_CASE("CFSYS material slots are explicitly owned by physical tools", "[libslicer_api][cfsys][tool-materials]")
{
    libslicer::LibraryOptions options;
    options.resource_directory = LIBSLICER_TEST_RESOURCE_DIR;
    options.vendors = {"CFSYS"};
    auto library = libslicer::Library::open(options);
    REQUIRE(library);
    libslicer::ConfigSelection selection;
    selection.machine_model_id = "CFSYS Alpha500 Printer";
    selection.machine_variant_id = "0.4";
    selection.process_preset_id = "CCF&CIRON @CFSYS";
    selection.filament_preset_ids = {"CFSYS PLA", "CFSYS CCF"};
    const auto activated = library->activate_config(selection);
    for (const auto& diagnostic : activated.diagnostics) INFO(diagnostic.message);
    REQUIRE(activated.success);
    REQUIRE(activated.view.filament_slots.size() == 2);
    const auto contains = [](const auto& options, const std::string& id) {
        return std::any_of(options.begin(), options.end(), [&id](const auto& value) { return value.id == id; });
    };
    CHECK(contains(activated.view.filament_slots[0].compatible_presets, "CFSYS PLA"));
    CHECK_FALSE(contains(activated.view.filament_slots[0].compatible_presets, "CFSYS CCF"));
    CHECK(contains(activated.view.filament_slots[1].compatible_presets, "CFSYS CCF"));
    CHECK_FALSE(contains(activated.view.filament_slots[1].compatible_presets, "CFSYS PLA"));
    const auto initial_revision = activated.view.revision;
    CHECK_FALSE(library->set_active_filament_preset(0, "CFSYS CCF").success);
    CHECK_FALSE(library->set_active_filament_preset(1, "CFSYS PLA").success);
    CHECK_FALSE(library->add_active_filament(1).success);
    CHECK_FALSE(library->add_active_filament(0, "CFSYS CCF").success);
    CHECK_FALSE(library->resize_active_filament_slots(3).success);
    CHECK(library->active_config()->revision == initial_revision);
    REQUIRE(library->set_active_config_value("layer_height", "0.15").success);
    REQUIRE(library->set_active_filament_color(0, {20, 40, 60, 255}).success);
    for (size_t count = 3; count <= 5; ++count) {
        const auto added = library->add_active_filament(0, "CFSYS PLA");
        for (const auto& diagnostic : added.diagnostics) INFO(diagnostic.message);
        REQUIRE(added.success);
        REQUIRE(added.view.filament_slots.size() == count);
        CHECK(added.view.filament_slots.back().physical_tool_index == 0);
        CHECK(added.view.filament_slots[1].physical_tool_index == 1);
        CHECK(added.view.filament_slots[0].color.red == 20);
        CHECK(library->active_config_snapshot()->value("layer_height") == "0.15");
    }
    CHECK_FALSE(library->add_active_filament(0).success);
    CHECK(library->active_config_snapshot()->value("filament_map") == "1,2,1,1,1");
    CHECK(library->validate_active_config().empty());

    SECTION("arbitrary explicit material order is retained") {
        auto second = libslicer::Library::open(options);
        selection.filament_preset_ids = {"CFSYS PLA", "CFSYS PLA", "CFSYS CCF"};
        selection.filament_physical_tools = {0, 0, 1};
        const auto reordered = second->activate_config(selection, {
            {"reinforced_perimeters_filament", "3"}, {"reinforced_infill_filament", "3"}});
        for (const auto& diagnostic : reordered.diagnostics) INFO(diagnostic.message);
        REQUIRE(reordered.success);
        CHECK(second->active_config_snapshot()->value("filament_map") == "1,1,2");
        CHECK(reordered.view.filament_slots[2].physical_tool_index == 1);
        CHECK_FALSE(second->set_active_filament_preset(2, "CFSYS PLA").success);
    }
}

TEST_CASE("CFSYS original machine scripts produce real fiber G-code and reimport", "[libslicer_api][fiber][cfsys][slice]")
{
    const unsigned fiber_slot = GENERATE(0u, 1u, 2u, 4u);
    INFO("fiber material index=" << fiber_slot);
    // Alpha500 is the machine registered by the current CFSYS vendor catalog.
    const std::string machine = "CFSYS Alpha500 Printer";
    INFO(machine);
    libslicer::LibraryOptions options;
    options.resource_directory = LIBSLICER_TEST_RESOURCE_DIR;
    options.vendors = {"CFSYS"};
    auto library = libslicer::Library::open(options);
    REQUIRE(library);
    libslicer::ConfigSelection selection;
    selection.machine_model_id = machine;
    selection.machine_variant_id = "0.4";
    selection.process_preset_id = "CCF&CIRON @CFSYS";
    const unsigned material_count = std::max(2u, fiber_slot + 1);
    selection.filament_preset_ids.assign(material_count, "CFSYS PLA");
    selection.filament_preset_ids[fiber_slot] = "CFSYS CCF";
    selection.filament_physical_tools.assign(material_count, 0);
    selection.filament_physical_tools[fiber_slot] = 1;
    const auto activated = library->activate_config(selection, {
        {"reinforced_perimeters_filament", std::to_string(fiber_slot + 1)},
        {"reinforced_infill_filament", std::to_string(fiber_slot + 1)},
        {"outer_wall_filament_id", fiber_slot == 0 ? "2" : "1"},
        {"inner_wall_filament_id", fiber_slot == 0 ? "2" : "1"},
        {"sparse_infill_filament_id", fiber_slot != 1 ? "2" : "1"},
        {"internal_solid_filament_id", fiber_slot != 1 ? "2" : "1"},
        {"top_surface_filament_id", fiber_slot != 1 ? "2" : "1"},
        {"bottom_surface_filament_id", fiber_slot != 1 ? "2" : "1"}});
    std::string activation_diagnostics;
    for (const auto& issue : activated.diagnostics) activation_diagnostics += issue.key + ": " + issue.message + "\n";
    INFO(activation_diagnostics);
    REQUIRE(activated.success);
    std::optional<double> reference_feed;
    std::optional<ActualFiberFeed> reference_actual_feed;
    for (bool relative_e : {false, true}) {
        INFO("relative_e=" << relative_e);
        REQUIRE(library->apply_active_config_patch({{"use_relative_e_distances", relative_e ? "1" : "0"}}).success);
        libslicer::SliceRequest request;
        const char* model = std::getenv("LIBSLICER_FIBER_VALIDATION_MODEL");
        if (model) request.objects.push_back(binary_stl_on_bed(model));
        else request.objects = {{std::string(LIBSLICER_TEST_DATA_DIR) + "/two_20mm_cubes.obj", {}}};
        request.config = *library->active_config_snapshot();
        const auto result = library->slice(request);
        std::string diagnostics;
        for (const auto& issue : result.diagnostics) diagnostics += issue.message + "\n";
        INFO(diagnostics);
        REQUIRE(result.success);
        REQUIRE(result.preview);
        REQUIRE(result.preview->statistics.total_fiber_feed_mm > 0);
        std::size_t powered = 0, passive = 0, finish = 0;
        std::set<unsigned> resin_materials;
        for (const auto& segment : result.preview->segments) {
            if (segment.deposition == libslicer::ToolpathDepositionKind::Thermoplastic) {
                CHECK(segment.tool_id == 0);
                resin_materials.insert(segment.filament_id);
            }
            if (segment.deposition == libslicer::ToolpathDepositionKind::ContinuousFiberPowered) {
                ++powered;
                REQUIRE(segment.tool_id == 1);
                REQUIRE(segment.filament_id == fiber_slot);
                REQUIRE(segment.fiber_feed_delta_mm > 0);
                REQUIRE(segment.end_mm.z > 0);
            }
            if (segment.deposition == libslicer::ToolpathDepositionKind::ContinuousFiberPassive) {
                ++passive;
                REQUIRE(segment.width_mm > 0);
                REQUIRE(segment.height_mm > 0);
                REQUIRE(segment.fiber_feed_delta_mm == 0);
            }
            if (segment.fiber_phase == libslicer::ToolpathFiberPhase::Finish) {
                ++finish;
                REQUIRE(segment.deposition == libslicer::ToolpathDepositionKind::None);
            }
        }
        REQUIRE(powered > 0);
        if (fiber_slot > 1) CHECK(resin_materials.count(1) > 0);
        REQUIRE(passive > 0);
        REQUIRE(finish > 0);
        std::ifstream input(result.output.path);
        const std::string gcode((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        const auto first_block = gcode.find(";FIBER_BEGIN");
        REQUIRE(first_block != std::string::npos);
        CHECK(gcode.find(machine == "CFSYS Alpha500 Printer" ? "DF1004" : "PRINT_START ") < first_block);
        CHECK(gcode.find("PRINT_END") > gcode.rfind(";FIBER_END"));
        std::size_t cursor = 0, cuts = 0;
        const std::string expected_cut = ";FIBER_CUT\nM400\nS0\nM400\n;FIBER_TAIL_BEGIN\n";
        while ((cursor = gcode.find(";FIBER_CUT\n", cursor)) != std::string::npos) {
            REQUIRE(gcode.compare(cursor, expected_cut.size(), expected_cut) == 0);
            ++cuts;
            cursor += 11;
        }
        REQUIRE(cuts > 0);
        const auto actual_feed=actual_fiber_feed(gcode);
        CHECK(actual_feed.path_count==cuts);
        CHECK(actual_feed.total_mm==Catch::Approx(result.preview->statistics.total_fiber_feed_mm).margin(.02));
        if(reference_actual_feed) {
            CHECK(actual_feed.path_count==reference_actual_feed->path_count);
            CHECK(actual_feed.command_count==reference_actual_feed->command_count);
            CHECK(actual_feed.total_mm==Catch::Approx(reference_actual_feed->total_mm).margin(.05));
            std::cout<<"[FiberFeedQuantization] slot="<<fiber_slot<<" absolute="<<std::setprecision(12)
                <<reference_actual_feed->total_mm<<" relative="<<actual_feed.total_mm
                <<" difference="<<actual_feed.total_mm-reference_actual_feed->total_mm<<'\n';
        } else reference_actual_feed=actual_feed;
        std::istringstream commands(gcode);
        std::string line;
        double acceleration = 0;
        while (std::getline(commands, line)) {
            const auto setting = line.find("ACCEL=");
            if (line.rfind("SET_VELOCITY_LIMIT", 0) == 0 && setting != std::string::npos)
                acceleration = std::stod(line.substr(setting+6));
            if (line == ";FIBER_LANDING_BEGIN" || line == ";FIBER_START" || line == ";FIBER_TAIL_BEGIN")
                REQUIRE(acceleration == Catch::Approx(500.0));
        }
        libslicer::GCodePreviewRequest import_request;
        import_request.gcode_path = result.output.path;
        const auto imported = library->load_gcode_preview(import_request);
        REQUIRE(imported.success);
        REQUIRE(imported.preview);
        CHECK(imported.preview->statistics.total_fiber_feed_mm ==
              Catch::Approx(result.preview->statistics.total_fiber_feed_mm).margin(0.02));
        CHECK(imported.preview->statistics.total_fiber_deposited_path_mm ==
              Catch::Approx(result.preview->statistics.total_fiber_deposited_path_mm).margin(0.02));
        if (reference_feed)
            CHECK(result.preview->statistics.total_fiber_feed_mm == Catch::Approx(*reference_feed).margin(0.05));
        else reference_feed = result.preview->statistics.total_fiber_feed_mm;
        std::error_code error;
        std::filesystem::remove(result.output.path, error);
    }
}

TEST_CASE("CFSYS reference model retains reinforced contours on every reference layer", "[libslicer_api][fiber][reference]")
{
    const char* model = std::getenv("LIBSLICER_FIBER_VALIDATION_MODEL");
    const char* reference = std::getenv("LIBSLICER_FIBER_REFERENCE_GCODE");
    if (!model || !reference) SKIP("Supply the reference STL and G-code to run the stage-one acceptance test");
    std::ifstream input(reference);
    REQUIRE(input.good());
    std::map<size_t,size_t> reference_contours;
    std::string line, role;
    size_t layers = 0;
    while (std::getline(input,line)) {
        if (line == ";LAYER_CHANGE") ++layers;
        if (line.rfind(";TYPE:",0) == 0) role = line.substr(6);
        if (line == "S0" && role == "Fiber wall") ++reference_contours[layers];
    }
    REQUIRE_FALSE(reference_contours.empty());
    libslicer::LibraryOptions options;
    options.resource_directory = LIBSLICER_TEST_RESOURCE_DIR;
    options.vendors = {"CFSYS"};
    auto library = libslicer::Library::open(options);
    REQUIRE(library);
    libslicer::ConfigSelection selection;
    selection.machine_model_id = "CFSYS Alpha500 Printer";
    selection.machine_variant_id = "0.4";
    selection.process_preset_id = "CCF&CIRON @CFSYS";
    selection.filament_preset_ids = {"CFSYS PLA", "CFSYS CCF"};
    REQUIRE(library->activate_config(selection).success);
    REQUIRE(library->apply_active_config_patch({{"generate_reinforced_infills","1"},
        {"reinforced_infill_density","100%"},{"curr_bed_type","High Temp Plate"},
        {"precise_outer_wall","0"},{"wall_direction","auto"}}).success);
    libslicer::SliceRequest request;
    request.objects.push_back(binary_stl_on_bed(model));
    request.config = *library->active_config_snapshot();
    if (const char* output = std::getenv("LIBSLICER_FIBER_VALIDATION_OUTPUT")) request.output_gcode_path = output;
    const auto result = library->slice(request);
    for (const auto& d : result.diagnostics) INFO(d.message);
    REQUIRE(result.success);
    REQUIRE(result.summary.layer_count == layers);
    std::ifstream generated(result.output.path);
    std::map<size_t,size_t> contours;
    const std::regex occurrence("^;FIBER_BEGIN .* layer=([0-9]+) .*purpose=contour ");
    std::smatch match;
    while (std::getline(generated,line))
        if (std::regex_search(line,match,occurrence)) ++contours[std::stoul(match[1])+1];
    for (const auto& expected : reference_contours) {
        INFO("Reference layer=" << expected.first << ", reference contours=" << expected.second);
        CHECK(contours[expected.first] >= expected.second);
    }
    if (request.output_gcode_path.empty()) {
        std::error_code error; std::filesystem::remove(result.output.path,error);
    }
}

namespace {
libslicer::SliceObjectInput fiber_infill_block()
{
    libslicer::SliceObjectInput object;
    object.name = "fiber-infill-block";
    libslicer::SliceVolumeInput volume;
    volume.vertices = {{100,100,0},{160,100,0},{160,140,0},{100,140,0},
                       {100,100,8},{160,100,8},{160,140,8},{100,140,8}};
    volume.triangles = {{0,2,1},{0,3,2},{4,5,6},{4,6,7},{0,1,5},{0,5,4},
                        {1,2,6},{1,6,5},{2,3,7},{2,7,6},{3,0,4},{3,4,7}};
    object.volumes.push_back(std::move(volume));
    return object;
}
}

TEST_CASE("fiber width reaches both path roles and preview independently of resin width", "[libslicer_api][fiber-width][slice]")
{
    libslicer::LibraryOptions options;
    options.resource_directory = LIBSLICER_TEST_RESOURCE_DIR;
    options.vendors = {"CFSYS"};
    auto library = libslicer::Library::open(options);
    REQUIRE(library);
    libslicer::ConfigSelection selection;
    selection.machine_model_id = "CFSYS Alpha500 Printer";
    selection.machine_variant_id = "0.4";
    selection.process_preset_id = "CCF&CIRON @CFSYS";
    selection.filament_preset_ids = {"CFSYS CIRON", "CFSYS CCF"};
    selection.filament_physical_tools = {0, 1};
    REQUIRE(library->activate_config(selection, {
        {"generate_reinforced_perimeters", "1"}, {"generate_reinforced_infills", "1"},
        {"reinforced_infill_density", "100%"}, {"infill_direction", "0"},
        {"reinforced_infill_pattern", "rectilinear"}, {"fiber_infill_angle_sequence", "0"},
        {"fiber_infill_bend_radius", "0.3"},
        {"sparse_infill_rotate_template", "0"}}).success);
    for (const auto* width : {"0.8", "1.2"}) {
        std::optional<double> reference_pitch;
        for (const auto* resin_width : {"0.42", "0.64"}) {
            INFO("fiber=" << width << " resin=" << resin_width);
            REQUIRE(library->apply_active_config_patch({{"fiber_width", width},
                {"line_width", resin_width}, {"sparse_infill_line_width", resin_width},
                {"fiber_resin_fill_line_width", resin_width}}).success);
            libslicer::SliceRequest request;
            request.config = *library->active_config_snapshot();
            request.objects.push_back(fiber_infill_block());
            const auto sliced = library->slice(request);
            std::string slice_diagnostics;
            for (const auto& d : sliced.diagnostics) slice_diagnostics += d.message + "\n";
            INFO(slice_diagnostics);
            REQUIRE(sliced.success);
            REQUIRE(sliced.preview);
            const auto check_widths = [&](const auto& preview) {
                size_t contours = 0, infills = 0;
                for (const auto& segment : preview.segments) {
                    const auto role = segment.extrusion_role;
                    if (role != libslicer::ToolpathExtrusionRole::ContinuousFiberContour &&
                        role != libslicer::ToolpathExtrusionRole::ContinuousFiberInfill) continue;
                    REQUIRE(segment.width_mm == Catch::Approx(std::stod(width)).margin(1e-5));
                    if (role == libslicer::ToolpathExtrusionRole::ContinuousFiberContour) ++contours;
                    else ++infills;
                }
                REQUIRE(contours > 0);
                REQUIRE(infills > 0);
            };
            check_widths(*sliced.preview);
            const auto imported = library->load_gcode_preview({sliced.output.path});
            REQUIRE(imported.success);
            REQUIRE(imported.preview);
            check_widths(*imported.preview);
            // Sample parallel straight infill runs in each layer. Command subdivision
            // and process phases may split each run into several preview segments.
            std::map<std::pair<unsigned, bool>, std::set<long long>> lines;
            for (const auto& segment : sliced.preview->segments) {
                if (segment.extrusion_role != libslicer::ToolpathExtrusionRole::ContinuousFiberInfill) continue;
                const auto& a = segment.start_mm;
                const auto& b = segment.end_mm;
                if (std::abs(a.x-b.x) < 1e-4 && std::abs(a.y-b.y) > 1)
                    lines[{segment.layer_index, true}].insert(std::llround(a.x*10000));
                if (std::abs(a.y-b.y) < 1e-4 && std::abs(a.x-b.x) > 1)
                    lines[{segment.layer_index, false}].insert(std::llround(a.y*10000));
            }
            size_t measured = 0;
            for (const auto& [layer, positions] : lines) {
                if (positions.size() < 10) continue;
                std::vector<double> pitches;
                for (auto it = std::next(positions.begin()); it != positions.end(); ++it)
                    pitches.push_back(double(*it-*std::prev(it))/10000.0);
                std::sort(pitches.begin(), pitches.end());
                const double pitch = pitches[pitches.size()/2];
                CHECK(pitch == Catch::Approx(std::stod(width)).margin(0.002));
                if (reference_pitch) CHECK(pitch == Catch::Approx(*reference_pitch).margin(0.002));
                else reference_pitch = pitch;
                ++measured;
            }
            REQUIRE(measured > 0);
            std::error_code error;
            std::filesystem::remove(sliced.output.path, error);
        }
    }
}

TEST_CASE("ordinary multi-tool printing uses configured clearance without fiber", "[libslicer_api][machine-gcode][slice]")
{
    const bool tower = GENERATE(false, true);
    const auto tower_type = GENERATE(std::string("type1"), std::string("type2"));
    CAPTURE(tower, tower_type);
    libslicer::LibraryOptions options;
    options.resource_directory = LIBSLICER_TEST_RESOURCE_DIR;
    options.vendors = {"Flashforge"};
    auto library = libslicer::Library::open(options);
    REQUIRE(library);
    libslicer::ConfigSelection selection;
    selection.machine_model_id = "Flashforge Creator 5";
    selection.machine_variant_id = "0.4";
    auto created = library->create_config(selection);
    REQUIRE(created.success);
    REQUIRE(created.config->snapshot().value("toolchange_z_lift") == "0");
    REQUIRE(created.config->snapshot().value("part_cooling_fan_index") == "-1");
    REQUIRE(created.config->apply_patch({
        {"toolchange_z_lift", "7.5"}, {"part_cooling_fan_index", "4"},
        {"filament_colour", "#FFFFFF;#000000;#FF0000;#0000FF"},
        {"enable_prime_tower", tower ? "1" : "0"}, {"wipe_tower_type", tower_type},
        {"wipe_tower_x", "40"}, {"wipe_tower_y", "40"}, {"change_filament_gcode", ""},
        {"filament_start_gcode", ";;;"}, {"filament_end_gcode", ";;;"},
        {"outer_wall_filament_id", "1"}, {"inner_wall_filament_id", "1"},
        {"sparse_infill_filament_id", "2"}, {"internal_solid_filament_id", "2"}, {"gcode_comments", "1"}}).success);
    libslicer::SliceRequest request;
    request.config = created.config->snapshot();
    request.objects.push_back(fiber_infill_block());
    const auto sliced = library->slice(request);
    std::ostringstream diagnostics;
    for (const auto &d : sliced.diagnostics) diagnostics << d.message << "\n";
    INFO(diagnostics.str());
    REQUIRE(sliced.success);
    REQUIRE(sliced.preview);
    CHECK(std::any_of(sliced.preview->segments.begin(), sliced.preview->segments.end(), [](const auto& segment) {
        return segment.extrusion_role == libslicer::ToolpathExtrusionRole::WipeTower &&
            segment.motion == libslicer::ToolpathMotionKind::Extrusion;
    }) == tower);
    std::ifstream input(sliced.output.path);
    std::string line;
    double z = 0, layer_z = 0;
    std::optional<double> restore;
    size_t protected_changes = 0;
    while (std::getline(input, line)) {
        if (line.rfind(";Z:", 0) == 0) layer_z = std::stod(line.substr(3));
        REQUIRE(line.rfind(";FIBER_BEGIN", 0) != 0);
        // Type1 may retain Orca's spiral Z-hop (G2/G3) before the physical switch.
        std::istringstream words(line.substr(0, line.find(';')));
        std::string command;
        words >> command;
        bool has_z = false;
        double next_z = z;
        if (command == "G0" || command == "G1" || command == "G2" || command == "G3")
            for (std::string word; words >> word;)
                if (word.size() > 1 && word[0] == 'Z') {
                    has_z = true;
                    next_z = std::stod(word.substr(1));
                }
        if (has_z) {
            if (line.find("toolchange clearance") != std::string::npos) {
                REQUIRE_FALSE(restore.has_value());
                restore = std::max(z, layer_z);
                CHECK(next_z - *restore == Catch::Approx(7.5).margin(0.001));
                ++protected_changes;
            } else if (line.find("restore Z after toolchange") != std::string::npos) {
                REQUIRE(restore.has_value());
                CHECK(next_z == Catch::Approx(*restore).margin(0.001));
                restore.reset();
            }
            z = next_z;
        }
    }
    CHECK(protected_changes > 1);
    CHECK_FALSE(restore.has_value());
    std::filesystem::remove(sliced.output.path);
}

TEST_CASE("A500 wipe towers preserve deposition while protecting physical tool changes",
          "[libslicer_api][machine-gcode][wipe-tower-clearance]")
{
    const auto tower_type = GENERATE(std::string("type1"), std::string("type2"));
    CAPTURE(tower_type);
    libslicer::LibraryOptions options;
    options.resource_directory = LIBSLICER_TEST_RESOURCE_DIR;
    options.vendors = {"CFSYS"};
    auto library = libslicer::Library::open(options);
    REQUIRE(library);
    libslicer::ConfigSelection selection;
    selection.machine_model_id = "CFSYS Alpha500 Printer";
    selection.machine_variant_id = "0.4";
    selection.process_preset_id = "CCF&CIRON @CFSYS";
    selection.filament_preset_ids = {"CFSYS CIRON", "CFSYS CCF"};
    selection.filament_physical_tools = {0, 1};
    REQUIRE(library->activate_config(selection, {
        {"toolchange_z_lift", "0"}, {"enable_prime_tower", "1"},
        {"wipe_tower_type", tower_type}, {"wipe_tower_wall_type", "rectangle"},
        {"wipe_tower_x", "40"}, {"wipe_tower_y", "40"},
        {"curr_bed_type", "Engineering Plate"}, {"enable_support", "0"},
        {"enable_arc_fitting", "0"}, {"gcode_comments", "1"},
        {"generate_reinforced_perimeters", "1"},
        {"generate_reinforced_infills", "0"},
        {"outer_wall_filament_id", "1"}, {"inner_wall_filament_id", "1"},
        {"sparse_infill_filament_id", "1"}, {"internal_solid_filament_id", "1"}
    }).success);

    // Rejected edits are atomic, including revision and presentation changes.
    const auto before = *library->active_config_snapshot();
    const auto revision = library->active_config()->revision;
    const auto rejected = library->set_active_config_value("toolchange_z_lift", "-1");
    CHECK_FALSE(rejected.success);
    CHECK(rejected.changed_items.empty());
    CHECK(library->active_config_snapshot()->values() == before.values());
    CHECK(library->active_config()->revision == revision);

    std::vector<std::string> baseline_deposition;
    std::vector<int> baseline_changes;
    for (const int clearance : {0, 10}) {
        REQUIRE(library->set_active_config_value("toolchange_z_lift", std::to_string(clearance)).success);
        libslicer::SliceRequest request;
        request.config = *library->active_config_snapshot();
        request.objects.push_back(fiber_infill_block());
        const auto result = library->slice(request);
        for (const auto& d : result.diagnostics) INFO(d.code << ": " << d.message);
        REQUIRE(result.success);
        REQUIRE(result.preview);
        size_t tower_segments = 0;
        for (const auto& segment : result.preview->segments)
            if (segment.extrusion_role == libslicer::ToolpathExtrusionRole::WipeTower &&
                segment.motion == libslicer::ToolpathMotionKind::Extrusion) {
                ++tower_segments;
                CHECK(segment.tool_id == 0);
            }
        REQUIRE(tower_segments > 0);
        std::ifstream input(result.output.path);
        std::string line, role;
        int tool = -1;
        bool body = false;
        double x = 0, y = 0, z = 0, feed = 0;
        std::optional<double> restore_z;
        size_t lifts = 0, protected_changes = 0;
        std::vector<std::string> deposition;
        std::vector<int> changes;
        while (std::getline(input, line)) {
            if (line == ";LAYER_CHANGE") body = true;
            if (line.rfind(";TYPE:", 0) == 0) role = line.substr(6);
            std::istringstream words(line.substr(0, line.find(';')));
            std::string command;
            words >> command;
            if (command == "T0" || command == "T1") {
                const int next = command[1] - '0';
                if (body && next != tool) {
                    changes.push_back(next);
                    CHECK(restore_z.has_value() == (clearance > 0));
                    if (restore_z) ++protected_changes;
                }
                tool = next;
            }
            if (command != "G0" && command != "G1") continue;
            const double old_x = x, old_y = y, old_z = z;
            double e = 0;
            for (std::string word; words >> word;) {
                if (word.size() < 2) continue;
                const double value = std::stod(word.substr(1));
                if (word[0] == 'X') x = value;
                if (word[0] == 'Y') y = value;
                if (word[0] == 'Z') z = value;
                if (word[0] == 'E') e = value;
                if (word[0] == 'F') feed = value;
            }
            if (line.find("; toolchange clearance") != std::string::npos) {
                REQUIRE_FALSE(restore_z.has_value());
                restore_z = old_z;
                CHECK(z - old_z == Catch::Approx(clearance).margin(0.001));
                ++lifts;
            } else if (line.find("; restore Z after toolchange") != std::string::npos) {
                REQUIRE(restore_z.has_value());
                CHECK(z == Catch::Approx(*restore_z).margin(0.001));
                restore_z.reset();
            } else if (restore_z) {
                // Only the firmware Tx and temperature commands may intervene.
                FAIL("Motion before temporary tool-change clearance was restored: " << line);
            }
            if (e > 0 && (x != old_x || y != old_y))
                deposition.push_back(std::to_string(tool) + ":" + role + ":" +
                    std::to_string(x) + ":" + std::to_string(y) + ":" + std::to_string(z) + ":" +
                    std::to_string(e) + ":" + std::to_string(feed));
        }
        CHECK_FALSE(restore_z.has_value());
        REQUIRE(changes.size() > 1);
        CHECK(lifts == protected_changes);
        if (clearance == 0) {
            baseline_deposition = std::move(deposition);
            baseline_changes = std::move(changes);
        } else {
            CHECK(deposition == baseline_deposition);
            CHECK(changes == baseline_changes);
            CHECK(protected_changes == changes.size());
        }
        std::filesystem::remove(result.output.path);
    }
}

TEST_CASE("machine output settings migrate only missing known preset values", "[libslicer_api][machine-gcode][3mf]")
{
    libslicer::LibraryOptions options;
    options.resource_directory = LIBSLICER_TEST_RESOURCE_DIR;
    options.vendors = {"CFSYS"};
    auto library = libslicer::Library::open(options);
    REQUIRE(library);
    libslicer::ConfigSelection selection;
    selection.machine_model_id = "CFSYS Alpha500 Printer";
    selection.machine_variant_id = "0.4";
    selection.process_preset_id = "CCF&CIRON @CFSYS";
    selection.filament_preset_ids = {"CFSYS CIRON", "CFSYS CCF"};
    selection.filament_physical_tools = {0, 1};
    REQUIRE(library->activate_config(selection).success);
    const auto snapshot = *library->active_config_snapshot();
    REQUIRE(snapshot.value("toolchange_z_lift") == "10");
    REQUIRE(snapshot.value("part_cooling_fan_index") == "1");

    const int mode = GENERATE(0, 1, 2, 3);
    // 0: missing official fields; 1: explicit off; 2: unknown lineage;
    // 3: explicit customized values survive project serialization.
    const auto json_string = [](const std::string &value) {
        std::string result = "\"";
        for (const unsigned char c : value) {
            if (c == '\\' || c == '\"') { result += '\\'; result += c; }
            else if (c < 32) { result += "\\u00"; result += "0123456789abcdef"[c >> 4]; result += "0123456789abcdef"[c & 15]; }
            else result += c;
        }
        return result + "\"";
    };
    std::string settings = "{\"name\":\"project_settings\"";
    for (const auto &[key, value] : snapshot.values()) {
        std::string saved = value;
        if (key == "toolchange_z_lift" || key == "part_cooling_fan_index") {
            if (mode == 0 || mode == 2) continue;
            saved = key == "toolchange_z_lift" ? (mode == 1 ? "0" : "7.5") : (mode == 1 ? "-1" : "4");
        } else if (mode == 2 && (key == "printer_settings_id" || key == "inherits_group"))
            saved = "Unknown custom preset";
        settings += "," + json_string(key) + ":" + json_string(saved);
    }
    settings += "}";
    const std::string model = R"xml(<?xml version="1.0" encoding="UTF-8"?>
<model unit="millimeter" xmlns="http://schemas.microsoft.com/3dmanufacturing/core/2015/02">
<metadata name="Application">BambuStudio-2.0.0</metadata>
<metadata name="BambuStudio:3mfVersion">1</metadata>
<resources><object id="1" type="model"><mesh><vertices>
<vertex x="0" y="0" z="0"/><vertex x="10" y="0" z="0"/>
<vertex x="0" y="10" z="0"/><vertex x="0" y="0" z="10"/>
</vertices><triangles><triangle v1="0" v2="2" v3="1"/><triangle v1="0" v2="1" v3="3"/>
<triangle v1="0" v2="3" v3="2"/><triangle v1="1" v2="2" v3="3"/>
</triangles></mesh></object></resources><build><item objectid="1"/></build></model>)xml";
    const auto path = std::filesystem::temp_directory_path() / ("libslicer-machine-settings-" + std::to_string(mode) + ".3mf");
    mz_zip_archive archive{};
    REQUIRE(mz_zip_writer_init_file(&archive, path.string().c_str(), 0));
    const std::string relationships = R"xml(<?xml version="1.0"?><Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships"><Relationship Target="/3D/3dmodel.model" Id="rel0" Type="http://schemas.microsoft.com/3dmanufacturing/2013/01/3dmodel"/></Relationships>)xml";
    const std::string content_types = R"xml(<?xml version="1.0"?><Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types"><Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/><Default Extension="model" ContentType="application/vnd.ms-package.3dmanufacturing-3dmodel+xml"/></Types>)xml";
    REQUIRE(mz_zip_writer_add_mem(&archive, "_rels/.rels", relationships.data(), relationships.size(), MZ_DEFAULT_COMPRESSION));
    REQUIRE(mz_zip_writer_add_mem(&archive, "[Content_Types].xml", content_types.data(), content_types.size(), MZ_DEFAULT_COMPRESSION));
    REQUIRE(mz_zip_writer_add_mem(&archive, "3D/3dmodel.model", model.data(), model.size(), MZ_DEFAULT_COMPRESSION));
    REQUIRE(mz_zip_writer_add_mem(&archive, "Metadata/project_settings.config", settings.data(), settings.size(), MZ_DEFAULT_COMPRESSION));
    REQUIRE(mz_zip_writer_finalize_archive(&archive));
    REQUIRE(mz_zip_writer_end(&archive));
    libslicer::ProjectImportRequest request;
    request.path = path.string();
    const auto imported = library->import_project(request);
    std::ostringstream import_diagnostics;
    for (const auto &diagnostic : imported.diagnostics) import_diagnostics << diagnostic.message << "\n";
    INFO(import_diagnostics.str());
    REQUIRE(imported);
    REQUIRE(library->active_config_snapshot());
    const auto loaded = *library->active_config_snapshot();
    CAPTURE(mode, loaded.value("toolchange_z_lift").value_or("missing"), loaded.value("part_cooling_fan_index").value_or("missing"));
    CHECK(loaded.value("toolchange_z_lift") == (mode == 0 ? "10" : mode == 3 ? "7.5" : "0"));
    CHECK(loaded.value("part_cooling_fan_index") == (mode == 0 ? "1" : mode == 3 ? "4" : "-1"));
    const auto patch = library->apply_active_config_patch({{"toolchange_z_lift", "6"}, {"part_cooling_fan_index", "5"}});
    REQUIRE(patch.success);
    CHECK(library->active_config_snapshot()->value("toolchange_z_lift") == "6");
    CHECK(library->active_config_snapshot()->value("part_cooling_fan_index") == "5");
    // A previous snapshot remains immutable when UI values change.
    CHECK(snapshot.value("toolchange_z_lift") == "10");
    std::filesystem::remove(path);
}

TEST_CASE("CFSYS export restores legacy toolchange clearance and fan channels", "[libslicer_api][fiber][cfsys-output]")
{
    libslicer::LibraryOptions options;
    options.resource_directory = LIBSLICER_TEST_RESOURCE_DIR;
    options.vendors = {"CFSYS"};
    auto library = libslicer::Library::open(options);
    REQUIRE(library);
    libslicer::ConfigSelection selection;
    selection.machine_model_id = "CFSYS Alpha500 Printer";
    selection.machine_variant_id = "0.4";
    selection.process_preset_id = "CCF&CIRON @CFSYS";
    selection.filament_preset_ids = {"CFSYS CIRON", "CFSYS CCF"};
    selection.filament_physical_tools = {0, 1};
    REQUIRE(library->activate_config(selection, {
        {"curr_bed_type", "Engineering Plate"},
        {"generate_reinforced_infills", "0"}}).success);
    const char* model = std::getenv("LIBSLICER_FIBER_VALIDATION_MODEL");
    const double clearance = model ? 10.0 : GENERATE(0.0, 7.5, 10.0);
    const int fan_channel = clearance == 7.5 ? 4 : 1;
    if (clearance != 10.0)
        REQUIRE(library->apply_active_config_patch({{"toolchange_z_lift", std::to_string(clearance)},
            {"part_cooling_fan_index", std::to_string(fan_channel)}}).success);
    libslicer::SliceRequest request;
    request.config = *library->active_config_snapshot();
    request.center_on_build_plate = model != nullptr;
    request.objects.push_back(model ? binary_stl_on_bed(model) : fiber_infill_block());
    const char* output = std::getenv("LIBSLICER_CFSYS_REVIEW_OUTPUT");
    if (output) request.output_gcode_path = output;
    const auto result = library->slice(request);
    for (const auto& diagnostic : result.diagnostics) INFO(diagnostic.message);
    REQUIRE(result.success);
    std::ifstream input(result.output.path);
    const std::string gcode((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    REQUIRE(gcode.find(";FIBER_BEGIN") != std::string::npos);
    CHECK(gcode.find("M190 S100") != std::string::npos);
    CHECK(gcode.find("M109 S300 T0") != std::string::npos);
    CHECK(gcode.find("M109 S235 T1") != std::string::npos);
    CHECK(gcode.find("M106 P" + std::to_string(fan_channel) + " S255") != std::string::npos);
    CHECK(gcode.find("M106 P2 S255") != std::string::npos);

    std::istringstream stream(gcode);
    std::string line;
    bool body = false, last_move_only_z = false;
    double z = 0, before_last_move_z = 0, layer_z = 0;
    std::optional<double> restore_z;
    size_t changes = 0, restored = 0;
    while (std::getline(stream, line)) {
        if (line == ";LAYER_CHANGE") body = true;
        if (line.rfind(";Z:", 0) == 0) layer_z = std::stod(line.substr(3));
        const auto comment = line.find(';');
        if (comment != std::string::npos) line.resize(comment);
        std::istringstream command(line);
        std::string code;
        command >> code;
        if (code == "M106") {
            CHECK((line.find(" P" + std::to_string(fan_channel) + " ") != std::string::npos ||
                   line.find(" P2") != std::string::npos || line.find(" P3") != std::string::npos));
        }
        if (body && (code == "T0" || code == "T1")) {
            ++changes;
            if (clearance == 0) continue;
            REQUIRE_FALSE(restore_z.has_value());
            REQUIRE(last_move_only_z);
            // A deferred layer transition may be combined with the upward
            // move. Clearance is above the current layer (or an existing hop).
            restore_z = std::max(before_last_move_z, layer_z);
            CHECK(z - *restore_z == Catch::Approx(clearance).margin(0.001));
        }
        if (code == "G0" || code == "G1") {
            bool has_z = false, has_xy_or_e = false;
            double next_z = z;
            for (std::string parameter; command >> parameter;) {
                if (parameter[0] == 'Z') { has_z = true; next_z = std::stod(parameter.substr(1)); }
                if (parameter[0] == 'X' || parameter[0] == 'Y' || parameter[0] == 'E') has_xy_or_e = true;
            }
            if (has_z || has_xy_or_e) {
                if (restore_z) {
                    CHECK(has_z);
                    CHECK_FALSE(has_xy_or_e);
                    CHECK(next_z == Catch::Approx(*restore_z).margin(0.001));
                    restore_z.reset();
                    ++restored;
                }
                before_last_move_z = z;
                z = next_z;
                last_move_only_z = has_z && !has_xy_or_e;
            }
        }
    }
    REQUIRE(changes > 1);
    CHECK(restored == (clearance > 0 ? changes : 0));
    CHECK_FALSE(restore_z.has_value());
    if (!output) std::filesystem::remove(result.output.path);
}

namespace {
libslicer::SliceObjectInput fiber_debug_ring()
{
    libslicer::SliceObjectInput object;
    object.name = "fiber-debug-ring";
    libslicer::SliceVolumeInput volume;
    // Extruded rectangular annulus: includes a through hole and non-zero XY placement.
    volume.vertices = {{100,100,0},{160,100,0},{160,140,0},{100,140,0},
                       {120,110,0},{140,110,0},{140,130,0},{120,130,0},
                       {100,100,3},{160,100,3},{160,140,3},{100,140,3},
                       {120,110,3},{140,110,3},{140,130,3},{120,130,3}};
    const auto quad = [&](std::uint32_t a, std::uint32_t b, std::uint32_t c, std::uint32_t d) {
        volume.triangles.push_back({a,b,c}); volume.triangles.push_back({a,c,d});
    };
    for (std::uint32_t i=0;i<4;++i) {
        const auto j=(i+1)%4;
        quad(i,j,j+8,i+8); // outer wall
        quad(i+4,i+12,j+12,j+4); // inner wall
        quad(i+8,j+8,j+12,i+12); // top
        quad(i,i+4,j+4,j); // bottom
    }
    object.volumes.push_back(std::move(volume));
    return object;
}
}

TEST_CASE("fiber fill debug is opt-in and does not change print output", "[libslicer_api][fiber-fill-debug]")
{
    libslicer::LibraryOptions options;
    options.resource_directory = LIBSLICER_TEST_RESOURCE_DIR;
    options.vendors = {"CFSYS"};
    auto library = libslicer::Library::open(options);
    REQUIRE(library);
    libslicer::ConfigSelection selection;
    selection.machine_model_id = "CFSYS Alpha500 Printer";
    selection.machine_variant_id = "0.4";
    selection.process_preset_id = "CCF&CIRON @CFSYS";
    selection.filament_preset_ids = {"CFSYS CIRON", "CFSYS CCF"};
    selection.filament_physical_tools = {0,1};
    const char* model = std::getenv("LIBSLICER_FIBER_VALIDATION_MODEL");
    // Synthetic fixture deliberately rejects candidates; the optional real model
    // uses the unmodified production process budget.
    REQUIRE(library->activate_config(selection, {{"generate_reinforced_infills", "1"}}).success);
    if (!model) REQUIRE(library->apply_active_config_patch({{"fiber_minimum_path_length", "1000"}}).success);
    libslicer::SliceRequest request;
    request.config = *library->active_config_snapshot();
    request.center_on_build_plate = model != nullptr;
    request.objects.push_back(model ? binary_stl_on_bed(model) : fiber_debug_ring());
    const auto normal = library->slice(request);
    for (const auto& d : normal.diagnostics) INFO(d.message);
    REQUIRE(normal.success);
    REQUIRE(normal.preview);
    CHECK(normal.preview->fiber_fill_diagnostics.empty());
    if (!model) CHECK(std::any_of(normal.diagnostics.begin(), normal.diagnostics.end(),
        [](const auto& d) { return d.code == "fiber_outer_contour_rejected"; }));
    REQUIRE(request.config.value("fiber_fill_debug") == "0");
    REQUIRE(library->apply_active_config_patch({{"fiber_fill_debug", "1"}}).success);
    request.config = *library->active_config_snapshot();
    REQUIRE(request.config.value("fiber_fill_debug") == "1");
    // A later UI edit cannot change the already captured slice configuration.
    REQUIRE(library->apply_active_config_patch({{"fiber_fill_debug", "0"}}).success);
    const auto debug = library->slice(request);
    for (const auto& d : debug.diagnostics) INFO(d.message);
    REQUIRE(debug.success);
    REQUIRE(debug.preview);
    REQUIRE_FALSE(debug.preview->fiber_fill_diagnostics.empty());
    size_t contour_count = 0, infill_count = 0, original_count = 0, missing_count = 0;
    bool original_has_holes = false, missing_has_holes = false;
    double original_area = 0, missing_area = 0;
    const auto loop_area = [](const std::vector<libslicer::ToolpathPoint>& loop) {
        double result = 0;
        for (size_t i=1;i<loop.size();++i)
            result += double(loop[i-1].x)*loop[i].y-double(loop[i].x)*loop[i-1].y;
        return result*.5;
    };
    for (const auto& path : debug.preview->fiber_fill_diagnostics) {
        INFO("diagnostic kind=" << unsigned(path.kind) << " layer=" << path.layer_index
             << " component=" << path.component_id);
        CHECK_FALSE(path.reason.empty());
        if (path.kind == libslicer::FiberDiagnosticKind::ContourCandidate ||
            path.kind == libslicer::FiberDiagnosticKind::RoundedContourCandidate) {
            REQUIRE(path.points.size() >= 2);
            CHECK(path.contour);
            CHECK(path.source_length_mm > 0);
            CHECK(path.boundaries.empty());
            CHECK(path.triangles.empty());
        } else if (path.kind == libslicer::FiberDiagnosticKind::RejectedPath) {
            REQUIRE(path.points.size() >= 2);
            CHECK(path.source_length_mm > 0);
            path.contour ? ++contour_count : ++infill_count;
        } else {
            REQUIRE_FALSE(path.boundaries.empty());
            double area = 0;
            for (const auto& loop : path.boundaries) {
                REQUIRE(loop.size() >= 4);
                CHECK(loop.front().x == loop.back().x);
                CHECK(loop.front().y == loop.back().y);
                area += loop_area(loop);
            }
            if (path.kind == libslicer::FiberDiagnosticKind::OriginalContourRegion) {
                // Preserve even sub-resolution original domains. Quantizing their
                // world coordinates to float can flip their tiny signed area.
                // Bound the possible area change by coordinate ULP and perimeter.
                double area_roundoff = 0;
                for (const auto& loop : path.boundaries) {
                    double coordinate_scale = 1;
                    for (const auto& point : loop)
                        coordinate_scale = std::max({coordinate_scale, std::abs(double(point.x)), std::abs(double(point.y))});
                    const double ulp = coordinate_scale * std::numeric_limits<float>::epsilon();
                    for (size_t i=1;i<loop.size();++i)
                        area_roundoff += ulp*(std::abs(double(loop[i].x)-loop[i-1].x) +
                                              std::abs(double(loop[i].y)-loop[i-1].y)) + 2*ulp*ulp;
                }
                CHECK(area >= -area_roundoff);
                ++original_count;
                original_has_holes |= path.boundaries.size() > 1;
                original_area += area;
                CHECK(path.triangles.empty());
            } else {
                CHECK(area > 0);
                ++missing_count;
                missing_has_holes |= path.boundaries.size() > 1;
                missing_area += area;
                REQUIRE_FALSE(path.triangles.empty());
                REQUIRE(path.triangles.size()%3 == 0);
                double triangle_area = 0;
                for (size_t i=0;i<path.triangles.size();i+=3) {
                    const auto a=path.triangles[i], b=path.triangles[i+1], c=path.triangles[i+2];
                    triangle_area += std::abs((double(b.x)-a.x)*(double(c.y)-a.y)-(double(b.y)-a.y)*(double(c.x)-a.x))*.5;
                    if (!model) {
                        const double x=(double(a.x)+b.x+c.x)/3, y=(double(a.y)+b.y+c.y)/3;
                        CHECK_FALSE((x>120 && x<140 && y>110 && y<130));
                    }
                }
                CHECK(triangle_area == Catch::Approx(area).margin(0.005));
            }
        }
        REQUIRE(path.layer_index < debug.preview->layers.size());
        CHECK(path.object_index == 0);
        CHECK(path.instance_index == 0);
        std::vector<libslicer::ToolpathPoint> all_points = path.points;
        for (const auto& loop : path.boundaries) all_points.insert(all_points.end(), loop.begin(), loop.end());
        all_points.insert(all_points.end(), path.triangles.begin(), path.triangles.end());
        for (const auto& p : all_points) {
            CHECK(std::isfinite(p.x)); CHECK(std::isfinite(p.y));
            CHECK(p.z == Catch::Approx(debug.preview->layers[path.layer_index].print_z_mm));
            if (!model) {
                CHECK(p.x >= 100); CHECK(p.x <= 160);
                CHECK(p.y >= 100); CHECK(p.y <= 140);
            }
        }
    }
    CHECK(original_count > 0);
    CHECK(missing_count > 0);
    CHECK(original_has_holes);
    CHECK(missing_has_holes);
    // Candidate bands occupy only a fraction of the domain, even when all candidates fail.
    CHECK(missing_area < original_area*.5);
    CHECK(contour_count > 0);
    CHECK(infill_count > 0);
    CHECK(debug.preview->segments.size() == normal.preview->segments.size());
    CHECK(debug.summary.filament_used_mm == Catch::Approx(normal.summary.filament_used_mm));
    CHECK(debug.summary.estimated_time_seconds == Catch::Approx(normal.summary.estimated_time_seconds));
    const auto commands = [](const std::string& path) {
        std::ifstream input(path);
        std::vector<std::string> result;
        for (std::string line; std::getline(input, line);) {
            // Timestamps and file names live in comments, never compare those.
            line = line.substr(0, line.find(';'));
            if (!line.empty()) result.push_back(std::move(line));
        }
        return result;
    };
    CHECK(commands(debug.output.path) == commands(normal.output.path));
    std::cout << "[FiberFillDebug] contour_rejected=" << contour_count
              << " infill_rejected=" << infill_count << " commands_unchanged\n";
    for (const auto& diagnostic : debug.diagnostics)
        if (diagnostic.code == "fiber_infill_summary" || diagnostic.code == "fiber_infill_empty") {
            std::cout << "[FiberFillDebug] " << diagnostic.message << '\n';
            const auto start = diagnostic.message.find("Rejected fragments:");
            REQUIRE(start != std::string::npos);
            std::istringstream counts(diagnostic.message.substr(start + 19));
            size_t rejected = 0;
            for (std::string entry; counts >> entry;) {
                const auto equal = entry.find('=');
                REQUIRE(equal != std::string::npos);
                rejected += std::stoul(entry.substr(equal+1));
            }
            CHECK(infill_count == rejected);
        }
    CHECK(std::none_of(debug.diagnostics.begin(), debug.diagnostics.end(), [](const auto& d) { return d.code == "fiber_debug_layer"; }));
    libslicer::GCodePreviewRequest imported_request;
    imported_request.gcode_path = debug.output.path;
    const auto imported = library->load_gcode_preview(imported_request);
    REQUIRE(imported.success);
    REQUIRE(imported.preview);
    CHECK(imported.preview->fiber_fill_diagnostics.empty());
    std::filesystem::remove(normal.output.path);
    std::filesystem::remove(debug.output.path);
}

TEST_CASE("CFSYS rectilinear fiber keeps the cut tail before non-depositing finish on T1", "[libslicer_api][fiber][fiber-infill-output]")
{
    const unsigned fiber_slot = GENERATE(1u, 4u);
    const bool relative_e = GENERATE(false, true);
    libslicer::LibraryOptions options;
    options.resource_directory = LIBSLICER_TEST_RESOURCE_DIR;
    options.vendors = {"CFSYS"};
    auto library = libslicer::Library::open(options);
    REQUIRE(library);
    libslicer::ConfigSelection selection;
    selection.machine_model_id = "CFSYS Alpha500 Printer";
    selection.machine_variant_id = "0.4";
    selection.process_preset_id = "CCF&CIRON @CFSYS";
    selection.filament_preset_ids.assign(5, "CFSYS CIRON");
    selection.filament_physical_tools.assign(5, 0);
    selection.filament_preset_ids[fiber_slot] = "CFSYS CCF";
    selection.filament_physical_tools[fiber_slot] = 1;
    REQUIRE(library->activate_config(selection, {
        {"generate_reinforced_infills", "1"},
        {"reinforced_perimeters_filament", std::to_string(fiber_slot+1)},
        {"reinforced_infill_filament", std::to_string(fiber_slot+1)},
        {"use_relative_e_distances", relative_e ? "1" : "0"}}).success);
    libslicer::SliceRequest request;
    const char* model = std::getenv("LIBSLICER_FIBER_VALIDATION_MODEL");
    request.objects.push_back(model ? binary_stl_on_bed(model) : fiber_infill_block());
    request.config = *library->active_config_snapshot();
    if (const char* dir = std::getenv("LIBSLICER_FIBER_INFILL_OUTPUT_DIR"))
        request.output_gcode_path = (std::filesystem::path(dir) /
            ("fiber-infill-T1-slot" + std::to_string(fiber_slot) + (relative_e ? "-relative.gcode" : "-absolute.gcode"))).string();
    const auto result = library->slice(request);
    std::string diagnostics;
    for (const auto& issue : result.diagnostics) diagnostics += issue.message + "\n";
    INFO(diagnostics);
    if (model) std::cout << "[FiberOutputValidation] slot=" << fiber_slot << " relative_e=" << relative_e << " " << diagnostics;
    REQUIRE(result.success);
    REQUIRE(result.preview);
    CHECK(std::none_of(result.diagnostics.begin(), result.diagnostics.end(), [](const auto& d) { return d.code == "fiber_infill_empty"; }));

    std::ifstream output(result.output.path);
    REQUIRE(output.good());
    std::string line, phase;
    bool infill = false, relative = false, absolute_xyz = true;
    unsigned tool = 0;
    double x = 0, y = 0, z = 0, e = 0, tail = 0, finish = 0, powered = 0;
    size_t infills = 0;
    std::map<size_t,size_t> contours;
    while (std::getline(output,line)) {
        if (line.rfind(";FIBER_BEGIN ",0) == 0) {
            infill = line.find("purpose=infill") != std::string::npos;
            if (infill) { ++infills; CHECK(tool == 1); tail = finish = powered = 0; }
            else if (line.find("purpose=contour") != std::string::npos) {
                const auto pos = line.find(" layer="); REQUIRE(pos != std::string::npos);
                ++contours[std::stoul(line.substr(pos+7))];
            }
        }
        if (line == ";FIBER_START") phase = "powered";
        if (line == ";FIBER_TAIL_BEGIN") phase = "tail";
        if (line == ";FIBER_DEPLETED") phase = "finish";
        if (line == ";FIBER_END") {
            if (infill) {
                CHECK(powered > 0);
                CHECK(tail == Catch::Approx(23).margin(0.03));
                CHECK(finish == Catch::Approx(23).margin(0.03));
            }
            infill = false; phase.clear();
        }
        std::istringstream fields(line);
        std::string cmd; fields >> cmd;
        if (cmd == "M82") relative = false;
        if (cmd == "M83") relative = true;
        if (cmd == "G90") absolute_xyz = true;
        if (cmd == "G91") absolute_xyz = false;
        if (cmd.size() > 1 && cmd[0] == 'T' && std::isdigit(static_cast<unsigned char>(cmd[1]))) tool = std::stoul(cmd.substr(1));
        if (cmd != "G0" && cmd != "G1" && cmd != "G92") continue;
        double nx=x, ny=y, nz=z, ne=e; bool has_e=false;
        std::string field;
        while (fields >> field) {
            if (field[0] == ';') break;
            if (field.size() < 2 || std::string("XYZE").find(field[0]) == std::string::npos) continue;
            const double value = std::stod(field.substr(1));
            if (field[0] == 'X') nx = value + (absolute_xyz || cmd == "G92" ? 0 : x);
            if (field[0] == 'Y') ny = value + (absolute_xyz || cmd == "G92" ? 0 : y);
            if (field[0] == 'Z') nz = value + (absolute_xyz || cmd == "G92" ? 0 : z);
            if (field[0] == 'E') { ne = value + (relative && cmd != "G92" ? e : 0); has_e = true; }
        }
        if (infill && cmd != "G92" && !phase.empty()) {
            CHECK(tool == 1);
            if (phase == "powered") { if (has_e) powered += ne-e; }
            else {
                CHECK_FALSE(has_e);
                CHECK(nz == Catch::Approx(z).margin(0.0001));
                if (phase == "tail") tail += std::hypot(nx-x,ny-y);
                else finish += std::hypot(nx-x,ny-y);
            }
        }
        x=nx; y=ny; z=nz; e=ne;
    }
    CHECK(infills > 0);
    if (const char* baseline = std::getenv("LIBSLICER_FIBER_CONTOUR_BASELINE_GCODE")) {
        std::ifstream input(baseline); REQUIRE(input.good());
        std::map<size_t,size_t> expected;
        while (std::getline(input,line))
            if (line.rfind(";FIBER_BEGIN ",0) == 0 && line.find("purpose=contour") != std::string::npos) {
                const auto pos = line.find(" layer="); REQUIRE(pos != std::string::npos);
                ++expected[std::stoul(line.substr(pos+7))];
            }
        REQUIRE_FALSE(expected.empty());
        for (const auto& [layer,count] : expected) { INFO("layer=" << layer); CHECK(contours[layer] >= count); }
    }
    const auto verify_preview = [&](const libslicer::ToolpathPreview& preview) {
        size_t powered_segments = 0, passive_segments = 0;
        for (const auto& segment : preview.segments) {
            if (segment.extrusion_role != libslicer::ToolpathExtrusionRole::ContinuousFiberInfill) continue;
            CHECK(segment.tool_id == 1);
            CHECK(segment.filament_id == fiber_slot);
            if (segment.deposition == libslicer::ToolpathDepositionKind::ContinuousFiberPowered) {
                ++powered_segments; CHECK(segment.fiber_feed_delta_mm > 0);
            }
            if (segment.deposition == libslicer::ToolpathDepositionKind::ContinuousFiberPassive) ++passive_segments;
        }
        CHECK(powered_segments > 0); CHECK(passive_segments > 0);
    };
    verify_preview(*result.preview);
    const auto imported = library->load_gcode_preview({result.output.path});
    REQUIRE(imported.success); REQUIRE(imported.preview);
    verify_preview(*imported.preview);
    if (request.output_gcode_path.empty()) { std::error_code ec; std::filesystem::remove(result.output.path,ec); }
}

TEST_CASE("CFSYS rectilinear infill changes only non-depositing finish with extension", "[libslicer_api][fiber][fiber-rectilinear-finish]")
{
    const bool relative_e=GENERATE(false,true);
    libslicer::LibraryOptions options;
    options.resource_directory=LIBSLICER_TEST_RESOURCE_DIR;options.vendors={"CFSYS"};
    auto library=libslicer::Library::open(options);
    libslicer::ConfigSelection selection;
    selection.machine_model_id="CFSYS Alpha500 Printer";selection.machine_variant_id="0.4";
    selection.process_preset_id="CCF&CIRON @CFSYS";
    selection.filament_preset_ids={"CFSYS CIRON","CFSYS CCF"};selection.filament_physical_tools={0,1};
    struct Proof {
        std::vector<std::array<double,3>> depositing;
        std::vector<std::string> feed_commands;
        std::array<double,3> cut{};
        double tail=0,finish=0;
        size_t cuts=0;
    };
    std::vector<Proof> reference;
    for(const std::string extension:{"0","23"}) {
        CAPTURE(extension,relative_e);
        REQUIRE(library->activate_config(selection,{{"generate_reinforced_infills","1"},
            {"generate_reinforced_perimeters","0"},{"reinforced_infill_pattern","rectilinear"},
            {"fiber_finish_extension_length",extension},{"use_relative_e_distances",relative_e?"1":"0"}}).success);
        libslicer::SliceRequest request;request.objects.push_back(fiber_infill_block());
        request.config=*library->active_config_snapshot();
        const auto result=library->slice(request);
        for(const auto& d:result.diagnostics)UNSCOPED_INFO(d.message);
        REQUIRE(result.success);REQUIRE(result.preview);
        std::ifstream input(result.output.path);REQUIRE(input.good());
        std::vector<Proof> proofs;
        bool active=false,cut=false,absolute=true;
        std::string phase,line;double x=0,y=0,z=0;
        while(std::getline(input,line)) {
            if(line.rfind(";FIBER_BEGIN ",0)==0) {
                REQUIRE_FALSE(active);
                CHECK(line.find("purpose=infill ")!=std::string::npos);
                active=true;cut=false;phase.clear();proofs.emplace_back();
            } else if(line==";FIBER_LANDING_BEGIN")phase="landing";
            else if(line==";FIBER_START") {CHECK_FALSE(cut);phase="powered";}
            else if(line==";FIBER_CUT") {
                REQUIRE(active);CHECK_FALSE(cut);cut=true;phase="tail";
                ++proofs.back().cuts;proofs.back().cut={x,y,z};
            } else if(line==";FIBER_FINISH_BEGIN")phase="finish";
            else if(line==";FIBER_END") {REQUIRE(active);active=false;phase.clear();}
            if(active && cut) {
                CHECK(line!=";FIBER_START");
                CHECK(line.rfind(";FIBER_PREFEED",0)!=0);
                if(!line.empty() && line.front()!=';') {
                    CHECK_FALSE(std::regex_search(line,std::regex(R"(\bE[-+0-9.])")));
                    CHECK_FALSE(std::regex_search(line,std::regex(R"(^T[0-9])")));
                }
            }
            std::istringstream fields(line);std::string cmd;fields>>cmd;
            if(active && !cut && (cmd=="G0" || cmd=="G1") &&
                std::regex_search(line,std::regex(R"(\bE[-+0-9.])")))
                proofs.back().feed_commands.push_back(line);
            if(cmd=="G90")absolute=true;if(cmd=="G91")absolute=false;
            if(cmd!="G0" && cmd!="G1" && cmd!="G92")continue;
            double nx=x,ny=y,nz=z;
            for(std::string field;fields>>field;) {
                if(field.front()==';')break;
                if(field.size()<2 || std::string("XYZ").find(field.front())==std::string::npos)continue;
                const double value=std::stod(field.substr(1));
                if(field.front()=='X')nx=value+(absolute || cmd=="G92"?0:x);
                if(field.front()=='Y')ny=value+(absolute || cmd=="G92"?0:y);
                if(field.front()=='Z')nz=value+(absolute || cmd=="G92"?0:z);
            }
            if(active && cmd!="G92" && !phase.empty()) {
                auto& proof=proofs.back();
                const double distance=std::hypot(nx-x,ny-y);
                if(phase=="finish") {
                    CHECK(nz==Catch::Approx(z).margin(.0001));proof.finish+=distance;
                } else {
                    if(proof.depositing.empty())proof.depositing.push_back({x,y,z});
                    if(distance>0)proof.depositing.push_back({nx,ny,nz});
                    if(phase=="tail")proof.tail+=distance;
                }
            }
            x=nx;y=ny;z=nz;
        }
        REQUIRE_FALSE(active);REQUIRE(proofs.size()>1);
        for(const auto& proof:proofs) {
            CHECK(proof.cuts==1);CHECK(proof.tail==Catch::Approx(23).margin(.03));
            CHECK(proof.finish==Catch::Approx(std::stod(extension)).margin(.003));
        }
        if(reference.empty())reference=proofs;
        else {
            REQUIRE(proofs.size()==reference.size());
            for(size_t i=0;i<proofs.size();++i) {
                CHECK(proofs[i].depositing==reference[i].depositing);
                CHECK(proofs[i].cut==reference[i].cut);
                REQUIRE_FALSE(proofs[i].feed_commands.empty());
                CHECK(proofs[i].feed_commands==reference[i].feed_commands);
            }
        }
        const auto verify_preview=[&](const libslicer::ToolpathPreview& preview) {
            std::set<std::uint64_t> infill_occurrences;
            for(const auto& segment:preview.segments)
                if(segment.extrusion_role==libslicer::ToolpathExtrusionRole::ContinuousFiberInfill)
                    infill_occurrences.insert(segment.fiber_occurrence);
            REQUIRE(infill_occurrences.size()==proofs.size());
            std::map<std::uint64_t,double> finish_lengths;
            for(const auto& segment:preview.segments) {
                if(!infill_occurrences.count(segment.fiber_occurrence))continue;
                if(segment.fiber_phase==libslicer::ToolpathFiberPhase::Tail ||
                   segment.fiber_phase==libslicer::ToolpathFiberPhase::Finish)
                    CHECK(segment.fiber_feed_delta_mm==0);
                if(segment.fiber_phase!=libslicer::ToolpathFiberPhase::Finish)continue;
                CHECK(segment.deposition==libslicer::ToolpathDepositionKind::None);
                CHECK(segment.extrusion_role==libslicer::ToolpathExtrusionRole::None);
                finish_lengths[segment.fiber_occurrence]+=std::hypot(segment.end_mm.x-segment.start_mm.x,segment.end_mm.y-segment.start_mm.y);
            }
            for(auto id:infill_occurrences)CHECK(finish_lengths[id]==Catch::Approx(std::stod(extension)).margin(.005));
        };
        verify_preview(*result.preview);
        const auto imported=library->load_gcode_preview({result.output.path});
        REQUIRE(imported.success);REQUIRE(imported.preview);verify_preview(*imported.preview);
        input.close();
        std::filesystem::remove(result.output.path);
    }
    // A non-depositing finish may leave the model, but never the machine.
    REQUIRE(library->activate_config(selection,{{"generate_reinforced_infills","1"},
        {"generate_reinforced_perimeters","0"},{"reinforced_infill_pattern","rectilinear"},
        {"fiber_finish_extension_length","500"}}).success);
    libslicer::SliceRequest excessive;excessive.objects.push_back(fiber_infill_block());
    excessive.config=*library->active_config_snapshot();
    const auto rejected=library->slice(excessive);
    CHECK_FALSE(rejected.success);CHECK(rejected.output.path.empty());
    CHECK(std::any_of(rejected.diagnostics.begin(),rejected.diagnostics.end(),[](const auto& d) {
        return !d.warning && d.message.find("Fiber motion exceeds")!=std::string::npos;
    }));
}

TEST_CASE("translated rectilinear finish respects final machine coordinates", "[libslicer_api][fiber-motion-limit][fiber-rectilinear-finish]")
{
    libslicer::LibraryOptions options;
    options.resource_directory=LIBSLICER_TEST_RESOURCE_DIR;options.vendors={"CFSYS"};
    auto library=libslicer::Library::open(options);
    libslicer::ConfigSelection selection;
    selection.machine_model_id="CFSYS Alpha500 Printer";selection.machine_variant_id="0.4";
    selection.process_preset_id="CCF&CIRON @CFSYS";
    selection.filament_preset_ids={"CFSYS CIRON","CFSYS CCF"};selection.filament_physical_tools={0,1};
    REQUIRE(library->activate_config(selection,{{"generate_reinforced_infills","1"},
        {"generate_reinforced_perimeters","0"},{"reinforced_infill_pattern","rectilinear"}}).success);
    // Use a horizontal finish longer than the 60 mm source width. Determine
    // its direction before translating: a zigzag need not end on the right.
    REQUIRE(library->apply_active_config_patch({{"fiber_finish_extension_length","80"},
        {"fiber_infill_angle_sequence","0"}}).success);
    libslicer::SliceRequest centered;centered.center_on_build_plate=false;
    centered.objects.push_back(fiber_infill_block());
    centered.config=*library->active_config_snapshot();
    const auto center=library->slice(centered);
    REQUIRE(center.success);REQUIRE(center.preview);
    const auto finish=std::find_if(center.preview->segments.begin(),center.preview->segments.end(),[](const auto& segment) {
        return segment.fiber_phase==libslicer::ToolpathFiberPhase::Finish &&
            std::abs(segment.end_mm.x-segment.start_mm.x)>70;
    });
    REQUIRE(finish!=center.preview->segments.end());
    const double translation=finish->end_mm.x>finish->start_mm.x?227:-68;
    std::filesystem::remove(center.output.path);
    // The profile excludes x<27 and x>392. Source fits at x=327..387 or
    // 32..92; only F crosses the usable area after the instance transform.
    for(const auto* extension:{"0","80"}) {
        REQUIRE(library->apply_active_config_patch({{"fiber_finish_extension_length",extension}}).success);
        libslicer::SliceRequest shifted;shifted.center_on_build_plate=false;
        auto object=fiber_infill_block();object.transform[3]=translation;
        shifted.objects.push_back(std::move(object));shifted.config=*library->active_config_snapshot();
        const auto result=library->slice(shifted);
        CAPTURE(extension);
        if(std::string(extension)=="0") {
            std::string diagnostics;for(const auto& d:result.diagnostics)diagnostics+=d.message+"\n";
            INFO(diagnostics);CAPTURE(translation);
            REQUIRE(result.success);std::filesystem::remove(result.output.path);
        } else {
            CHECK_FALSE(result.success);CHECK(result.output.path.empty());
            CHECK(std::any_of(result.diagnostics.begin(),result.diagnostics.end(),[](const auto& d) {
                return !d.warning && d.message.find("Fiber motion exceeds")!=std::string::npos;
            }));
        }
    }
}

TEST_CASE("CFSYS rejects fiber motion outside machine height limits", "[libslicer_api][fiber][fiber-motion-limit]")
{
    libslicer::LibraryOptions options;
    options.resource_directory=LIBSLICER_TEST_RESOURCE_DIR;options.vendors={"CFSYS"};
    auto library=libslicer::Library::open(options);
    libslicer::ConfigSelection selection;
    selection.machine_model_id="CFSYS Alpha500 Printer";selection.machine_variant_id="0.4";
    selection.process_preset_id="CCF&CIRON @CFSYS";
    selection.filament_preset_ids={"CFSYS CIRON","CFSYS CCF"};selection.filament_physical_tools={0,1};
    REQUIRE(library->activate_config(selection,{{"generate_reinforced_infills","1"},
        {"fiber_z_hop_height","600"}}).success);
    libslicer::SliceRequest request;request.objects.push_back(fiber_infill_block());
    request.config=*library->active_config_snapshot();
    const auto result=library->slice(request);
    CHECK_FALSE(result.success);CHECK(result.output.path.empty());
    CHECK(std::any_of(result.diagnostics.begin(),result.diagnostics.end(),[](const auto& d) {
        return !d.warning && d.message.find("Fiber motion exceeds")!=std::string::npos;
    }));
}

TEST_CASE("rectilinear infill exposes tangent finishing controls but not loop overlap", "[libslicer_api][fiber-rectilinear-finish]")
{
    auto config=libslicer::Config::defaults();
    REQUIRE(config.apply_patch({{"generate_reinforced_perimeters","0"},{"generate_reinforced_infills","1"},
        {"reinforced_infill_pattern","rectilinear"},{"fiber_finish_extension_length","23"}}).success);
    auto items=config.settings();
    for(const auto* key:{"fiber_finish_extension_length","fiber_finish_motion_speed"}) {
        const auto* item=find_item(items,key);REQUIRE(item);CHECK(item->visible);CHECK(item->enabled);
    }
    REQUIRE(find_item(items,"fiber_finish_overlap_length"));
    CHECK_FALSE(find_item(items,"fiber_finish_overlap_length")->visible);
    CHECK(config.snapshot().value("fiber_finish_extension_length")=="23");
    REQUIRE(config.set("generate_reinforced_perimeters","1").success);
    items=config.settings();CHECK(find_item(items,"fiber_finish_extension_length")->visible);
    REQUIRE(config.apply_patch({{"generate_reinforced_perimeters","0"},{"reinforced_infill_pattern","concentric"}}).success);
    items=config.settings();
    CHECK(find_item(items,"fiber_finish_extension_length")->visible);
    CHECK(find_item(items,"fiber_finish_overlap_length")->visible);
    CHECK(find_item(items,"fiber_finish_motion_speed")->enabled);
}

TEST_CASE("continuous fiber rejects a machine without a cut command", "[libslicer_api][fiber-missing-cut][slice]")
{
    libslicer::LibraryOptions options;
    options.resource_directory = LIBSLICER_TEST_RESOURCE_DIR;
    options.vendors = {"Flashforge"};
    auto library = libslicer::Library::open(options);
    REQUIRE(library != nullptr);

    libslicer::ConfigSelection selection;
    selection.machine_model_id = "Flashforge Creator 5";
    selection.machine_variant_id = "0.4";
    const auto activated = library->activate_config(selection, {
        {"generate_reinforced_perimeters", "1"},
        {"outer_reinforced_perimeters_counts", "1"},
        {"reinforced_perimeters_filament", "2"},
        {"fiber_minimum_path_length", "2"},
        {"fiber_cut_gcode", ""}
    });
    REQUIRE(activated.success);
    const auto config = library->active_config_snapshot();
    REQUIRE(config.has_value());

    libslicer::SliceRequest request;
    request.objects = {{std::string(LIBSLICER_TEST_DATA_DIR) + "/20mm_cube.obj", {}}};
    request.config = *config;
    request.generate_preview = false;
    const auto sliced = library->slice(request);

    CHECK_FALSE(sliced.success);
    REQUIRE_FALSE(sliced.diagnostics.empty());
    const bool reported_missing_cut = std::any_of(
        sliced.diagnostics.begin(), sliced.diagnostics.end(),
        [](const libslicer::SliceDiagnostic& diagnostic) {
            return diagnostic.message.find("no fiber cut command") != std::string::npos;
        });
    CHECK(reported_missing_cut);
}

TEST_CASE("continuous fiber composite fill survives an end-to-end slice", "[libslicer_api][fiber][slice]")
{
    const char* validation_model = std::getenv("LIBSLICER_FIBER_VALIDATION_MODEL");
    const int execution_case = GENERATE(0, 1, 2);
    const bool swapped_physical_tools = execution_case == 2;
    INFO("execution_case=" << execution_case);
    libslicer::LibraryOptions options;
    options.resource_directory = LIBSLICER_TEST_RESOURCE_DIR;
    options.vendors = {"Flashforge"};
    auto library = libslicer::Library::open(options);
    REQUIRE(library != nullptr);

    libslicer::ConfigSelection selection;
    // Keep the seed's four physical tools and define every tool/material.
    // Only the first two materials participate in this controlled fixture.
    selection.machine_model_id = "Flashforge Creator 5";
    selection.machine_variant_id = "0.4";
    const auto seed=library->create_config(selection);REQUIRE(seed);
    REQUIRE_FALSE(seed.selection.filament_preset_ids.empty());
    selection.filament_preset_ids.assign(4,seed.selection.filament_preset_ids.front());
    const auto activated = library->activate_config(selection, {
        {"generate_reinforced_perimeters", "1"},
        {"outer_reinforced_perimeters_counts", "2"},
        {"generate_reinforced_infills", "1"},
        {"reinforced_infill_density", "40%"},
        {"reinforced_infill_pattern", "rectilinear"},
        {"reinforced_perimeters_filament", "2"},
        {"reinforced_infill_filament", "2"},
        {"fiber_layer_height_ratio", "1"},
        {"fiber_minimum_path_length", "2"},
        {"fiber_contour_infill_clearance", "0"},
        {"fiber_resin_overlap", "0.05"},
        {"fiber_cut_to_contact_length", "5"},
        {"fiber_prefeed_extra_length", "0.5"},
        {"fiber_prefeed_speed", "10"},
        {"fiber_z_hop_height", "2"},
        {"fiber_landing_length", "0"},
        {"fiber_landing_speed", "3"},
        {"fiber_adhesion_dwell_ms", "25"},
        {"fiber_start_stabilization_length", "2"},
        {"fiber_start_speed", "10"},
        {"fiber_cut_gcode", "M400\nM42 P4 S255\nG4 P100\n; TEST_FIBER_CUT"},
        {"filament_process_type", "thermoplastic;continuous_fiber;thermoplastic;thermoplastic"},
        {"filament_fiber_feed_correction", "1,1,1,1"},
        {"toolhead_process_capabilities", swapped_physical_tools ? "continuous_fiber;thermoplastic;thermoplastic;thermoplastic" : "thermoplastic;continuous_fiber;thermoplastic;thermoplastic"},
        {"toolhead_fiber_protocol_id", swapped_physical_tools ? "linear-e-v1;;;" : ";linear-e-v1;;"},
        {"toolhead_fiber_e_units_per_mm", swapped_physical_tools ? "2,1,1,1" : "1,2,1,1"},
        {"use_relative_e_distances", execution_case == 0 ? "0" : "1"},
        {"z_offset", "0.7"},
        {"fiber_contour_feed_ratio", "1.02"},
        {"fiber_infill_feed_ratio", "1.05"},
        {"physical_extruder_map", swapped_physical_tools ? "1,0,2,3" : "0,1,2,3"},
        {"fiber_tail_min_speed", "3"},
        {"fiber_tail_max_speed", "3"},
        {"enable_prime_tower", "0"},
        {"wipe", "1"}
    });
    std::string activation_diagnostics;
    for (const auto& issue : activated.diagnostics) activation_diagnostics += issue.key + ": " + issue.message + "\n";
    INFO(activation_diagnostics);
    REQUIRE(activated.success);
    REQUIRE(library->active_config()->filament_slots.size()==4);
    // Synthetic, explicitly command-only machine. Production CFSYS firmware
    // macros are deliberately not treated as an audited software fixture.
    std::vector<std::pair<std::string, std::string>> script_patch;
    const auto before_scripts = library->active_config_snapshot();
    REQUIRE(before_scripts.has_value());
    for (const auto& [key, value] : before_scripts->values())
        if (key.size() >= 6 && key.compare(key.size()-6, 6, "_gcode") == 0 &&
            key != "fiber_cut_gcode" && key != "emit_machine_limits_to_gcode")
        {
            // Native filament scripts are string vectors. A bare empty string
            // means zero elements, not an empty script for each material.
            std::string empty_scripts;
            if(key.rfind("filament_",0)==0) {
                for(std::size_t i=0;i<library->active_config()->filament_slots.size();++i) {
                    if(i)empty_scripts+=';';empty_scripts+="\"\"";
                }
            }
            script_patch.emplace_back(key,empty_scripts);
        }
    const auto cleared_scripts = library->apply_active_config_patch(script_patch);
    std::string script_diagnostics;
    for (const auto& issue : cleared_scripts.diagnostics) script_diagnostics += issue.key + ": " + issue.message + "\n";
    INFO(script_diagnostics);
    REQUIRE(cleared_scripts.success);
    const auto config = library->active_config_snapshot();
    REQUIRE(config.has_value());

    const std::filesystem::path output =
        std::filesystem::temp_directory_path() / "libslicer_api_continuous_fiber_cube.gcode";
    std::error_code remove_error;
    std::filesystem::remove(output, remove_error);

    libslicer::SliceRequest request;
    if (validation_model) request.objects.push_back(binary_stl_on_bed(validation_model));
    else request.objects = {{std::string(LIBSLICER_TEST_DATA_DIR) + "/two_20mm_cubes.obj", {}}};
    request.config = *config;
    request.project_semantics=R"({"version":1,"scope":"plate","settings":{"filament_map_mode":"Manual","filament_map":"1,2,3,4","filament_printable":"15,15,15,15"},"custom_gcode_mode":0,"custom_gcodes":[]})";
    request.output_gcode_path = output.string();
    std::ostringstream execution_configuration;
    for(const char* key:{"nozzle_diameter","filament_process_type","toolhead_process_capabilities","physical_extruder_map","filament_map","filament_map_mode","filament_printable"})
        execution_configuration<<key<<"="<<config->value(key).value_or("<absent>")<<"\n";
    INFO(execution_configuration.str());
    const auto sliced = library->slice(request);
    const std::string diagnostic = sliced.diagnostics.empty()
        ? std::string{}
        : sliced.diagnostics.front().message;
    INFO(diagnostic);
    REQUIRE(sliced.success);
    REQUIRE(sliced.preview != nullptr);

    std::size_t contour_segments = 0;
    std::size_t infill_segments = 0;
    std::size_t resin_segments = 0;
    std::size_t passive_segments = 0;
    for (const auto& segment : sliced.preview->segments) {
        if (segment.deposition == libslicer::ToolpathDepositionKind::ContinuousFiberPassive) {
            ++passive_segments;
            CHECK(segment.motion == libslicer::ToolpathMotionKind::Travel);
            CHECK(segment.fiber_feed_delta_mm == 0);
            CHECK(segment.width_mm > 0);
            CHECK(segment.height_mm > 0);
        }
        if (segment.deposition == libslicer::ToolpathDepositionKind::ContinuousFiberPowered) {
            CHECK(segment.filament_id == 1);
            REQUIRE(segment.tool_id == (swapped_physical_tools ? 0 : 1));
            const double ratio = segment.extrusion_role == libslicer::ToolpathExtrusionRole::ContinuousFiberContour ? 1.02 : 1.05;
            CHECK(segment.fiber_feed_delta_mm == Catch::Approx(segment.deposited_path_length_mm * ratio).margin(0.002));
            CHECK(segment.end_mm.z > 0);
            CHECK(segment.start_mm.z == Catch::Approx(segment.end_mm.z));
            CHECK(segment.extrusion_delta_mm == 0); // not plastic filament millimeters
        }
        if (segment.extrusion_role == libslicer::ToolpathExtrusionRole::ContinuousFiberContour)
            ++contour_segments;
        else if (segment.extrusion_role == libslicer::ToolpathExtrusionRole::ContinuousFiberInfill)
            ++infill_segments;
        else if (segment.extrusion_role == libslicer::ToolpathExtrusionRole::ResinInfill)
            ++resin_segments;
    }
    INFO("contour_segments=" << contour_segments
         << " infill_segments=" << infill_segments
         << " resin_segments=" << resin_segments);
    CHECK(contour_segments > 0);
    CHECK(infill_segments > 0);
    CHECK(resin_segments > 0);
    CHECK(passive_segments > 0);
    CHECK(sliced.preview->statistics.total_fiber_feed_mm > 0);
    CHECK(sliced.preview->statistics.total_fiber_prefeed_mm > 0);
    libslicer::GCodePreviewRequest import_request;
    import_request.gcode_path = output.string();
    const auto imported_fiber = library->load_gcode_preview(import_request);
    for (const auto& issue : imported_fiber.diagnostics) UNSCOPED_INFO(issue.message);
    REQUIRE(imported_fiber.success);
    REQUIRE(imported_fiber.preview);
    for (const auto& segment : imported_fiber.preview->segments)
        if (segment.deposition == libslicer::ToolpathDepositionKind::ContinuousFiberPowered ||
            segment.deposition == libslicer::ToolpathDepositionKind::ContinuousFiberPassive)
            REQUIRE(segment.tool_id == (swapped_physical_tools ? 0 : 1));
    CHECK(imported_fiber.preview->statistics.total_fiber_feed_mm ==
          Catch::Approx(sliced.preview->statistics.total_fiber_feed_mm).margin(0.02));
    CHECK(imported_fiber.preview->statistics.total_fiber_deposited_path_mm ==
          Catch::Approx(sliced.preview->statistics.total_fiber_deposited_path_mm).margin(0.02));

    std::ifstream generated(output);
    const std::string gcode((std::istreambuf_iterator<char>(generated)),
                            std::istreambuf_iterator<char>());
    CHECK(gcode.find(";FIBER_BEGIN") != std::string::npos);
    CHECK(gcode.find("purpose=contour") != std::string::npos);
    CHECK(gcode.find("purpose=infill") != std::string::npos);
    CHECK(gcode.find("component=0") != std::string::npos);
    if (validation_model == nullptr)
        CHECK(gcode.find("component=1") != std::string::npos);
    CHECK(gcode.find(";FIBER_CUT") != std::string::npos);
    CHECK(gcode.find("; TEST_FIBER_CUT") != std::string::npos);

    std::istringstream gcode_lines(gcode);
    std::string gcode_line;
    bool inside_fiber_block = false;
    bool awaiting_first_motion_after_end = false;
    bool invalid_block_nesting = false;
    bool protected_block_rewritten = false;
    bool post_end_wipe = false;
    bool block_saw_prefeed = false;
    bool block_saw_landing = false;
    bool block_saw_dwell = false;
    bool block_saw_start = false;
    bool block_saw_powered_deposition = false;
    bool invalid_start_sequence = false;
    std::size_t fiber_begin_count = 0;
    std::size_t fiber_end_count = 0;
    std::size_t valid_start_sequence_count = 0;
    while (std::getline(gcode_lines, gcode_line)) {
        if (gcode_line.rfind(";FIBER_BEGIN", 0) == 0) {
            invalid_block_nesting = invalid_block_nesting || inside_fiber_block;
            inside_fiber_block = true;
            awaiting_first_motion_after_end = false;
            block_saw_prefeed = false;
            block_saw_landing = false;
            block_saw_dwell = false;
            block_saw_start = false;
            block_saw_powered_deposition = false;
            ++fiber_begin_count;
        }
        if (inside_fiber_block) {
            protected_block_rewritten = protected_block_rewritten ||
                gcode_line.rfind(";WIPE", 0) == 0 ||
                gcode_line.find(";_WIPE") != std::string::npos ||
                gcode_line.rfind("M106", 0) == 0 ||
                gcode_line.rfind("M107", 0) == 0;

            const bool is_linear_move = gcode_line.rfind("G1 ", 0) == 0;
            const bool has_x = gcode_line.find(" X") != std::string::npos;
            const bool has_y = gcode_line.find(" Y") != std::string::npos;
            const bool has_z = gcode_line.find(" Z") != std::string::npos;
            const bool has_e = gcode_line.find(" E") != std::string::npos;
            if (!block_saw_start && is_linear_move && has_e && !has_x && !has_y)
                block_saw_prefeed = true;
            if (!block_saw_start && block_saw_prefeed && is_linear_move &&
                (has_x || has_y) && has_z && !has_e)
                block_saw_landing = true;
            if (!block_saw_start && gcode_line.rfind("G4 P25", 0) == 0)
                block_saw_dwell = true;
            if (gcode_line.rfind(";FIBER_START", 0) == 0) {
                invalid_start_sequence = invalid_start_sequence || block_saw_start ||
                    !block_saw_prefeed || !block_saw_landing || !block_saw_dwell;
                block_saw_start = true;
            } else if (block_saw_start && is_linear_move && (has_x || has_y)) {
                if (!block_saw_powered_deposition) {
                    invalid_start_sequence = invalid_start_sequence || !has_e;
                    block_saw_powered_deposition = has_e;
                }
            } else if (!block_saw_start && is_linear_move && (has_x || has_y) && has_e) {
                invalid_start_sequence = true;
            }
        }
        if (gcode_line.rfind(";FIBER_END", 0) == 0) {
            invalid_block_nesting = invalid_block_nesting || !inside_fiber_block;
            invalid_start_sequence = invalid_start_sequence || !block_saw_start ||
                !block_saw_powered_deposition;
            if (block_saw_start && block_saw_powered_deposition)
                ++valid_start_sequence_count;
            inside_fiber_block = false;
            awaiting_first_motion_after_end = true;
            ++fiber_end_count;
            continue;
        }
        if (awaiting_first_motion_after_end &&
            (gcode_line.rfind(";WIPE", 0) == 0 ||
             gcode_line.find(";_WIPE") != std::string::npos))
            post_end_wipe = true;
        if (awaiting_first_motion_after_end &&
            (gcode_line.rfind("G0 ", 0) == 0 || gcode_line.rfind("G1 ", 0) == 0)) {
            awaiting_first_motion_after_end = false;
        }
    }
    CHECK_FALSE(inside_fiber_block);
    CHECK_FALSE(invalid_block_nesting);
    CHECK_FALSE(protected_block_rewritten);
    CHECK_FALSE(post_end_wipe);
    CHECK_FALSE(invalid_start_sequence);
    CHECK(fiber_begin_count > 0);
    CHECK(fiber_begin_count == fiber_end_count);
    CHECK(valid_start_sequence_count == fiber_begin_count);

    const std::size_t tail_begin = gcode.find(";FIBER_TAIL_BEGIN");
    const std::size_t depleted = tail_begin == std::string::npos ?
        std::string::npos : gcode.find(";FIBER_DEPLETED", tail_begin);
    REQUIRE(tail_begin != std::string::npos);
    REQUIRE(depleted != std::string::npos);
    const std::string passive_tail = gcode.substr(tail_begin, depleted - tail_begin);
    std::istringstream passive_tail_lines(passive_tail);
    std::string passive_line;
    bool has_passive_xy_motion = false;
    while (std::getline(passive_tail_lines, passive_line)) {
        if (passive_line.rfind("G1 ", 0) != 0)
            continue;
        has_passive_xy_motion = has_passive_xy_motion ||
            (passive_line.find(" X") != std::string::npos || passive_line.find(" Y") != std::string::npos);
        CHECK(passive_line.find(" E") == std::string::npos);
    }
    CHECK(has_passive_xy_motion);
    if (execution_case == 1) {
        // A rejected machine event must not destroy a previously valid output.
        REQUIRE(library->apply_active_config_patch({{"fiber_cut_gcode", "UNKNOWN_CUT_MACRO"}}).success);
        request.config = *library->active_config_snapshot();
        const auto rejected = library->slice(request);
        REQUIRE_FALSE(rejected.success);
        std::ifstream retained(output);
        const std::string retained_gcode((std::istreambuf_iterator<char>(retained)), std::istreambuf_iterator<char>());
        CHECK(retained_gcode == gcode);
        CHECK_FALSE(std::filesystem::exists(output.string() + ".tmp"));
    }
    std::filesystem::remove(output, remove_error);
}

TEST_CASE("disabling continuous fiber preserves the ordinary Orca fill path", "[libslicer_api][fiber-disabled][slice]")
{
    libslicer::LibraryOptions options;
    options.resource_directory = LIBSLICER_TEST_RESOURCE_DIR;
    options.vendors = {"Flashforge"};
    auto library = libslicer::Library::open(options);
    REQUIRE(library != nullptr);

    libslicer::ConfigSelection selection;
    selection.machine_model_id = "Flashforge Creator 5";
    selection.machine_variant_id = "0.4";
    const auto activated = library->activate_config(selection, {
        {"generate_reinforced_perimeters", "0"},
        {"generate_reinforced_infills", "0"}
    });
    REQUIRE(activated.success);
    const auto config = library->active_config_snapshot();
    REQUIRE(config.has_value());

    const std::filesystem::path output =
        std::filesystem::temp_directory_path() / "libslicer_api_fiber_disabled_cube.gcode";
    std::error_code remove_error;
    std::filesystem::remove(output, remove_error);

    libslicer::SliceRequest request;
    request.objects = {{std::string(LIBSLICER_TEST_DATA_DIR) + "/20mm_cube.obj", {}}};
    request.config = *config;
    request.output_gcode_path = output.string();
    const auto sliced = library->slice(request);
    const std::string diagnostic = sliced.diagnostics.empty()
        ? std::string{}
        : sliced.diagnostics.front().message;
    INFO(diagnostic);
    REQUIRE(sliced.success);
    REQUIRE(sliced.preview != nullptr);

    std::size_t contour_segments = 0;
    std::size_t infill_segments = 0;
    std::size_t sparse_segments = 0;
    for (const auto& segment : sliced.preview->segments) {
        contour_segments += segment.extrusion_role ==
            libslicer::ToolpathExtrusionRole::ContinuousFiberContour;
        infill_segments += segment.extrusion_role ==
            libslicer::ToolpathExtrusionRole::ContinuousFiberInfill;
        sparse_segments += segment.extrusion_role ==
            libslicer::ToolpathExtrusionRole::SparseInfill;
    }
    CHECK(contour_segments == 0);
    CHECK(infill_segments == 0);
    CHECK(sparse_segments > 0);

    std::ifstream generated(output);
    const std::string gcode((std::istreambuf_iterator<char>(generated)),
                            std::istreambuf_iterator<char>());
    CHECK(gcode.find(";FIBER_BEGIN") == std::string::npos);
    CHECK(gcode.find(";FIBER_END") == std::string::npos);
    std::filesystem::remove(output, remove_error);
}

TEST_CASE("library owns machine presets and builds a selected configuration", "[libslicer_api][presets]")
{
    libslicer::LibraryOptions options;
    options.resource_directory = LIBSLICER_TEST_RESOURCE_DIR;
    options.vendors = {"Flashforge"};

    std::vector<libslicer::ConfigDiagnostic> diagnostics;
    auto library = libslicer::Library::open(options, &diagnostics);
    const std::string open_diagnostic = diagnostics.empty() ? std::string{} : diagnostics.front().message;
    INFO(open_diagnostic);
    REQUIRE(library != nullptr);
    REQUIRE_FALSE(library->machine_models().empty());
    REQUIRE(library->build_plate_options().size() == 6);
    for (const auto& plate : library->build_plate_options()) {
        CHECK_FALSE(plate.value.empty());
        CHECK_FALSE(plate.name.empty());
        CHECK(std::filesystem::is_regular_file(plate.image_path));
    }

    const auto machine = std::find_if(library->machine_models().begin(), library->machine_models().end(),
                                      [](const libslicer::MachineModelOption& item) {
                                          return item.id == "Flashforge AD5X";
                                      });
    REQUIRE(machine != library->machine_models().end());
    CHECK(std::filesystem::is_regular_file(machine->cover_image_path));
    CHECK_FALSE(machine->bed_model_path.empty());
    CHECK(std::filesystem::is_regular_file(machine->bed_model_path));
    CHECK(std::filesystem::path(machine->bed_texture_path).extension() == ".png");
    CHECK(std::filesystem::is_regular_file(machine->bed_texture_path));
    const auto nozzle = std::find_if(machine->variants.begin(), machine->variants.end(),
                                     [](const libslicer::MachineVariantOption& item) {
                                         return item.id == "0.4";
                                     });
    REQUIRE(nozzle != machine->variants.end());
    CHECK(nozzle->nozzle_diameter == 0.4);
    CHECK(nozzle->printable_width == 220.0);
    CHECK(nozzle->printable_depth == 220.0);
    CHECK(nozzle->printable_height == 220.0);
    REQUIRE(nozzle->printable_area.size() == 4);
    CHECK(nozzle->printable_area.front().x == 0.0);
    CHECK(nozzle->printable_area.front().y == 0.0);
    CHECK(nozzle->printable_area[2].x == 220.0);
    CHECK(nozzle->printable_area[2].y == 220.0);
    CHECK_FALSE(nozzle->printer_preset_id.empty());
    CHECK(nozzle->variable_filament_slots);
    CHECK(nozzle->max_filament_slots == 4);

    libslicer::ConfigSelection selection;
    selection.machine_model_id = machine->id;
    selection.machine_variant_id = nozzle->id;
    auto created = library->create_config(selection);
    const std::string create_diagnostic = created.diagnostics.empty() ? std::string{} : created.diagnostics.front().message;
    INFO(create_diagnostic);
    REQUIRE(created.success);
    REQUIRE(created.config != nullptr);
    CHECK(created.selection.machine_model_id == "Flashforge AD5X");
    CHECK(created.selection.machine_variant_id == "0.4");
    CHECK_FALSE(created.selection.process_preset_id.empty());
    REQUIRE_FALSE(created.selection.filament_preset_ids.empty());
    CHECK(created.config->snapshot().value("nozzle_diameter") == "0.4");
    CHECK(created.config->snapshot().value("printable_height") == "220");

    selection.process_preset_id = "missing process preset";
    const auto missing_process = library->create_config(selection);
    CHECK_FALSE(missing_process.success);
    REQUIRE_FALSE(missing_process.diagnostics.empty());
    CHECK(missing_process.diagnostics.front().key == "process");

    selection.process_preset_id.clear();
    selection.filament_preset_ids = {"missing filament preset"};
    const auto missing_filament = library->create_config(selection);
    CHECK_FALSE(missing_filament.success);
    REQUIRE_FALSE(missing_filament.diagnostics.empty());
    CHECK(missing_filament.diagnostics.front().key == "filament");
}

TEST_CASE("library resizes variable filament slots atomically", "[libslicer_api][filaments]")
{
    libslicer::LibraryOptions options;
    options.resource_directory = LIBSLICER_TEST_RESOURCE_DIR;
    options.vendors = {"Flashforge"};
    auto library = libslicer::Library::open(options);
    REQUIRE(library != nullptr);

    libslicer::ConfigSelection selection;
    selection.machine_model_id = "Flashforge AD5X";
    selection.machine_variant_id = "0.4";
    auto activated = library->activate_config(selection);
    REQUIRE(activated.success);
    REQUIRE(activated.view.filament_slots.size() == 1);
    REQUIRE(library->set_active_filament_color(0, {0x24, 0x74, 0xd8, 0xff}));

    const auto resized = library->resize_active_filament_slots(4);
    REQUIRE(resized.success);
    REQUIRE(resized.view.filament_slots.size() == 4);
    CHECK(resized.view.selection.filament_preset_ids.size() == 4);
    CHECK(resized.view.filament_slots[0].color.red == 0x24);
    CHECK(resized.view.filament_slots[3].color.blue == 0xd8);
    CHECK(library->validate_active_config().empty());

    const auto rejected = library->resize_active_filament_slots(5);
    CHECK_FALSE(rejected.success);
    REQUIRE_FALSE(rejected.diagnostics.empty());
    CHECK(library->active_config()->filament_slots.size() == 4);
}

TEST_CASE("library owns one normalized active filament configuration", "[libslicer_api][filaments]")
{
    libslicer::LibraryOptions options;
    options.resource_directory = LIBSLICER_TEST_RESOURCE_DIR;
    options.vendors = {"Flashforge"};
    auto library = libslicer::Library::open(options);
    REQUIRE(library != nullptr);

    const auto machine = std::find_if(
        library->machine_models().begin(), library->machine_models().end(),
        [](const libslicer::MachineModelOption& item) {
            return item.id == "Flashforge Creator 5";
        });
    REQUIRE(machine != library->machine_models().end());
    const auto variant = std::find_if(
        machine->variants.begin(), machine->variants.end(),
        [](const libslicer::MachineVariantOption& item) { return item.id == "0.4"; });
    REQUIRE(variant != machine->variants.end());
    CHECK(variant->physical_tool_count == 4);
    CHECK_FALSE(variant->variable_filament_slots);

    libslicer::ConfigSelection selection;
    selection.machine_model_id = machine->id;
    selection.machine_variant_id = variant->id;
    auto activated = library->activate_config(
        selection, {{"filament_colour", "#11223344"}});
    const std::string diagnostic = activated.diagnostics.empty()
        ? std::string{}
        : activated.diagnostics.front().key + ": " + activated.diagnostics.front().message;
    INFO(diagnostic);
    REQUIRE(activated.success);
    REQUIRE(activated.view.filament_slots.size() == 4);
    CHECK(activated.view.selection.filament_preset_ids.size() == 4);
    CHECK(activated.view.filament_slots.front().color.red == 0x11);
    CHECK(activated.view.filament_slots.front().color.green == 0x22);
    CHECK(activated.view.filament_slots.front().color.blue == 0x33);
    CHECK(activated.view.filament_slots.front().color.alpha == 0x44);
    CHECK(library->validate_active_config().empty());
    const auto snapshot = library->active_config_snapshot();
    REQUIRE(snapshot.has_value());
    const auto flush_multipliers = snapshot->value("flush_multiplier");
    const auto flush_matrix = snapshot->value("flush_volumes_matrix");
    REQUIRE(flush_multipliers.has_value());
    REQUIRE(flush_matrix.has_value());
    CHECK(std::count(flush_multipliers->begin(), flush_multipliers->end(), ',') + 1 == 4);
    CHECK(std::count(flush_matrix->begin(), flush_matrix->end(), ',') + 1 == 64);

    const auto initial_revision = activated.view.revision;
    const auto changed_color = library->set_active_filament_color(
        3, {0xaa, 0xbb, 0xcc, 0xdd});
    REQUIRE(changed_color.success);
    const auto view = library->active_config();
    REQUIRE(view.has_value());
    CHECK(view->revision > initial_revision);
    REQUIRE(view->filament_slots.size() == 4);
    CHECK(view->filament_slots[3].color.red == 0xaa);
    CHECK(view->filament_slots[3].color.green == 0xbb);
    CHECK(view->filament_slots[3].color.blue == 0xcc);
    CHECK(view->filament_slots[3].color.alpha == 0xdd);

    const auto invalid_slot = library->set_active_filament_color(
        4, {0, 0, 0, 255});
    CHECK_FALSE(invalid_slot.success);
    REQUIRE_FALSE(invalid_slot.diagnostics.empty());
    CHECK(invalid_slot.diagnostics.front().key == "filament");
}

TEST_CASE("library rejects mismatched filament cardinality before slicing", "[libslicer_api][slice][filaments]")
{
    libslicer::LibraryOptions options;
    options.resource_directory = LIBSLICER_TEST_RESOURCE_DIR;
    options.vendors = {"Flashforge"};
    auto library = libslicer::Library::open(options);
    REQUIRE(library != nullptr);

    libslicer::ConfigSelection selection;
    selection.machine_model_id = "Flashforge Creator 5";
    selection.machine_variant_id = "0.4";
    auto created = library->create_config(selection);
    REQUIRE(created.success);
    REQUIRE(created.config != nullptr);
    REQUIRE(created.config->set("filament_colour", "#11223344").success);

    libslicer::SliceRequest request;
    request.objects = {{std::string(LIBSLICER_TEST_DATA_DIR) + "/20mm_cube.obj", {}}};
    request.config = created.config->snapshot();
    const auto sliced = library->slice(request);
    CHECK_FALSE(sliced.success);
    REQUIRE_FALSE(sliced.diagnostics.empty());
    CHECK(sliced.diagnostics.front().code == "filament");
    CHECK(sliced.diagnostics.front().message.find("filament colours") != std::string::npos);
}

TEST_CASE("library slices a model with a preset-backed configuration", "[libslicer_api][slice]")
{
    libslicer::LibraryOptions options;
    options.resource_directory = LIBSLICER_TEST_RESOURCE_DIR;
    options.vendors = {"Flashforge"};
    auto library = libslicer::Library::open(options);
    REQUIRE(library != nullptr);

    libslicer::ConfigSelection selection;
    selection.machine_model_id = "Flashforge AD5X";
    selection.machine_variant_id = "0.4";
    auto created = library->create_config(selection);
    REQUIRE(created.success);
    REQUIRE(created.config != nullptr);

    const std::filesystem::path output =
        std::filesystem::temp_directory_path() / "libslicer_api_20mm_cube.gcode";
    const std::filesystem::path packaged_output =
        std::filesystem::temp_directory_path() / "libslicer_api_20mm_cube.gcode.3mf";
    std::error_code remove_error;
    std::filesystem::remove(output, remove_error);
    std::filesystem::remove(packaged_output, remove_error);

    libslicer::SliceRequest request;
    request.objects = {{std::string(LIBSLICER_TEST_DATA_DIR) + "/20mm_cube.obj", {}}};
    const auto missing_config = library->slice(request);
    CHECK_FALSE(missing_config.success);
    REQUIRE_FALSE(missing_config.diagnostics.empty());
    CHECK(missing_config.diagnostics.front().code == "config");

    request.config = created.config->snapshot();
    request.output_gcode_path = output.string();
    request.output_gcode_3mf_path = packaged_output.string();

    const auto original_relative = created.config->snapshot().value(
        "use_relative_e_distances");
    const auto original_before_layer = created.config->snapshot().value(
        "before_layer_change_gcode");
    const auto original_layer = created.config->snapshot().value(
        "layer_change_gcode");
    REQUIRE(original_relative.has_value());
    REQUIRE(original_before_layer.has_value());
    REQUIRE(original_layer.has_value());
    // Missing per-layer E resets are invalid for Marlin, not this preset's Klipper flavor.
    const auto original_flavor = created.config->snapshot().value("gcode_flavor");
    REQUIRE(original_flavor.has_value());
    REQUIRE(created.config->set("gcode_flavor", "marlin").success);
    REQUIRE(created.config->set("use_relative_e_distances", "1").success);
    REQUIRE(created.config->set("before_layer_change_gcode", "").success);
    REQUIRE(created.config->set("layer_change_gcode", "").success);
    request.config = created.config->snapshot();
    const auto invalid = library->slice(request);
    REQUIRE_FALSE(invalid.success);
    const auto validation = std::find_if(
        invalid.diagnostics.begin(), invalid.diagnostics.end(),
        [](const libslicer::SliceDiagnostic& diagnostic) {
            return diagnostic.code == "validation" && !diagnostic.warning;
        });
    REQUIRE(validation != invalid.diagnostics.end());
    CHECK(validation->option_key == "before_layer_change_gcode");

    REQUIRE(created.config->set("gcode_flavor", *original_flavor).success);
    REQUIRE(created.config->set("use_relative_e_distances", *original_relative).success);
    REQUIRE(created.config->set("before_layer_change_gcode", *original_before_layer).success);
    REQUIRE(created.config->set("layer_change_gcode", *original_layer).success);
    request.config = created.config->snapshot();

    float last_progress = 0.0f;
    libslicer::SliceCallbacks callbacks;
    callbacks.progress = [&last_progress](float progress, std::string_view) {
        CHECK(progress >= 0.0f);
        CHECK(progress <= 1.0f);
        last_progress = std::max(last_progress, progress);
    };
    const auto sliced = library->slice(request, callbacks);
    const std::string diagnostic = sliced.diagnostics.empty() ? std::string{} : sliced.diagnostics.front().message;
    INFO(diagnostic);
    REQUIRE(sliced.success);
    CHECK_FALSE(sliced.cancelled);
    CHECK(sliced.output.path == output.string());
    CHECK(sliced.output.ownership == libslicer::OutputArtifactOwnership::CallerOwned);
    CHECK(sliced.gcode_3mf.path == packaged_output.string());
    CHECK(sliced.gcode_3mf.ownership == libslicer::OutputArtifactOwnership::CallerOwned);
    CHECK(std::filesystem::file_size(output) > 0);
    CHECK(std::filesystem::file_size(packaged_output) > 0);
    std::ifstream packaged_stream(packaged_output, std::ios::binary);
    char zip_signature[4]{};
    packaged_stream.read(zip_signature, sizeof(zip_signature));
    CHECK(std::string(zip_signature, sizeof(zip_signature)) == std::string("PK\x03\x04", 4));
    packaged_stream.clear();
    packaged_stream.seekg(0);
    const std::string packaged_content((std::istreambuf_iterator<char>(packaged_stream)),
                                       std::istreambuf_iterator<char>());
    CHECK(packaged_content.find("Metadata/plate_1.gcode") != std::string::npos);
    CHECK(packaged_content.find("Metadata/plate_1.gcode.md5") != std::string::npos);
    CHECK(packaged_content.find("Metadata/plate_1.png") != std::string::npos);
    CHECK(packaged_content.find("Metadata/plate_1_small.png") != std::string::npos);
    CHECK(sliced.summary.layer_count > 0);
    CHECK(sliced.summary.logical_motion_count > 0);
    CHECK(sliced.summary.render_segment_count > 0);
    REQUIRE(sliced.preview != nullptr);
    CHECK(sliced.preview->statistics.total_layers == sliced.summary.layer_count);
    CHECK(sliced.preview->statistics.logical_motion_count == sliced.summary.logical_motion_count);
    CHECK(sliced.preview->statistics.render_segment_count == sliced.summary.render_segment_count);
    CHECK(sliced.preview->schema_version == libslicer::toolpath_schema_version);
    const std::vector<libslicer::ToolpathViewType> expected_view_types = {
        libslicer::ToolpathViewType::Summary,
        libslicer::ToolpathViewType::FeatureType,
        libslicer::ToolpathViewType::Filament,
        libslicer::ToolpathViewType::Speed,
        libslicer::ToolpathViewType::ActualSpeed,
        libslicer::ToolpathViewType::Acceleration,
        libslicer::ToolpathViewType::Jerk,
        libslicer::ToolpathViewType::LayerHeight,
        libslicer::ToolpathViewType::LineWidth,
        libslicer::ToolpathViewType::VolumetricFlow,
        libslicer::ToolpathViewType::ActualVolumetricFlow,
        libslicer::ToolpathViewType::LayerTime,
        libslicer::ToolpathViewType::LayerTimeLogarithmic,
        libslicer::ToolpathViewType::FanSpeed,
        libslicer::ToolpathViewType::Temperature,
        libslicer::ToolpathViewType::PressureAdvance
    };
    CHECK(sliced.preview->supported_view_types == expected_view_types);
    CHECK_FALSE(sliced.preview->segments.empty());
    CHECK(sliced.preview->bounds.valid);
    CHECK_FALSE(sliced.preview->layers.empty());
    CHECK_FALSE(sliced.preview->colors.empty());
    CHECK(last_progress == 1.0f);

    const auto count_extrusion_role = [](const libslicer::ToolpathPreview& preview,
                                         libslicer::ToolpathExtrusionRole role) {
        return std::count_if(preview.segments.begin(), preview.segments.end(), [role](const auto& segment) {
            return segment.motion == libslicer::ToolpathMotionKind::Extrusion &&
                segment.extrusion_role == role && segment.extrusion_delta_mm > 0.0f;
        });
    };
    const auto sliced_sparse_infill = count_extrusion_role(
        *sliced.preview, libslicer::ToolpathExtrusionRole::SparseInfill);
    const auto sliced_solid_infill = count_extrusion_role(
        *sliced.preview, libslicer::ToolpathExtrusionRole::InternalSolidInfill);
    CHECK(sliced_sparse_infill > 0);
    CHECK(sliced_solid_infill > 0);
    CHECK(std::all_of(sliced.preview->segments.begin(), sliced.preview->segments.end(), [](const auto& segment) {
        const auto finite = [](float value) { return std::isfinite(value); };
        const bool endpoints_valid = finite(segment.start_mm.x) && finite(segment.start_mm.y) &&
            finite(segment.start_mm.z) && finite(segment.end_mm.x) && finite(segment.end_mm.y) &&
            finite(segment.end_mm.z);
        const bool is_extrusion = segment.motion == libslicer::ToolpathMotionKind::Extrusion;
        const bool role_valid = is_extrusion ||
            segment.extrusion_role == libslicer::ToolpathExtrusionRole::None;
        const bool dimensions_valid = is_extrusion ||
            (segment.width_mm == 0.0f && segment.height_mm == 0.0f &&
             segment.mm3_per_mm == 0.0f && segment.extrusion_delta_mm == 0.0f);
        const bool scalar_values_valid = finite(segment.duration_seconds) &&
            finite(segment.layer_duration_seconds) && finite(segment.fan_speed_percent) &&
            finite(segment.temperature_c) && finite(segment.pressure_advance) &&
            finite(segment.acceleration_mm_s2) && finite(segment.jerk_mm_s);
        return endpoints_valid && role_valid && dimensions_valid && scalar_values_valid;
    }));
    for (std::size_t index = 1; index < sliced.preview->segments.size(); ++index) {
        const auto& previous = sliced.preview->segments[index - 1];
        const auto& current = sliced.preview->segments[index];
        if (previous.run_id != current.run_id)
            continue;
        CHECK(previous.layer_index == current.layer_index);
        CHECK(previous.motion == current.motion);
        CHECK(previous.extrusion_role == current.extrusion_role);
        CHECK(std::abs(previous.end_mm.x - current.start_mm.x) < 0.000001f);
        CHECK(std::abs(previous.end_mm.y - current.start_mm.y) < 0.000001f);
        CHECK(std::abs(previous.end_mm.z - current.start_mm.z) < 0.000001f);
    }
    for (std::size_t layer_index = 0; layer_index < sliced.preview->layers.size(); ++layer_index) {
        const auto& layer = sliced.preview->layers[layer_index];
        CHECK(layer.index == layer_index);
        CHECK(layer.segment_begin + layer.segment_count <= sliced.preview->segments.size());
        CHECK(layer.event_begin + layer.event_count <= sliced.preview->events.size());
        if (layer_index + 1 < sliced.preview->layers.size()) {
            CHECK(layer.segment_begin + layer.segment_count ==
                  sliced.preview->layers[layer_index + 1].segment_begin);
        }
    }
    const auto sparse_stats = std::find_if(
        sliced.preview->statistics.features.begin(), sliced.preview->statistics.features.end(),
        [](const auto& feature) {
            return feature.role == libslicer::ToolpathExtrusionRole::SparseInfill;
        });
    REQUIRE(sparse_stats != sliced.preview->statistics.features.end());
    CHECK(sparse_stats->path_count > 0);
    CHECK(sparse_stats->length_mm > 0.0);
    CHECK(sparse_stats->extrusion_volume_mm3 > 0.0);
    CHECK(sparse_stats->duration_seconds > 0.0);
    CHECK(sparse_stats->filament_length_m > 0.0);
    CHECK(sparse_stats->filament_weight_g > 0.0);
    CHECK_FALSE(sliced.preview->statistics.filament_usage.empty());
    CHECK(sliced.preview->statistics.total_filament_length_mm > 0.0);
    CHECK(sliced.preview->statistics.total_filament_weight_g > 0.0);
    CHECK_FALSE(sliced.preview->statistics.options.empty());
    CHECK(sliced.preview->statistics.max_actual_speed_mm_s > 0.0f);
    CHECK(sliced.preview->statistics.max_actual_volumetric_flow_mm3_s > 0.0f);
    CHECK(sliced.preview->statistics.max_layer_time_seconds > 0.0f);

    std::ifstream gcode(output);
    const std::string content((std::istreambuf_iterator<char>(gcode)), std::istreambuf_iterator<char>());
    CHECK(content.find("G1") != std::string::npos);

    libslicer::GCodePreviewRequest preview_request;
    preview_request.gcode_path = output.string();
    const auto imported = library->load_gcode_preview(preview_request);
    REQUIRE(imported.success);
    REQUIRE(imported.preview != nullptr);
    CHECK(imported.preview->schema_version == libslicer::toolpath_schema_version);
    CHECK(imported.preview->supported_view_types == expected_view_types);
    CHECK(imported.preview->statistics.total_layers == sliced.preview->statistics.total_layers);
    CHECK(imported.preview->statistics.render_segment_count > 0);
    CHECK_FALSE(imported.preview->statistics.features.empty());
    CHECK_FALSE(imported.preview->statistics.filament_usage.empty());
    CHECK(imported.preview->statistics.total_filament_length_mm > 0.0);
    CHECK(imported.preview->source_path == output.string());
    CHECK(count_extrusion_role(*imported.preview,
                               libslicer::ToolpathExtrusionRole::SparseInfill) > 0);
    CHECK(count_extrusion_role(*imported.preview,
                               libslicer::ToolpathExtrusionRole::InternalSolidInfill) > 0);
    std::filesystem::remove(output, remove_error);
    std::filesystem::remove(packaged_output, remove_error);
}

TEST_CASE("library imports generic G-code without embedded Orca configuration",
          "[libslicer_api][gcode][preview]")
{
    const auto gcode_path =
        std::filesystem::temp_directory_path() /
        "libslicer_api_generic_external.gcode";
    std::error_code remove_error;
    std::filesystem::remove(gcode_path, remove_error);

    {
        std::ofstream gcode(gcode_path, std::ios::binary);
        REQUIRE(gcode.good());
        gcode << R"(;Generated with Cura_SteamEngine 4.7.0
G21
G90
M82
G92 E0
;LAYER_COUNT:2
;LAYER:0
G1 Z0.2 F1200
;TYPE:WALL-OUTER
G1 X0 Y0 F3000
G1 X20 Y0 E1 F1200
G1 X20 Y20 E2
G1 X0 Y20 E3
G1 X0 Y0 E4
;LAYER:1
G1 Z0.4 F1200
;TYPE:WALL-OUTER
G1 X20 Y0 E5 F1200
G1 X20 Y20 E6
G1 X0 Y20 E7
G1 X0 Y0 E8
)";
        REQUIRE(gcode.good());
    }

    libslicer::LibraryOptions options;
    options.resource_directory = LIBSLICER_TEST_RESOURCE_DIR;
    options.vendors = {"Flashforge"};
    const auto library = libslicer::Library::open(options);
    REQUIRE(library != nullptr);

    libslicer::GCodePreviewRequest request;
    request.gcode_path = gcode_path.string();
    const auto imported = library->load_gcode_preview(request);
    REQUIRE(imported.success);
    REQUIRE(imported.preview != nullptr);
    CHECK(imported.preview->statistics.total_layers == 2);
    CHECK(imported.preview->statistics.render_segment_count > 0);
    CHECK_FALSE(imported.preview->segments.empty());
    for (const auto& segment : imported.preview->segments) {
        if (segment.motion == libslicer::ToolpathMotionKind::Extrusion) {
            CHECK(segment.layer_index == (segment.end_mm.z < 0.3f ? 0 : 1));
        }
    }

    std::filesystem::remove(gcode_path, remove_error);
}

TEST_CASE("external layer tags assign motions without double counting",
          "[libslicer_api][gcode][preview][layer_tags]")
{
    std::string first = "; LAYER:1 [0.2]\r\n";
    std::string second = "; LAYER:2 [0.4]\r\n";
    std::string duplicate = first;
    bool elevated_first_move = false;
    SECTION("FibreSeek numbered layers") {}
    SECTION("whitespace and zero-based layers") {
        first = ";\t LAYER: 0 [ 0.2 ] \t\r\n";
        second = "; LAYER: 1 [0.4]\r\n";
        duplicate = first;
    }
    SECTION("sparse external numbers are contiguous internally") {
        first = ";LAYER:10\n";
        second = ";LAYER:20\n";
        duplicate = first;
    }
    SECTION("numbered tags take precedence when encountered first") {
        first += ";LAYER_CHANGE\n";
        second += "; CHANGE_LAYER\n";
    }
    SECTION("Orca change tags take precedence when encountered first") {
        first = "; CHANGE_LAYER\n;LAYER:1\n";
        second = "; CHANGE_LAYER\n;LAYER:2\n";
        duplicate = ";LAYER:1\n";
    }
    SECTION("compatible change tags remain supported") {
        first = ";LAYER_CHANGE\n";
        second = ";LAYER_CHANGE\n";
        duplicate.clear();
    }
    SECTION("compatible nominal Z is independent of landing height") {
        first = ";LAYER_CHANGE\n;Z:0.2\n";
        second = ";LAYER_CHANGE\n;Z:0.4\n";
        duplicate.clear();
        elevated_first_move = true;
    }
    SECTION("Orca nominal Z is independent of landing height") {
        first = "; CHANGE_LAYER\n; Z_HEIGHT: 0.2\n";
        second = "; CHANGE_LAYER\n; Z_HEIGHT: 0.4\n";
        duplicate.clear();
        elevated_first_move = true;
    }

    const auto path = std::filesystem::temp_directory_path() / "libslicer_layer_tags.gcode";
    {
        std::ofstream gcode(path, std::ios::binary);
        REQUIRE(gcode.good());
        gcode << "; Generated with FibreSeek Rocket Slicer\r\n"
                 "G21\nG90\nM83\nG92 E0\n; LAYER_COUNT:363\n"
                 "; LAYER:invalid\n; LAYER:9999999999999999999999\n"
                 "; LAYER:12garbage\n; LAYER:12 [bad]\n"
              << first
              << (elevated_first_move ? "G0 X0 Y0 Z2 F1200\nG1 X10 E1\n" : "G0 X0 Y0 Z0.2 F1200\nG1 X10 E1\n")
              << duplicate
              << "G0 Z2\nG0 Z0.2\nG1 X20 E1\n"
              << second
              << "G0 Z0.4\nG1 X30 E1\n";
        REQUIRE(gcode.good());
    }
    libslicer::LibraryOptions options;
    options.resource_directory = LIBSLICER_TEST_RESOURCE_DIR;
    options.vendors = {"Flashforge"};
    const auto library = libslicer::Library::open(options);
    REQUIRE(library != nullptr);
    libslicer::GCodePreviewRequest request;
    request.gcode_path = path.string();
    const auto imported = library->load_gcode_preview(request);
    REQUIRE(imported.success);
    REQUIRE(imported.preview != nullptr);
    CHECK(imported.preview->statistics.total_layers == 2);
    REQUIRE(imported.preview->layers.size() == 2);
    CHECK(imported.preview->layers[0].print_z_mm == Catch::Approx(0.2));
    CHECK(imported.preview->layers[1].print_z_mm == Catch::Approx(0.4));
    std::set<std::uint32_t> extrusion_layers;
    for (const auto& segment : imported.preview->segments) {
        if (segment.motion == libslicer::ToolpathMotionKind::Extrusion) {
            CHECK(segment.layer_index == (segment.end_mm.x <= 20.0f ? 0 : 1));
            extrusion_layers.insert(segment.layer_index);
        }
    }
    CHECK(extrusion_layers == std::set<std::uint32_t>{0, 1});
    std::error_code remove_error;
    std::filesystem::remove(path, remove_error);
}

TEST_CASE("FibreSeek file preserves every segment's source layer",
          "[.][libslicer_api][gcode_external]")
{
    const char* path = std::getenv("LIBSLICER_TEST_GCODE_PATH");
    REQUIRE(path != nullptr);
    std::ifstream input(path);
    REQUIRE(input.good());
    std::vector<std::uint32_t> source_layers{0}; // Source command IDs are 1-based lines.
    unsigned int layer_count = 0;
    std::string line;
    while (std::getline(input, line)) {
        if (line.rfind("; LAYER:", 0) == 0) {
            unsigned int source_number = 0;
            std::istringstream number(line.substr(8));
            REQUIRE(bool(number >> source_number));
            REQUIRE(source_number == layer_count + 1);
            ++layer_count;
        }
        source_layers.push_back(layer_count == 0 ? 0 : layer_count - 1);
    }
    REQUIRE(layer_count == 363);
    libslicer::LibraryOptions options;
    options.resource_directory = LIBSLICER_TEST_RESOURCE_DIR;
    options.vendors = {"Flashforge"};
    const auto library = libslicer::Library::open(options);
    REQUIRE(library != nullptr);
    libslicer::GCodePreviewRequest request;
    request.gcode_path = path;
    const auto imported = library->load_gcode_preview(request);
    REQUIRE(imported.success);
    REQUIRE(imported.preview != nullptr);
    REQUIRE(imported.preview->layers.size() == layer_count);
    REQUIRE(imported.preview->statistics.total_layers == layer_count);
    REQUIRE_FALSE(imported.preview->segments.empty());
    std::set<std::uint32_t> populated_layers;
    for (const auto& segment : imported.preview->segments) {
        REQUIRE(segment.source_command_id < source_layers.size());
        CHECK(segment.layer_index == source_layers[segment.source_command_id]);
        populated_layers.insert(segment.layer_index);
    }
    CHECK(populated_layers.size() == layer_count);
}

TEST_CASE("belt G-code preview reconstructs world coordinates across G92 Z reset",
          "[libslicer_api][gcode][preview][belt]")
{
    const auto gcode_path =
        std::filesystem::temp_directory_path() /
        "libslicer_api_belt_world_preview.gcode";
    std::error_code remove_error;
    std::filesystem::remove(gcode_path, remove_error);

    {
        std::ofstream gcode(gcode_path, std::ios::binary);
        REQUIRE(gcode.good());
        gcode << R"(;SLICING_KINEMATICS:BELT
;BELT_COORDINATE_VERSION:1
;BELT_GANTRY_ANGLE:45
;BELT_PLATE_MAX_WORLD_Y:100
G21
G90
M83
G1 Z20 F1200
G92 Z0
;LAYER_CHANGE
; Z_HEIGHT: 10
;TYPE:Outer wall
G1 X0 Y2 Z14.142136 F1200
G1 X1 Y2 E1
)";
        REQUIRE(gcode.good());
    }

    libslicer::LibraryOptions options;
    options.resource_directory = LIBSLICER_TEST_RESOURCE_DIR;
    options.vendors = {"Flashforge"};
    const auto library = libslicer::Library::open(options);
    REQUIRE(library != nullptr);

    libslicer::GCodePreviewRequest request;
    request.gcode_path = gcode_path.string();
    const auto imported = library->load_gcode_preview(request);
    REQUIRE(imported.success);
    REQUIRE(imported.preview != nullptr);

    const auto segment = std::find_if(
        imported.preview->segments.begin(), imported.preview->segments.end(),
        [](const libslicer::ToolpathSegment& candidate) {
            return candidate.motion == libslicer::ToolpathMotionKind::Extrusion;
        });
    REQUIRE(segment != imported.preview->segments.end());
    CHECK(std::abs(segment->start_mm.x - 0.0f) < 0.001f);
    CHECK(std::abs(segment->end_mm.x - 1.0f) < 0.001f);
    CHECK(std::abs(segment->start_mm.y - 87.27208f) < 0.001f);
    CHECK(std::abs(segment->end_mm.y - 87.27208f) < 0.001f);
    CHECK(std::abs(segment->start_mm.z - 1.414214f) < 0.001f);
    CHECK(std::abs(segment->end_mm.z - 1.414214f) < 0.001f);
    CHECK(std::abs(segment->print_z_mm - 10.0f) < 0.001f);

    std::filesystem::remove(gcode_path, remove_error);
}

TEST_CASE("painted model thumbnails preserve filament colors", "[libslicer_api][slice][thumbnail]")
{
    const int width_case = GENERATE(0, 1, 2);
    INFO("width_case=" << width_case);
    libslicer::LibraryOptions options;
    options.resource_directory = LIBSLICER_TEST_RESOURCE_DIR;
    options.vendors = {"Flashforge"};
    auto library = libslicer::Library::open(options);
    REQUIRE(library != nullptr);

    libslicer::ConfigSelection selection;
    selection.machine_model_id = "Flashforge Creator 5";
    selection.machine_variant_id = "0.4";
    const auto activated = library->activate_config(
        selection, {{"filament_colour", "#FF0000FF"},
                    {"outer_wall_line_width", width_case == 0 ? "0.42" : "0"},
                    {"line_width", width_case == 2 ? "0" : "0.42"}});
    REQUIRE(activated.success);
    REQUIRE(library->set_active_filament_color(0, {255, 0, 0, 255}).success);
    REQUIRE(library->set_active_filament_color(1, {0, 255, 0, 255}).success);
    const auto config = library->active_config_snapshot();
    REQUIRE(config.has_value());

    libslicer::SliceVolumeInput volume;
    volume.default_filament_slot = 1;
    volume.vertices = {
        {0.0f, 0.0f, 0.0f}, {20.0f, 0.0f, 0.0f},
        {20.0f, 20.0f, 0.0f}, {0.0f, 20.0f, 0.0f},
        {0.0f, 0.0f, 20.0f}, {20.0f, 0.0f, 20.0f},
        {20.0f, 20.0f, 20.0f}, {0.0f, 20.0f, 20.0f},
    };
    volume.triangles = {
        {0, 2, 1}, {0, 3, 2},
        {4, 5, 6}, {4, 6, 7},
        {0, 1, 5}, {0, 5, 4},
        {1, 2, 6}, {1, 6, 5},
        {2, 3, 7}, {2, 7, 6},
        {3, 0, 4}, {3, 4, 7},
    };
    // A leaf with state 2 is encoded as the little-endian nibble 0b1000.
    // Paint both top triangles with filament slot 2; all other faces retain slot 1.
    volume.facet_labels.roots = {{2, 0}, {3, 4}};
    volume.facet_labels.bitstream = {0, 0, 0, 1, 0, 0, 0, 1};

    libslicer::SliceObjectInput object;
    object.name = "painted-cube";
    object.volumes.push_back(std::move(volume));

    const std::filesystem::path output =
        std::filesystem::temp_directory_path() / "libslicer_api_painted_cube.gcode";
    const std::filesystem::path packaged_output =
        std::filesystem::temp_directory_path() / "libslicer_api_painted_cube.gcode.3mf";
    std::error_code remove_error;
    std::filesystem::remove(output, remove_error);
    std::filesystem::remove(packaged_output, remove_error);

    libslicer::SliceRequest request;
    request.config = *config;
    request.objects.push_back(std::move(object));
    request.output_gcode_path = output.string();
    request.output_gcode_3mf_path = packaged_output.string();
    const auto sliced = library->slice(request);
    const std::string diagnostic = sliced.diagnostics.empty()
        ? std::string{}
        : sliced.diagnostics.front().message;
    INFO(diagnostic);
    REQUIRE(sliced.success);

    const auto thumbnail_bytes = read_zip_entry(
        packaged_output, "Metadata/plate_1.png");
    const auto thumbnail = decode_png(thumbnail_bytes);
    REQUIRE(thumbnail.has_value());
    REQUIRE(thumbnail->rgba.size() ==
            static_cast<std::size_t>(thumbnail->width) * thumbnail->height * 4);

    std::size_t red_pixels = 0;
    std::size_t green_pixels = 0;
    for (std::size_t pixel = 0; pixel < thumbnail->rgba.size(); pixel += 4) {
        const unsigned int red = thumbnail->rgba[pixel];
        const unsigned int green = thumbnail->rgba[pixel + 1];
        const unsigned int blue = thumbnail->rgba[pixel + 2];
        if (red > 48 && red > green + 32 && red > blue + 32) {
            ++red_pixels;
        }
        if (green > 48 && green > red + 32 && green > blue + 32) {
            ++green_pixels;
        }
    }
    CHECK(red_pixels > 100);
    CHECK(green_pixels > 100);

    std::filesystem::remove(output, remove_error);
    std::filesystem::remove(packaged_output, remove_error);
}

TEST_CASE("inactive fiber infill settings do not prevent contour slicing", "[libslicer_api][cleanup]")
{
    libslicer::LibraryOptions options;
    options.resource_directory = LIBSLICER_TEST_RESOURCE_DIR;
    options.vendors = {"CFSYS"};
    auto library = libslicer::Library::open(options);
    REQUIRE(library);
    libslicer::ConfigSelection selection;
    selection.machine_model_id = "CFSYS Alpha500 Printer";
    selection.machine_variant_id = "0.4";
    selection.process_preset_id = "CCF&CIRON @CFSYS";
    selection.filament_preset_ids = {"CFSYS CIRON", "CFSYS CCF"};
    selection.filament_physical_tools = {0, 1};
    REQUIRE(library->activate_config(selection, {
        {"generate_reinforced_infills", "0"},
        {"fiber_infill_max_speed", "0"},
        {"fiber_resin_overlap", "0.05"}}).success);
    libslicer::SliceRequest request;
    request.config = *library->active_config_snapshot();
    request.objects.push_back(fiber_infill_block());
    const auto result = library->slice(request);
    for (const auto& diagnostic : result.diagnostics) INFO(diagnostic.message);
    REQUIRE(result.success);
    std::ifstream input(result.output.path);
    const std::string gcode((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    CHECK(gcode.find(";FIBER_BEGIN") != std::string::npos);
    CHECK(gcode.find("purpose=contour") != std::string::npos);
    CHECK(gcode.find("fiber_minimum_segment_length =") == std::string::npos);
    CHECK(gcode.find("fiber_maximum_turn_angle =") == std::string::npos);
    std::filesystem::remove(result.output.path);
    if (!result.gcode_3mf.path.empty()) std::filesystem::remove(result.gcode_3mf.path);
}

TEST_CASE("fiber rounding uses preset radii and preserves independent overrides", "[libslicer_api][fiber-rounding]")
{
    libslicer::LibraryOptions options;
    options.resource_directory=LIBSLICER_TEST_RESOURCE_DIR;
    options.vendors={"CFSYS"};
    auto library=libslicer::Library::open(options);
    libslicer::ConfigSelection selection;
    selection.machine_model_id="CFSYS Alpha500 Printer";
    selection.machine_variant_id="0.4";
    selection.process_preset_id="CCF&CIRON @CFSYS";
    selection.filament_preset_ids={"CFSYS CIRON","CFSYS CCF"};
    selection.filament_physical_tools={0,1};
    std::ifstream preset_file(std::filesystem::path(LIBSLICER_TEST_RESOURCE_DIR)
        / "profiles" / "CFSYS" / "process" / (selection.process_preset_id + ".json"));
    REQUIRE(preset_file.good());
    const auto preset=nlohmann::json::parse(preset_file);
    const auto contour_radius=preset.at("fiber_contour_bend_radius").get<std::string>();
    const auto infill_radius=preset.at("fiber_infill_bend_radius").get<std::string>();
    const std::string contour_override=contour_radius=="0.7"?"0.8":"0.7";
    const std::string infill_override=infill_radius=="0"?"1":"0";
    REQUIRE(library->activate_config(selection,{}).success);
    CHECK(library->active_config_snapshot()->value("fiber_contour_bend_radius")==std::optional<std::string>{contour_radius});
    CHECK(library->active_config_snapshot()->value("fiber_infill_bend_radius")==std::optional<std::string>{infill_radius});
    const auto patch=library->apply_active_config_patch({{"fiber_contour_bend_radius",contour_override}});
    for (const auto& diagnostic:patch.diagnostics) UNSCOPED_INFO(diagnostic.key << ": " << diagnostic.message);
    REQUIRE(patch.success);
    const auto snapshot=library->active_config_snapshot();
    CHECK(snapshot->value("fiber_contour_bend_radius")==std::optional<std::string>{contour_override});
    CHECK_FALSE(snapshot->value("fiber_contour_rounding_max_reserve").has_value());
    CHECK(snapshot->value("fiber_infill_bend_radius")==std::optional<std::string>{infill_radius});
    REQUIRE(library->apply_active_config_patch({{"fiber_infill_bend_radius",infill_override}}).success);
    CHECK(library->active_config_snapshot()->value("fiber_infill_bend_radius")==std::optional<std::string>{infill_override});
    CHECK(library->active_config_snapshot()->value("fiber_contour_bend_radius")==std::optional<std::string>{contour_override});
}

TEST_CASE("fiber corner stabilization setting round trips independently", "[libslicer_api][config][fiber-corner-stability]")
{
    auto config=libslicer::Config::defaults();
    const auto items=config.settings();
    const auto* item=find_item(items,"fiber_corner_stabilization_length");
    REQUIRE(item!=nullptr);
    CHECK(item->group==libslicer::SettingGroup::Process);
    CHECK(item->category=="Continuous fiber");
    CHECK(config.snapshot().value("fiber_corner_stabilization_length")=="5");
    REQUIRE(config.set("fiber_corner_stabilization_length","3").success);
    CHECK(config.snapshot().value("fiber_corner_stabilization_length")=="3");
    REQUIRE(config.reset("fiber_corner_stabilization_length").success);
    CHECK(config.snapshot().value("fiber_corner_stabilization_length")=="5");
}

TEST_CASE("fiber infill stability settings belong to their selected pattern", "[libslicer_api][config][fiber-corner-stability]")
{
    auto config=libslicer::Config::defaults();
    const char* rectilinear="fiber_corner_stabilization_length";
    const char* concentric="fiber_concentric_corner_stabilization_length";
    auto items=config.settings();
    const auto* item=find_item(items,concentric);
    REQUIRE(item!=nullptr);
    CHECK(item->group==libslicer::SettingGroup::Process);
    CHECK(item->category=="Continuous fiber");
    CHECK(item->unit=="mm");
    CHECK(config.snapshot().value(concentric)=="0");
    CHECK(config.snapshot().value(rectilinear)=="5");
    CHECK_FALSE(item->visible);CHECK_FALSE(item->enabled);
    CHECK_FALSE(find_item(items,rectilinear)->visible);
    REQUIRE(config.set("generate_reinforced_infills","1").success);
    REQUIRE(config.set("reinforced_infill_pattern","rectilinear").success);
    items=config.settings();
    CHECK(find_item(items,rectilinear)->visible);CHECK(find_item(items,rectilinear)->enabled);
    CHECK_FALSE(find_item(items,concentric)->visible);CHECK_FALSE(find_item(items,concentric)->enabled);
    REQUIRE(config.set(rectilinear,"3").success);
    REQUIRE(config.set(concentric,"5").success);
    const auto switched=config.set("reinforced_infill_pattern","concentric");
    REQUIRE(switched.success);
    REQUIRE(find_item(switched.changed_items,rectilinear)!=nullptr);
    REQUIRE(find_item(switched.changed_items,concentric)!=nullptr);
    items=config.settings();
    CHECK_FALSE(find_item(items,rectilinear)->visible);CHECK_FALSE(find_item(items,rectilinear)->enabled);
    CHECK(find_item(items,concentric)->visible);CHECK(find_item(items,concentric)->enabled);
    CHECK(config.snapshot().value(concentric)=="5");CHECK(config.snapshot().value(rectilinear)=="3");
    for(const char* invalid:{"-1","nan","inf"}) {
        CHECK_FALSE(config.set(concentric,invalid).success);
        CHECK(config.snapshot().value(concentric)=="5");
    }
    REQUIRE(config.reset(concentric).success);
    CHECK(config.snapshot().value(concentric)=="0");CHECK(config.snapshot().value(rectilinear)=="3");
    REQUIRE(config.set("generate_reinforced_infills","0").success);
    items=config.settings();
    CHECK_FALSE(find_item(items,concentric)->visible);CHECK_FALSE(find_item(items,rectilinear)->visible);
}

TEST_CASE("fiber infill angle sequence is an independently validated process setting", "[libslicer_api][config][fiber-angles]")
{
    auto config=libslicer::Config::defaults();
    auto items=config.settings();
    const auto* item=find_item(items,"fiber_infill_angle_sequence");
    REQUIRE(item!=nullptr);
    CHECK(item->group==libslicer::SettingGroup::Process);
    CHECK(item->category=="Continuous fiber");
    CHECK(item->type==libslicer::SettingType::String);
    CHECK_FALSE(item->visible);
    CHECK_FALSE(item->enabled);
    CHECK(config.snapshot().value("fiber_infill_angle_sequence")=="45,135");
    const auto enabled_result=config.set("generate_reinforced_infills","1");
    REQUIRE(enabled_result.success);
    REQUIRE(find_item(enabled_result.changed_items,"fiber_infill_angle_sequence")!=nullptr);
    CHECK(find_item(enabled_result.changed_items,"fiber_infill_angle_sequence")->visible);
    items=config.settings();
    REQUIRE(find_item(items,"fiber_infill_angle_sequence")!=nullptr);
    CHECK(find_item(items,"fiber_infill_angle_sequence")->visible);
    CHECK(find_item(items,"fiber_infill_angle_sequence")->enabled);
    REQUIRE(config.set("fiber_infill_angle_sequence","0,45,90,135").success);
    CHECK(config.snapshot().value("fiber_infill_angle_sequence")=="0,45,90,135");
    for (const char* invalid:{"","0,","0,,90","45deg","nan","360"}) {
        const auto result=config.set("fiber_infill_angle_sequence",invalid);
        CHECK_FALSE(result.success);
        CHECK(config.snapshot().value("fiber_infill_angle_sequence")=="0,45,90,135");
    }
    const auto concentric_result=config.set("reinforced_infill_pattern","concentric");
    REQUIRE(concentric_result.success);
    REQUIRE(find_item(concentric_result.changed_items,"fiber_infill_angle_sequence")!=nullptr);
    CHECK_FALSE(find_item(concentric_result.changed_items,"fiber_infill_angle_sequence")->visible);
    items=config.settings();
    CHECK_FALSE(find_item(items,"fiber_infill_angle_sequence")->visible);
    CHECK_FALSE(find_item(items,"fiber_infill_angle_sequence")->enabled);
    REQUIRE(config.reset("fiber_infill_angle_sequence").success);
    CHECK(config.snapshot().value("fiber_infill_angle_sequence")=="45,135");
}

TEST_CASE("fiber infill angle sequence controls deposited G-code on successive fiber layers", "[libslicer_api][fiber-angles][slice]")
{
    const int interval=GENERATE(1,2);
    const bool use_default=GENERATE(false,true);
    libslicer::LibraryOptions options;
    options.resource_directory=LIBSLICER_TEST_RESOURCE_DIR;
    options.vendors={"CFSYS"};
    auto library=libslicer::Library::open(options);
    libslicer::ConfigSelection selection;
    selection.machine_model_id="CFSYS Alpha500 Printer";
    selection.machine_variant_id="0.4";
    selection.process_preset_id="CCF&CIRON @CFSYS";
    selection.filament_preset_ids={"CFSYS CIRON","CFSYS CCF"};
    selection.filament_physical_tools={0,1};
    std::vector<std::pair<std::string,std::string>> overrides{
        {"generate_reinforced_perimeters","0"},
        {"generate_reinforced_infills","1"},
        {"reinforced_infill_pattern","rectilinear"},
        {"reinforced_infill_density","100%"},
        {"sparse_infill_density","100%"},
        {"top_shell_layers","0"}, {"bottom_shell_layers","0"},
        {"fiber_layer_height_ratio",std::to_string(interval)}};
    if (!use_default)
        overrides.emplace_back("fiber_infill_angle_sequence","0,45,90,135");
    REQUIRE(library->activate_config(selection,overrides).success);
    auto model=fiber_infill_block();
    for (auto& vertex:model.volumes.front().vertices) if (vertex.z>0)
        vertex.z=interval==1?1.3f:2.4f;
    libslicer::SliceRequest request;
    request.config=*library->active_config_snapshot();
    request.objects.push_back(std::move(model));
    const auto sliced=library->slice(request);
    for (const auto& diagnostic:sliced.diagnostics) INFO(diagnostic.message);
    REQUIRE(sliced.success);
    const auto imported=library->load_gcode_preview({sliced.output.path});
    REQUIRE(imported.success);
    REQUIRE(imported.preview);
    constexpr double pi=3.14159265358979323846;
    const std::vector<double> expected=use_default?
        std::vector<double>{45,135}:std::vector<double>{0,45,90,135};
    std::set<unsigned> observed_layers;
    std::set<size_t> observed_angles;
    for (const auto& segment:imported.preview->segments) {
        if (segment.extrusion_role!=libslicer::ToolpathExtrusionRole::ContinuousFiberInfill ||
            segment.deposition!=libslicer::ToolpathDepositionKind::ContinuousFiberPowered) continue;
        const double dx=segment.end_mm.x-segment.start_mm.x;
        const double dy=segment.end_mm.y-segment.start_mm.y;
        if (std::hypot(dx,dy)<1.0) continue; // Short segments may be rounded returns.
        const size_t fiber_layer=segment.layer_index/size_t(interval);
        const size_t slot=fiber_layer%expected.size();
        double actual=std::fmod(std::atan2(dy,dx)*180/pi+360,180);
        const double difference=std::abs(actual-expected[slot]);
        INFO("interval=" << interval << " layer=" << segment.layer_index << " angle=" << actual);
        CHECK(std::min(difference,180-difference)<0.5);
        observed_layers.insert(segment.layer_index);
        observed_angles.insert(slot);
    }
    CHECK(observed_layers.size()>=8); // Two complete angle cycles in final G-code.
    CHECK(observed_angles.size()==expected.size());
    std::error_code error;
    std::filesystem::remove(sliced.output.path,error);
    if (!sliced.gcode_3mf.path.empty()) std::filesystem::remove(sliced.gcode_3mf.path,error);
}


TEST_CASE("hole contour setting retains default and round trips through configuration", "[libslicer_api][config][fiber][hole-contours]")
{
    auto config = libslicer::Config::defaults();
    auto items = config.settings();
    const auto* item = find_item(items, "fiber_contour_include_holes");
    REQUIRE(item != nullptr);
    CHECK(item->type == libslicer::SettingType::Boolean);
    CHECK(item->group == libslicer::SettingGroup::Process);
    CHECK(item->category == "Continuous fiber");
    CHECK(item->default_value == "0");
    CHECK(config.snapshot().value("fiber_contour_boundary_clearance") == "0.05");
    CHECK_FALSE(item->enabled);
    REQUIRE(config.set("generate_reinforced_perimeters", "1").success);
    items = config.settings();
    CHECK(find_item(items, "fiber_contour_include_holes")->enabled);
    REQUIRE(config.set("fiber_contour_include_holes", "1").success);
    const auto saved = config.snapshot().value("fiber_contour_include_holes");
    REQUIRE(saved == "1");
    auto restored = libslicer::Config::defaults();
    REQUIRE(restored.set("fiber_contour_include_holes", *saved).success);
    CHECK(restored.snapshot().value("fiber_contour_include_holes") == "1");
    REQUIRE(config.reset("fiber_contour_include_holes").success);
    CHECK(config.snapshot().value("fiber_contour_include_holes") == "0");
}


TEST_CASE("fiber outer contours do not open a printable bridge next to a hole", "[libslicer_api][fiber][hole-contours]")
{
    const auto radius = GENERATE("0", "0.3");
    CAPTURE(radius);
    libslicer::LibraryOptions options;
    options.resource_directory = LIBSLICER_TEST_RESOURCE_DIR;
    options.vendors = {"CFSYS"};
    auto library = libslicer::Library::open(options);
    libslicer::ConfigSelection selection;
    selection.machine_model_id = "CFSYS Alpha500 Printer";
    selection.machine_variant_id = "0.4";
    selection.process_preset_id = "CCF&CIRON @CFSYS";
    selection.filament_preset_ids = {"CFSYS CIRON", "CFSYS CCF"};
    selection.filament_physical_tools = {0, 1};
    REQUIRE(library->activate_config(selection, {
        {"fiber_contour_include_holes", "0"}, {"fiber_contour_bend_radius", radius},
        {"fiber_fill_debug", "1"}, {"generate_reinforced_infills", "0"},
        {"outer_reinforced_perimeters_counts", "1"}, {"wall_loops", "3"},
        {"top_shell_layers", "0"}, {"bottom_shell_layers", "0"}
    }).success);
    auto object = fiber_debug_ring();
    // After resin walls this bridge fits a fiber centerline, but the removed
    // extra erosion/dilation opened the hole and replaced the outer contour.
    for (auto& vertex : object.volumes.front().vertices)
        if (vertex.x == 120) vertex.x = 104.6f;
    libslicer::SliceRequest request;
    request.config = *library->active_config_snapshot();
    request.objects.push_back(std::move(object));
    const auto result = library->slice(request);
    for (const auto& d : result.diagnostics) UNSCOPED_INFO(d.message);
    REQUIRE(result.success);
    REQUIRE(result.preview);
    REQUIRE(std::any_of(result.preview->fiber_fill_diagnostics.begin(), result.preview->fiber_fill_diagnostics.end(),
        [](const auto& d) { return d.layer_index == 4 && d.kind == libslicer::FiberDiagnosticKind::OriginalContourRegion && d.boundaries.size() == 2; }));
    std::set<std::uint64_t> occurrences;
    for (const auto& segment : result.preview->segments) {
        if (segment.layer_index != 4 || segment.extrusion_role != libslicer::ToolpathExtrusionRole::ContinuousFiberContour ||
            segment.deposition == libslicer::ToolpathDepositionKind::None) continue;
        occurrences.insert(segment.fiber_occurrence);
        for (const auto& p : {segment.start_mm, segment.end_mm}) {
            // The outer rectangle lies outside this inset box. Following the
            // inner hole after a topology-changing opening would enter it.
            CHECK_FALSE((p.x > 104 && p.x < 156 && p.y > 104 && p.y < 136));
        }
    }
    CHECK(occurrences.size() == 1);
    std::error_code error;
    std::filesystem::remove(result.output.path, error);
}

TEST_CASE("resin infill settings expose independent values and validate unsupported patterns", "[libslicer_api][fiber][resin-fill]")
{
    auto config = libslicer::Config::defaults();
    auto items = config.settings();
    CHECK(find_item(items, "fiber_resin_fill_mode") == nullptr);
    for (const auto& item : items)
        if (item.key.rfind("fiber_resin_fill_", 0) == 0) CHECK_FALSE(item.enabled);
    REQUIRE(config.set("generate_reinforced_perimeters", "1").success);
    for (const auto& item : config.settings())
        if (item.key.rfind("fiber_resin_fill_", 0) == 0) CHECK(item.enabled);
    REQUIRE(config.set("generate_reinforced_perimeters", "0").success);
    REQUIRE(config.set("generate_reinforced_infills", "1").success);
    for (const auto& item : config.settings())
        if (item.key.rfind("fiber_resin_fill_", 0) == 0) CHECK(item.enabled);
    const auto ordinary = config.snapshot().value("sparse_infill_density");
    REQUIRE(config.set("fiber_resin_fill_density", "60%").success);
    CHECK(config.snapshot().value("sparse_infill_density") == ordinary);
    REQUIRE(config.set("fiber_resin_fill_pattern", "lightning").success);
    const auto errors = config.validate();
    CHECK(std::any_of(errors.begin(), errors.end(), [](const auto& error) {
        return error.message == "Unsupported independent resin infill pattern";
    }));
    REQUIRE(config.reset("fiber_resin_fill_pattern").success);
    CHECK_FALSE(config.set("fiber_resin_fill_density", "101%").success);
    const auto snapshot = config.snapshot();
    REQUIRE(config.set("generate_reinforced_infills", "0").success);
    for (const auto& item : config.settings())
        if (item.key.rfind("fiber_resin_fill_", 0) == 0) CHECK_FALSE(item.enabled);
    CHECK(snapshot.value("fiber_resin_fill_density") == "60%");
    CHECK(config.snapshot().value("fiber_resin_fill_density") == "60%");
}

TEST_CASE("independent resin infill preserves fiber and ordinary layers", "[libslicer_api][fiber][resin-fill][slice]")
{
    using Role = libslicer::ToolpathExtrusionRole;
    libslicer::LibraryOptions options;
    options.resource_directory = LIBSLICER_TEST_RESOURCE_DIR;
    options.vendors = {"CFSYS"};
    auto library = libslicer::Library::open(options);
    libslicer::ConfigSelection selection;
    selection.machine_model_id = "CFSYS Alpha500 Printer";
    selection.machine_variant_id = "0.4";
    selection.process_preset_id = "CCF&CIRON @CFSYS";
    selection.filament_preset_ids = {"CFSYS CIRON", "CFSYS CCF"};
    selection.filament_physical_tools = {0, 1};
    REQUIRE(library->activate_config(selection, {
        {"layer_height", "0.2"}, {"initial_layer_print_height", "0.2"},
        {"top_shell_layers", "2"}, {"bottom_shell_layers", "2"},
        {"generate_reinforced_perimeters", "1"}, {"generate_reinforced_infills", "0"},
        {"fiber_contour_bend_radius", "0"}, {"fiber_layer_height_ratio", "2"},
        {"sparse_infill_density", "15%"}, {"sparse_infill_pattern", "grid"},
        {"fiber_resin_fill_pattern", "rectilinear"},
        {"fiber_resin_fill_density", "30%"}, {"fiber_resin_fill_line_width", "0.5"},
        {"fiber_resin_fill_speed", "17"}, {"fiber_resin_fill_acceleration", "123"},
        {"enable_support", "0"}, {"slow_down_for_layer_cooling", "0,0"}}).success);
    auto model = fiber_infill_block();
    for (auto& vertex : model.volumes.front().vertices) vertex.z *= .25f;
    const auto slice = [&]() {
        libslicer::SliceRequest request;
        request.objects = {model};
        request.config = *library->active_config_snapshot();
        auto result = library->slice(request);
        for (const auto& d : result.diagnostics) INFO(d.message);
        REQUIRE(result.success); REQUIRE(result.preview);
        return result;
    };
    const auto segments = [](const libslicer::ToolpathPreview& preview, Role role) {
        std::vector<std::tuple<unsigned,float,float,float,float,float,float>> result;
        for (const auto& s : preview.segments)
            if (s.extrusion_role == role && s.deposition != libslicer::ToolpathDepositionKind::None)
                result.emplace_back(s.layer_index, s.start_mm.x, s.start_mm.y, s.start_mm.z,
                    s.end_mm.x, s.end_mm.y, s.end_mm.z);
        std::sort(result.begin(),result.end());
        return result;
    };
    const auto reference_config = *library->active_config_snapshot();
    const auto low = slice();
    const auto fiber = segments(*low.preview,Role::ContinuousFiberContour);
    REQUIRE_FALSE(fiber.empty());
    const auto ordinary = segments(*low.preview,Role::SparseInfill);
    REQUIRE_FALSE(ordinary.empty());
    size_t resin_count = 0;
    for (const auto& s : low.preview->segments) if (s.extrusion_role == Role::ResinInfill) {
        ++resin_count;
        CHECK(s.layer_index % 2 == 0);
        CHECK(s.deposition == libslicer::ToolpathDepositionKind::Thermoplastic);
        CHECK(s.tool_id == 0);
        CHECK(s.width_mm == Catch::Approx(.5).margin(.001));
        CHECK(s.nominal_speed_mm_s <= 17.001);
        CHECK(s.acceleration_mm_s2 == Catch::Approx(123).margin(.1));
    }
    REQUIRE(resin_count > 0);
    const auto imported = library->load_gcode_preview({low.output.path});
    REQUIRE(imported.success); REQUIRE(imported.preview);
    CHECK(segments(*imported.preview,Role::ResinInfill) == segments(*low.preview,Role::ResinInfill));
    // Ordinary sparse controls must not leak into resin jobs on fiber layers.
    REQUIRE(library->apply_active_config_patch({{"sparse_infill_density", "40%"},
        {"sparse_infill_pattern", "triangles"}, {"infill_direction", "70"},
        {"sparse_infill_rotate_template", "10,30"}, {"align_infill_direction_to_model", "1"},
        {"sparse_infill_line_width", "0.65"}, {"fill_multiline", "2"},
        {"infill_anchor", "0"}, {"infill_anchor_max", "0"},
        {"sparse_infill_speed", "80"}, {"sparse_infill_acceleration", "456"}}).success);
    const auto changed_sparse = slice();
    CHECK(segments(*changed_sparse.preview,Role::ResinInfill) == segments(*low.preview,Role::ResinInfill));
    CHECK(segments(*changed_sparse.preview,Role::SparseInfill) != ordinary);
    for (const auto& segment : changed_sparse.preview->segments) if (segment.extrusion_role == Role::ResinInfill) {
        CHECK(segment.width_mm == Catch::Approx(.5).margin(.001));
        CHECK(segment.nominal_speed_mm_s <= 17.001);
        CHECK(segment.acceleration_mm_s2 == Catch::Approx(123).margin(.1));
    }
    std::vector<std::pair<std::string, std::string>> restore_sparse;
    for (const auto* key : {"sparse_infill_density", "sparse_infill_pattern", "infill_direction",
        "sparse_infill_rotate_template", "align_infill_direction_to_model", "sparse_infill_line_width",
        "fill_multiline", "infill_anchor", "infill_anchor_max", "sparse_infill_speed", "sparse_infill_acceleration"})
        restore_sparse.emplace_back(key, *reference_config.value(key));
    REQUIRE(library->apply_active_config_patch(restore_sparse).success);
    REQUIRE(library->apply_active_config_patch({{"fiber_resin_fill_density", "60%"}}).success);
    const auto high = slice();
    CHECK(segments(*high.preview,Role::ContinuousFiberContour) == fiber);
    CHECK(segments(*high.preview,Role::SparseInfill) == ordinary);
    CHECK(segments(*high.preview,Role::TopSurface) == segments(*low.preview,Role::TopSurface));
    CHECK(segments(*high.preview,Role::ResinInfill).size() > segments(*low.preview,Role::ResinInfill).size());
    REQUIRE(library->apply_active_config_patch({{"fiber_resin_fill_density", "0%"}}).success);
    const auto off = slice();
    CHECK(segments(*off.preview,Role::ResinInfill).empty());
    CHECK(segments(*off.preview,Role::ContinuousFiberContour) == fiber);
    CHECK(segments(*off.preview,Role::SparseInfill) == ordinary);
    REQUIRE(library->apply_active_config_patch({{"sparse_infill_density", "0%"}, {"fiber_resin_fill_density", "30%"}}).success);
    const auto no_sparse = slice();
    CHECK_FALSE(segments(*no_sparse.preview,Role::ResinInfill).empty());
    CHECK_FALSE(segments(*no_sparse.preview,Role::ContinuousFiberContour).empty());
    CHECK(segments(*no_sparse.preview,Role::SparseInfill).empty());
    // Geometrically rejected fiber still leaves an explicitly configured resin job.
    REQUIRE(library->apply_active_config_patch({{"fiber_minimum_path_length", "10000"}}).success);
    const auto rejected = slice();
    CHECK(segments(*rejected.preview,Role::ContinuousFiberContour).empty());
    CHECK_FALSE(segments(*rejected.preview,Role::ResinInfill).empty());
    // Every exposed pattern must work with both sparse and full resin density.
    REQUIRE(library->apply_active_config_patch({{"fiber_minimum_path_length", "0"},
        {"sparse_infill_density", "100%"}}).success);
    for (const auto* pattern : {"rectilinear", "grid", "triangles", "cubic", "gyroid", "concentric"}) {
        for (const auto* density : {"30%", "100%"}) {
            INFO(pattern); INFO(density);
            REQUIRE(library->apply_active_config_patch({{"fiber_resin_fill_pattern", pattern},
                {"fiber_resin_fill_density", density}}).success);
            const auto patterned = slice();
            CHECK_FALSE(segments(*patterned.preview,Role::ResinInfill).empty());
            CHECK_FALSE(segments(*patterned.preview,Role::ContinuousFiberContour).empty());
            std::filesystem::remove(patterned.output.path);
        }
    }
    REQUIRE(library->apply_active_config_patch({{"sparse_infill_density", "15%"},
        {"generate_reinforced_perimeters", "0"}, {"fiber_resin_fill_density", "60%"}}).success);
    const auto disabled = slice();
    CHECK(segments(*disabled.preview,Role::ResinInfill).empty());
    CHECK(segments(*disabled.preview,Role::ContinuousFiberContour).empty());
    CHECK_FALSE(segments(*disabled.preview,Role::SparseInfill).empty());
    for (const auto* result : {&low,&high,&off,&no_sparse,&rejected,&changed_sparse,&disabled})
        std::filesystem::remove(result->output.path);
}

TEST_CASE("concentric shares contour settings while preserving inactive infill values", "[libslicer_api][config][shared-ring]")
{
    auto config=libslicer::Config::defaults();
    REQUIRE(config.apply_patch({{"generate_reinforced_perimeters","0"},{"generate_reinforced_infills","1"},
        {"reinforced_infill_pattern","concentric"},{"fiber_infill_feed_ratio","1.2"}}).success);
    const auto items=config.settings();
    for(const auto* key:{"reinforced_perimeters_filament","fiber_contour_bend_radius","fiber_contour_boundary_clearance",
        "fiber_contour_feed_ratio","fiber_contour_include_holes"}) {
        INFO(key);REQUIRE(find_item(items,key));CHECK(find_item(items,key)->enabled);
    }
    for(const auto* key:{"reinforced_infill_filament","fiber_infill_bend_radius","fiber_infill_feed_ratio","fiber_infill_min_speed",
        "fiber_contour_infill_clearance","outer_reinforced_perimeters_counts"}) {
        INFO(key);REQUIRE(find_item(items,key));CHECK_FALSE(find_item(items,key)->enabled);
    }
    CHECK(config.snapshot().value("fiber_infill_feed_ratio")=="1.2");
    REQUIRE(config.set("reinforced_infill_pattern","rectilinear").success);
    const auto restored=config.settings();CHECK(find_item(restored,"fiber_infill_feed_ratio")->enabled);
    CHECK(config.snapshot().value("fiber_infill_feed_ratio")=="1.2");
}


namespace {
std::unique_ptr<libslicer::Library> mask_library()
{
    libslicer::LibraryOptions options;
    options.resource_directory = LIBSLICER_TEST_RESOURCE_DIR;
    options.vendors = {"CFSYS"};
    auto library = libslicer::Library::open(options);
    REQUIRE(library);
    libslicer::ConfigSelection selection;
    selection.machine_model_id = "CFSYS Alpha500 Printer";
    selection.machine_variant_id = "0.4";
    selection.process_preset_id = "CCF&CIRON @CFSYS";
    selection.filament_preset_ids = {"CFSYS CIRON", "CFSYS CCF"};
    selection.filament_physical_tools = {0,1};
    const auto activated = library->activate_config(selection, {
        {"generate_reinforced_perimeters", "0"}, {"generate_reinforced_infills", "0"},
        {"enable_prime_tower", "0"},
        {"sparse_infill_density", "0%"}, {"wall_loops", "2"},
        {"top_shell_layers", "1"}, {"bottom_shell_layers", "1"},
        {"layer_height", "0.2"}, {"initial_layer_print_height", "0.2"},
        {"infill_combination", "0"}, {"interface_shells", "0"},
        {"fiber_minimum_path_length", "1"}, {"fiber_minimum_effective_length", "0"},
        {"fiber_cut_to_contact_length", "2"}, {"fiber_landing_length", "0"},
        {"fiber_width", "0.8"},
        {"outer_reinforced_perimeters_counts", "1"},
        {"fiber_contour_bend_radius", "0"}, {"fiber_infill_bend_radius", "0"},
        {"fiber_finish_extension_length", "0"}, {"fiber_finish_overlap_length", "0"},
        {"fiber_resin_fill_density", "0%"}});
    std::ostringstream activation_diagnostics;
    for (const auto& issue : activated.diagnostics) activation_diagnostics << issue.key << ": " << issue.message << "\n";
    INFO(activation_diagnostics.str());
    REQUIRE(activated.success);
    return library;
}
libslicer::SliceFiberMaskInput box_mask(std::string id = "central")
{
    libslicer::SliceFiberMaskInput mask;
    mask.id = std::move(id);
    mask.dimensions_mm = {28, 24, 2};
    mask.mask_to_object[3] = 130;
    mask.mask_to_object[7] = 120;
    mask.mask_to_object[11] = 1;
    return mask;
}
libslicer::SliceRequest mask_request(const libslicer::Library& library)
{
    libslicer::SliceRequest request;
    request.config = *library.active_config_snapshot();
    request.center_on_build_plate = false;
    request.objects = {fiber_infill_block()};
    for (auto& vertex : request.objects[0].volumes[0].vertices)
        vertex.z *= 0.25; // Ten layers; explicit coordinates also test non-zero object origin.
    return request;
}
void require_slice(const libslicer::SliceResult& result)
{
    std::ostringstream diagnostics;
    for (const auto& issue : result.diagnostics) diagnostics << issue.message << "\n";
    INFO(diagnostics.str());
    REQUIRE(result.success);
    REQUIRE(result.preview);
}
bool depositing_fiber(const libslicer::ToolpathSegment& segment)
{
    return segment.deposition == libslicer::ToolpathDepositionKind::ContinuousFiberPowered ||
           segment.deposition == libslicer::ToolpathDepositionKind::ContinuousFiberPassive;
}
void remove_slice(const libslicer::SliceResult& result)
{
    std::error_code error;
    if (!result.output.path.empty()) std::filesystem::remove(result.output.path, error);
}
using MaskDepositionSignature = std::vector<std::array<double, 20>>;
MaskDepositionSignature mask_deposition_signature(const libslicer::ToolpathPreview& preview,
                                                 double minimum_x = -std::numeric_limits<double>::infinity(),
                                                 bool fiber_only = false)
{
    MaskDepositionSignature result;
    for (const auto& segment : preview.segments)
        if ((segment.deposition == libslicer::ToolpathDepositionKind::Thermoplastic || depositing_fiber(segment)) &&
            (!fiber_only || depositing_fiber(segment)) &&
            segment.start_mm.x >= minimum_x && segment.end_mm.x >= minimum_x)
            result.push_back({segment.start_mm.x, segment.start_mm.y, segment.start_mm.z,
                              segment.end_mm.x, segment.end_mm.y, segment.end_mm.z,
                              double(segment.extrusion_role), double(segment.deposition), double(segment.fiber_phase),
                              double(segment.tool_id), double(segment.filament_id), double(segment.motion),
                              segment.extrusion_delta_mm, segment.fiber_feed_delta_mm, segment.nominal_speed_mm_s,
                              segment.width_mm, segment.height_mm, segment.mm3_per_mm,
                              segment.print_z_mm, segment.deposited_path_length_mm});
    // Tool ordering may change, but each component's deposited geometry and
    // process must stay the same. Runtime IDs and travel timing are not a baseline.
    std::sort(result.begin(), result.end());
    return result;
}
void check_mask_deposition(const MaskDepositionSignature& actual, const MaskDepositionSignature& expected)
{
    REQUIRE(actual.size() == expected.size());
    for (std::size_t i = 0; i < actual.size(); ++i)
        for (std::size_t field = 0; field < actual[i].size(); ++field) {
            INFO("segment=" << i << " field=" << field);
            CHECK(actual[i][field] == Catch::Approx(expected[i][field]).margin(1e-5));
        }
}
using MaskFiberGroup = std::pair<std::array<double, 12>, std::vector<const libslicer::ToolpathSegment*>>;
std::vector<MaskFiberGroup> distant_fiber_groups(const libslicer::ToolpathPreview& preview, double minimum_x = 180)
{
    std::map<std::pair<std::array<double, 12>, std::uint64_t>, std::vector<const libslicer::ToolpathSegment*>> groups;
    for (const auto& segment : preview.segments) {
        if (!depositing_fiber(segment) || segment.start_mm.x < minimum_x || segment.end_mm.x < minimum_x) continue;
        const std::array<double, 12> process = {
            double(segment.layer_index), double(segment.extrusion_role), double(segment.deposition),
            double(segment.fiber_phase), double(segment.tool_id), double(segment.filament_id), double(segment.motion),
            segment.nominal_speed_mm_s, segment.width_mm, segment.height_mm, segment.mm3_per_mm, segment.print_z_mm};
        groups[{process, segment.fiber_occurrence}].push_back(&segment);
    }
    std::vector<MaskFiberGroup> result;
    for (auto& group : groups) result.emplace_back(group.first.first, std::move(group.second));
    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
        if (a.first != b.first) return a.first < b.first;
        const auto& pa = a.second.front()->start_mm;
        const auto& pb = b.second.front()->start_mm;
        return std::tie(pa.x, pa.y, pa.z) < std::tie(pb.x, pb.y, pb.z);
    });
    return result;
}
void check_distant_fiber(const libslicer::ToolpathPreview& actual, const libslicer::ToolpathPreview& expected)
{
    const auto a = distant_fiber_groups(actual), b = distant_fiber_groups(expected);
    REQUIRE_FALSE(a.empty());
    REQUIRE(a.size() == b.size());
    // Finalizer normalizes commands within 0.002 mm. Preview time interpolation
    // may split an unchanged command differently; allow only that spatial bound
    // plus float-coordinate error, while keeping phases and process exact.
    constexpr double tolerance_mm = 0.0021;
    const auto distance = [](const auto& p, const auto& q) { return std::hypot(p.x-q.x, p.y-q.y, p.z-q.z); };
    for (std::size_t i = 0; i < a.size(); ++i) {
        INFO("fiber group=" << i);
        REQUIRE(a[i].first == b[i].first);
        CHECK(distance(a[i].second.front()->start_mm, b[i].second.front()->start_mm) <= tolerance_mm);
        CHECK(distance(a[i].second.back()->end_mm, b[i].second.back()->end_mm) <= tolerance_mm);
        for (auto field : {&libslicer::ToolpathSegment::extrusion_delta_mm,
                           &libslicer::ToolpathSegment::fiber_feed_delta_mm,
                           &libslicer::ToolpathSegment::deposited_path_length_mm}) {
            const auto total = [field](const auto& group) {
                double sum = 0; for (const auto* segment : group) sum += segment->*field; return sum;
            };
            CHECK(std::abs(total(a[i].second) - total(b[i].second)) <= tolerance_mm);
        }
        const auto check_geometry = [&](const auto& source, const auto& target) {
            for (const auto* segment : source) {
                for (const auto& point : {segment->start_mm, segment->end_mm}) {
                    double closest = std::numeric_limits<double>::infinity();
                    for (const auto* other : target) {
                        const auto& start = other->start_mm;
                        const auto& end = other->end_mm;
                        const double dx = end.x-start.x, dy = end.y-start.y, dz = end.z-start.z;
                        const double squared_length = dx*dx + dy*dy + dz*dz;
                        const double t = squared_length == 0 ? 0 : std::clamp(
                            ((point.x-start.x)*dx + (point.y-start.y)*dy + (point.z-start.z)*dz) / squared_length, 0.0, 1.0);
                        closest = std::min(closest, std::hypot(point.x-start.x-t*dx, point.y-start.y-t*dy, point.z-start.z-t*dz));
                    }
                    CHECK(closest <= tolerance_mm);
                }
            }
        };
        check_geometry(a[i].second, b[i].second);
        check_geometry(b[i].second, a[i].second);
    }
}
// Ordinary modifier grouping adds two native 10-grid safety offsets. Its
// 0.000020-mm source change may shift command subdivision and preview timing;
// compare continuous deposited curves, retaining each phase and process.
bool same_resin_only_fiber_geometry(const libslicer::ToolpathPreview& actual,
                                   const libslicer::ToolpathPreview& expected)
{
    const auto a = distant_fiber_groups(actual, -std::numeric_limits<double>::infinity());
    const auto b = distant_fiber_groups(expected, -std::numeric_limits<double>::infinity());
    if (a.empty() || a.size() != b.size()) return false;
    const auto float_error = [](const auto& p) {
        const auto ulp = [](double value) {
            const float f = float(std::abs(value));
            return double(std::nextafter(f, std::numeric_limits<float>::infinity()) - f);
        };
        return std::hypot(ulp(p.x), ulp(p.y), ulp(p.z));
    };
    constexpr double native_offset_mm = 0.000020;
    const double rounding_mm = std::sqrt(2.0) * 0.001 + native_offset_mm;
    const auto distance = [](const auto& p, const auto& q) {
        return std::hypot(p.x-q.x, p.y-q.y, p.z-q.z);
    };
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].first != b[i].first) return false;
        const auto close = [&](const auto& p, const auto& q) {
            return distance(p, q) <= rounding_mm + float_error(p) + float_error(q);
        };
        if (!close(a[i].second.front()->start_mm, b[i].second.front()->start_mm) ||
            !close(a[i].second.back()->end_mm, b[i].second.back()->end_mm)) return false;
        const auto directed = [&](const auto& source, const auto& target) {
            for (const auto* segment : source)
                for (const auto& point : {segment->start_mm, segment->end_mm}) {
                    bool found = false;
                    for (const auto* other : target) {
                        const auto& start = other->start_mm;
                        const auto& end = other->end_mm;
                        const double dx=end.x-start.x, dy=end.y-start.y, dz=end.z-start.z;
                        const double length2=dx*dx+dy*dy+dz*dz;
                        const double t=length2==0?0:std::clamp(
                            ((point.x-start.x)*dx+(point.y-start.y)*dy+(point.z-start.z)*dz)/length2,0.0,1.0);
                        if (std::hypot(point.x-start.x-t*dx, point.y-start.y-t*dy, point.z-start.z-t*dz) <=
                            rounding_mm + float_error(point) + std::max(float_error(start), float_error(end))) {
                            found = true;
                            break;
                        }
                    }
                    if (!found) return false;
                }
            return true;
        };
        if (!directed(a[i].second,b[i].second) || !directed(b[i].second,a[i].second)) return false;
    }
    return true;
}
struct MaskFiberCommands {
    std::map<std::string,std::string> metadata;
    std::vector<std::string> phases;
    std::vector<std::tuple<std::string,double,bool,bool>> commands;
    std::map<std::string,std::pair<double,size_t>> feed;
};
std::vector<MaskFiberCommands> mask_fiber_commands(const std::string& path)
{
    std::ifstream input(path);
    REQUIRE(input.good());
    std::vector<MaskFiberCommands> result;
    MaskFiberCommands* block = nullptr;
    std::string phase, line;
    double speed = 0;
    bool relative_e = false;
    const std::regex number(R"(([XYZEF])([-+0-9.]+))");
    while (std::getline(input,line)) {
        if (line == "M83" || line.rfind("M83 ",0)==0) relative_e = true;
        if (line == "M82" || line.rfind("M82 ",0)==0) relative_e = false;
        if (line.rfind(";FIBER_BEGIN ",0)==0) {
            // This fixture exports relative E. Absolute E/G92 needs a different
            // accumulator and must never silently enter this rounding oracle.
            REQUIRE(relative_e);
            result.emplace_back(); block=&result.back(); phase="approach";
            std::istringstream header(line); std::string item;
            while (header>>item) {
                const auto equals=item.find('=');
                if (equals==std::string::npos) continue;
                const auto key=item.substr(0,equals);
                if (key!="object" && key!="occurrence") block->metadata[key]=item.substr(equals+1);
            }
        } else if (block) {
            if (line.rfind(";FIBER_",0)==0) {
                phase=line; block->phases.push_back(line);
                if (line==";FIBER_END") block=nullptr;
            } else if (line.rfind("G1 ",0)==0) {
                bool xy=false, has_e=false; double feed=0;
                for (std::sregex_iterator it(line.begin(),line.end(),number), end; it!=end; ++it) {
                    const char axis=(*it)[1].str().front();
                    const double value=std::stod((*it)[2].str());
                    if (axis=='F') speed=value;
                    else if (axis=='E') { has_e=true; feed=value; }
                    else xy=true;
                }
                if (xy || has_e) {
                    block->commands.emplace_back(phase,speed,xy,has_e);
                    if (has_e) { auto& total=block->feed[phase]; total.first+=feed; ++total.second; }
                }
            }
        }
    }
    REQUIRE_FALSE(result.empty());
    // Resin execution may reorder contour and infill blocks. Match process
    // metadata; preserve each occurrence and a stable ordinal for repeated keys.
    std::stable_sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
        return a.metadata < b.metadata;
    });
    return result;
}
bool same_resin_only_fiber_commands(const std::vector<MaskFiberCommands>& actual,
                                   const std::vector<MaskFiberCommands>& expected)
{
    if (actual.size()!=expected.size()) return false;
    for (size_t i=0;i<actual.size();++i) {
        const auto& a=actual[i]; const auto& b=expected[i];
        if (a.metadata!=b.metadata || a.phases!=b.phases || a.commands.size()!=b.commands.size() || a.feed.size()!=b.feed.size()) return false;
        for(size_t command=0;command<a.commands.size();++command) {
            const auto& x=a.commands[command];const auto& y=b.commands[command];
            if(std::get<0>(x)!=std::get<0>(y) || std::get<2>(x)!=std::get<2>(y) || std::get<3>(x)!=std::get<3>(y))return false;
            // Residual resin may introduce a tool activation before this same
            // strand. Only the first approach then receives the configured cap;
            // dedicated approach tests check that policy. Process speeds stay exact.
            if((std::get<0>(x)!="approach" || std::get<3>(x)) && std::get<1>(x)!=std::get<1>(y))return false;
        }
        const double units=std::stod(a.metadata.at("e_units_per_mm"));
        for (const auto& [phase,feed] : a.feed) {
            const auto found=b.feed.find(phase);
            if (found==b.feed.end() || feed.second!=found->second.second) return false;
            // Each file independently rounds every E command to five decimal
            // places. Counts come from original commands, never preview splits.
            const double budget=0.5e-5*(feed.second+found->second.second)/units;
            if (phase==";FIBER_PREFEED_BEGIN" && feed.first!=found->second.first) return false;
            if (phase==";FIBER_TAIL_BEGIN" && (feed.first!=0 || found->second.first!=0)) return false;
            if (std::abs(feed.first-found->second.first)/units > budget+1e-10) return false;
        }
    }
    return true;
}

}

TEST_CASE("mask override data contains explicit edits and rebases inherited values", "[libslicer_api][config][fiber-mask][overrides]")
{
    auto base = libslicer::Config::defaults();
    REQUIRE(base.set("fiber_width", "0.8"));
    auto mask = libslicer::Config::for_overrides(base.snapshot(), libslicer::ConfigScope::FiberMask);
    CHECK(mask.serialize_overrides() == "{}");
    REQUIRE(mask.set("reinforced_infill_pattern", "concentric"));
    const auto payload = nlohmann::json::parse(mask.serialize_overrides());
    REQUIRE(payload.size() == 1);
    CHECK(payload.at("reinforced_infill_pattern") == "concentric");
    REQUIRE(base.set("fiber_width", "1.2"));
    auto rebased = libslicer::Config::for_overrides(base.snapshot(), libslicer::ConfigScope::FiberMask);
    REQUIRE(rebased.load_overrides(mask.serialize_overrides()));
    CHECK(rebased.snapshot().value("fiber_width") == base.snapshot().value("fiber_width"));
    CHECK(rebased.snapshot().value("reinforced_infill_pattern") == "concentric");

    // An explicit value equal to the baseline must remain pinned after rebasing.
    const auto same_value = *base.snapshot().value("fiber_width");
    REQUIRE(rebased.set("fiber_width", same_value));
    REQUIRE(base.set("fiber_width", "1.6"));
    auto pinned = libslicer::Config::for_overrides(base.snapshot(), libslicer::ConfigScope::FiberMask);
    REQUIRE(pinned.load_overrides(rebased.serialize_overrides()));
    CHECK(pinned.snapshot().value("fiber_width") == same_value);
    REQUIRE(pinned.reset("fiber_width"));
    CHECK(pinned.snapshot().value("fiber_width") == base.snapshot().value("fiber_width"));
    CHECK_FALSE(nlohmann::json::parse(pinned.serialize_overrides()).contains("fiber_width"));
}

TEST_CASE("override context reports ownership and isolates copied edits", "[libslicer_api][config][fiber-mask][overrides]")
{
    const auto base = libslicer::Config::defaults();
    auto mask = libslicer::Config::for_overrides(base.snapshot(), libslicer::ConfigScope::FiberMask);
    const auto value = *base.snapshot().value("fiber_width");
    const auto result = mask.set("fiber_width", value);
    REQUIRE(result);
    REQUIRE(result.changed_items.size() == 1);
    CHECK(result.changed_items.front().overridden);
    auto copy = mask;
    REQUIRE(copy.reset("fiber_width"));
    CHECK(copy.serialize_overrides() == "{}");
    CHECK_FALSE(mask.overrides().empty());
    REQUIRE(mask.load_overrides("{}"));
    CHECK(mask.serialize_overrides() == "{}");
    const auto items = mask.settings();
    CHECK(std::none_of(items.begin(), items.end(), [](const auto& item) { return item.overridden; }));
    CHECK(std::none_of(items.begin(), items.end(), [](const auto& item) { return item.key == "layer_height"; }));
    CHECK_FALSE(mask.set(libslicer::ConfigScope::Full, "layer_height", "0.3"));
    CHECK_FALSE(mask.reset("layer_height"));
    CHECK(mask.serialize_overrides() == "{}");
    auto ordinary = libslicer::Config::defaults();
    CHECK_FALSE(ordinary.load_overrides("{}"));
    REQUIRE(ordinary.set("fiber_width", "1.2"));
    REQUIRE(ordinary.reset("fiber_width"));
    CHECK(ordinary.snapshot().value("fiber_width") == base.snapshot().value("fiber_width"));
}

TEST_CASE("SDK override import is atomic and retains legacy full parameter data", "[libslicer_api][config][fiber-mask][overrides]")
{
    auto base = libslicer::Config::defaults();
    REQUIRE(base.set("fiber_width", "0.8"));
    auto mask = libslicer::Config::for_overrides(base.snapshot(), libslicer::ConfigScope::FiberMask);
    REQUIRE(mask.set("reinforced_infill_pattern", "concentric"));
    const auto saved = mask.serialize_overrides();
    const auto values = mask.snapshot().values();
    SECTION("invalid data never replaces the current patch") {
        const auto invalid = GENERATE("[]", "{", R"({"fiber_width":1.2})",
            R"({"fiber_width":"1.2","unknown_setting":"1"})",
            R"({"fiber_width":"1.2","layer_height":"0.3"})",
            R"({"fiber_width":"40%"})");
        CHECK_FALSE(mask.load_overrides(invalid));
        CHECK(mask.serialize_overrides() == saved);
        CHECK(mask.snapshot().values() == values);
        CHECK_FALSE(mask.apply_patch({{"fiber_width", "1.2"}, {"layer_height", "0.3"}}));
        CHECK(mask.serialize_overrides() == saved);
        CHECK(mask.snapshot().values() == values);
    }
    SECTION("legacy snapshots retain every explicitly stored value") {
        auto legacy = nlohmann::json::object();
        for (const auto& item : base.settings(libslicer::ConfigScope::FiberMask))
            legacy[item.key] = item.value;
        REQUIRE(base.set("fiber_width", "1.2"));
        auto loaded = libslicer::Config::for_overrides(base.snapshot(), libslicer::ConfigScope::FiberMask);
        REQUIRE(loaded.load_overrides(legacy.dump()));
        CHECK(loaded.overrides().size() == legacy.size());
        CHECK(nlohmann::json::parse(loaded.serialize_overrides()) == legacy);
        CHECK(loaded.snapshot().value("fiber_width") != base.snapshot().value("fiber_width"));
        REQUIRE(loaded.load_overrides(R"({"reinforced_infill_pattern":"concentric"})"));
        CHECK(loaded.overrides().size() == 1);
        CHECK(loaded.snapshot().value("fiber_width") == base.snapshot().value("fiber_width"));
    }
}

TEST_CASE("fiber mask scope is atomic and independent of inactive fiber presentation", "[libslicer_api][fiber-mask]")
{
    auto config = libslicer::Config::defaults();
    const auto items = config.settings(libslicer::ConfigScope::FiberMask);
    REQUIRE(items.size() == 42);
    std::set<std::string> keys;
    for (const auto& item : items) keys.insert(item.key);
    CHECK_FALSE(keys.count("fiber_mask_process"));
    CHECK_FALSE(keys.count("layer_height"));
    auto density = std::find_if(items.begin(), items.end(), [](const auto& item) {
        return item.key == "fiber_resin_fill_density";
    });
    REQUIRE(density != items.end());
    CHECK(density->enabled);
    const auto before = config.snapshot().values();
    CHECK_FALSE(config.apply_patch(libslicer::ConfigScope::FiberMask,
        {{"fiber_width", "1.2"}, {"layer_height", "0.3"}}).success);
    CHECK(config.snapshot().values() == before);
    CHECK_FALSE(config.set("fiber_mask_process", "1").success);
    CHECK_FALSE(config.snapshot().value("fiber_mask_process").has_value());
    CHECK(config.set(libslicer::ConfigScope::FiberMask, "fiber_resin_fill_density", "75%").success);
}

TEST_CASE("fiber width fixture rejects an incompatible scanline pitch and bend radius", "[libslicer_api][fiber-width][slice]")
{
    auto library = mask_library();
    REQUIRE(library->apply_active_config_patch({
        {"generate_reinforced_infills", "1"}, {"reinforced_infill_pattern", "rectilinear"},
        {"reinforced_infill_density", "100%"}, {"fiber_width", "0.8"},
        {"fiber_infill_bend_radius", "0.5"}, {"fiber_infill_angle_sequence", "0"},
        {"sparse_infill_density", "40%"}}).success);
    const auto result = library->slice(mask_request(*library));
    CHECK_FALSE(result.success);
    CHECK(std::any_of(result.diagnostics.begin(), result.diagnostics.end(), [](const auto& issue) {
        return !issue.warning && issue.message.find(
            "fiber_infill_bend_radius requires scanline spacing >= twice the configured radius") != std::string::npos;
    }));
    remove_slice(result);
}

TEST_CASE("masked App layer 23 survives planning reserve and machine rounding around real holes", "[libslicer_api][fiber-mask][slice]")
{
    libslicer::LibraryOptions options;
    options.resource_directory = LIBSLICER_TEST_RESOURCE_DIR;
    options.vendors = {"CFSYS"};
    auto library = libslicer::Library::open(options);
    REQUIRE(library);
    libslicer::ConfigSelection selection;
    selection.machine_model_id = "CFSYS Alpha500 Printer";
    selection.machine_variant_id = "0.4";
    selection.process_preset_id = "CCF&CIRON @CFSYS";
    selection.filament_preset_ids = {"CFSYS CIRON", "CFSYS CCF"};
    selection.filament_physical_tools = {0, 1};
    // Keep the actual App's sparse interior, solid surfaces and process lengths.
    // A second nozzle phase exercises rounding without changing policy geometry.
    const auto offset = GENERATE("-19x0,-19x0", "0x0,0.1734x0.2876");
    REQUIRE(library->activate_config(selection, {
        {"generate_reinforced_perimeters", "0"}, {"generate_reinforced_infills", "0"},
        {"fiber_fill_debug", "1"}, {"extruder_offset", offset},
        {"filament_colour", "#FFFFFF;#000000"}}).success);
    libslicer::SliceRequest request;
    request.config = *library->active_config_snapshot();
    request.center_on_build_plate = false;
    auto object = fiber_infill_block();
    for (auto& vertex : object.volumes.front().vertices) {
        vertex.x -= 130;
        vertex.y -= 120;
        vertex.z = vertex.z * 0.5 - 2;
    }
    object.transform[3] = 217.5;
    object.transform[7] = 177.5;
    object.transform[11] = 2;
    libslicer::SliceFiberMaskInput mask;
    mask.id = "app-layer-23";
    mask.dimensions_mm = {32, 24, 4};
    mask.settings = {
        {"fiber_concentric_corner_stabilization_length", "0"},
        {"fiber_contour_acceleration", "500"}, {"fiber_contour_bend_radius", "0"},
        {"fiber_contour_boundary_clearance", "0"}, {"fiber_contour_feed_ratio", "1"},
        {"fiber_contour_include_holes", "0"}, {"fiber_contour_infill_clearance", "0"},
        {"fiber_contour_max_speed", "10"}, {"fiber_contour_min_speed", "10"},
        {"fiber_corner_stabilization_length", "5"}, {"fiber_infill_acceleration", "500"},
        {"fiber_infill_angle_sequence", "0"}, {"fiber_infill_bend_radius", "0"},
        {"fiber_infill_feed_ratio", "1"}, {"fiber_infill_max_speed", "10"},
        {"fiber_infill_min_speed", "10"}, {"fiber_layer_height_ratio", "1"},
        {"fiber_minimum_effective_length", "0"}, {"fiber_minimum_path_length", "1"},
        {"fiber_resin_fill_acceleration", "100%"}, {"fiber_resin_fill_align_to_model", "0"},
        {"fiber_resin_fill_anchor", "400%"}, {"fiber_resin_fill_anchor_max", "20"},
        {"fiber_resin_fill_density", "50%"}, {"fiber_resin_fill_direction", "45"},
        {"fiber_resin_fill_line_width", "0"}, {"fiber_resin_fill_multiline", "1"},
        {"fiber_resin_fill_pattern", "rectilinear"}, {"fiber_resin_fill_rotate_template", ""},
        {"fiber_resin_fill_speed", "17"}, {"fiber_resin_overlap", "0.05"},
        {"fiber_width", "1"}, {"generate_reinforced_infills", "1"},
        {"generate_reinforced_perimeters", "1"}, {"outer_reinforced_perimeters_counts", "1"},
        {"reinforced_infill_density", "40%"}, {"reinforced_infill_filament", "2"},
        {"reinforced_infill_pattern", "rectilinear"}, {"reinforced_perimeters_filament", "2"}};
    object.fiber_masks.push_back(std::move(mask));
    request.objects.push_back(std::move(object));
    const auto result = library->slice(request);
    require_slice(result);
    REQUIRE(result.summary.layer_count == 31);
    const auto imported = library->load_gcode_preview({result.output.path});
    REQUIRE(imported.success);
    REQUIRE(imported.preview);

    // OriginalContourRegion describes the native eligible fill domain. Its
    // twelve holes include shell/classification gaps; they are not automatically
    // foreign fiber policies. Strict foreign capsules are checked against actual
    // prepared contexts by the native fixture, including a negative control.
    size_t target_holes = 0;
    for (const auto& diagnostic : result.preview->fiber_fill_diagnostics)
        if (diagnostic.kind == libslicer::FiberDiagnosticKind::OriginalContourRegion &&
            diagnostic.layer_index == 22 && !diagnostic.boundaries.empty())
            target_holes += diagnostic.boundaries.size()-1;
    REQUIRE(target_holes == 12);
    size_t target_contours = 0, target_infills = 0, deposits = 0;
    for (const auto& segment : imported.preview->segments) if (depositing_fiber(segment)) {
        INFO("offset=" << offset << " layer=" << segment.layer_index << " segment=" << segment.id);
        REQUIRE(segment.filament_id == 1);
        ++deposits;
        if (segment.layer_index == 22) {
            if (segment.extrusion_role == libslicer::ToolpathExtrusionRole::ContinuousFiberContour) ++target_contours;
            if (segment.extrusion_role == libslicer::ToolpathExtrusionRole::ContinuousFiberInfill) ++target_infills;
        }
    }
    CHECK(deposits > 0);
    CHECK(target_contours > 0);
    CHECK(target_infills > 0);
    remove_slice(result);
}

TEST_CASE("fiber mask input diagnostics identify owner and mask before processing", "[libslicer_api][fiber-mask]")
{
    auto library = mask_library();
    auto request = mask_request(*library);
    request.objects[0].fiber_masks.push_back(box_mask());
    SECTION("invalid dimensions") { request.objects[0].fiber_masks[0].dimensions_mm[0] = 0; }
    SECTION("nonrigid pose") { request.objects[0].fiber_masks[0].mask_to_object[0] = 2; }
    SECTION("unknown parameter") { request.objects[0].fiber_masks[0].settings = {{"wall_loops", "4"}}; }
    SECTION("missing material") { request.objects[0].fiber_masks[0].settings = {{"reinforced_infill_filament", "9"}}; }
    SECTION("unsupported residual pattern") { request.objects[0].fiber_masks[0].settings = {{"fiber_resin_fill_pattern", "lightning"}}; }
    SECTION("duplicate id") { request.objects[0].fiber_masks.push_back(box_mask()); }
    SECTION("unsupported combination") {
        request.objects[0].fiber_masks[0].settings = {{"generate_reinforced_infills", "1"}, {"reinforced_infill_filament", "2"}};
        request.config = libslicer::ConfigSnapshot{};
        REQUIRE(library->apply_active_config_patch({{"infill_combination", "1"}}).success);
        request.config = *library->active_config_snapshot();
    }
    const auto result = library->slice(request);
    REQUIRE_FALSE(result.success);
    REQUIRE_FALSE(result.diagnostics.empty());
    CHECK(result.diagnostics.front().message.find("fiber-infill-block") != std::string::npos);
    CHECK(result.diagnostics.front().message.find("central") != std::string::npos);
}

TEST_CASE("masked fiber enables zero-density core and final output stays within the policy box", "[libslicer_api][fiber-mask][slice]")
{
    auto library = mask_library();
    auto request = mask_request(*library);
    auto mask = box_mask();
    mask.settings = {{"generate_reinforced_perimeters", "1"},
        {"generate_reinforced_infills", "1"}, {"reinforced_infill_density", "40%"},
        {"reinforced_perimeters_filament", "2"}, {"reinforced_infill_filament", "2"},
        {"fiber_resin_fill_density", "50%"}};
    request.objects[0].fiber_masks = {mask};
    const auto result = library->slice(request);
    require_slice(result);
    const auto imported = library->load_gcode_preview({result.output.path});
    REQUIRE(imported.success);
    REQUIRE(imported.preview);
    size_t contours = 0, infills = 0, resin = 0;
    for (const auto& segment : imported.preview->segments) {
        if (depositing_fiber(segment)) {
            if (segment.extrusion_role == libslicer::ToolpathExtrusionRole::ContinuousFiberContour) ++contours;
            if (segment.extrusion_role == libslicer::ToolpathExtrusionRole::ContinuousFiberInfill) ++infills;
            const double radius = 0.5 * segment.width_mm;
            // Independent analytic footprint check of both final rounded endpoints.
            for (const auto& point : {segment.start_mm, segment.end_mm}) {
                CHECK(point.x - radius >= 116 - 0.00001);
                CHECK(point.x + radius <= 144 + 0.00001);
                CHECK(point.y - radius >= 108 - 0.00001);
                CHECK(point.y + radius <= 132 + 0.00001);
            }
            CHECK(segment.filament_id == 1);
        } else if (segment.extrusion_role == libslicer::ToolpathExtrusionRole::ResinInfill &&
                   segment.deposition == libslicer::ToolpathDepositionKind::Thermoplastic) {
            ++resin;
            CHECK(segment.filament_id == 0);
        }
    }
    CHECK(contours > 0);
    CHECK(infills > 0);
    CHECK(resin > 0);
    remove_slice(result);
}

TEST_CASE("masked residue-only full snapshot accepts unused resin material and produces local resin", "[libslicer_api][fiber-mask][slice]")
{
    auto library = mask_library();
    auto request = mask_request(*library);
    auto mask = box_mask();
    auto snapshot = libslicer::Config::defaults();
    REQUIRE(snapshot.apply_patch(libslicer::ConfigScope::FiberMask, {
        {"generate_reinforced_perimeters", "0"}, {"generate_reinforced_infills", "0"},
        {"fiber_resin_fill_density", "50%"}, {"fiber_resin_fill_speed", "17"}}).success);
    for (const auto& item : snapshot.settings(libslicer::ConfigScope::FiberMask))
        mask.settings.emplace_back(item.key, item.value);
    request.objects[0].fiber_masks = {mask};
    const auto result = library->slice(request);
    require_slice(result);
    size_t resin = 0;
    for (const auto& segment : result.preview->segments) {
        CHECK_FALSE(depositing_fiber(segment));
        if (segment.extrusion_role == libslicer::ToolpathExtrusionRole::ResinInfill &&
            segment.deposition == libslicer::ToolpathDepositionKind::Thermoplastic) {
            ++resin;
            for (const auto& point : {segment.start_mm, segment.end_mm}) {
                CHECK(point.x >= 116 - 0.01); CHECK(point.x <= 144 + 0.01);
                CHECK(point.y >= 108 - 0.01); CHECK(point.y <= 132 + 0.01);
            }
        }
    }
    CHECK(resin > 0);
    remove_slice(result);
}

TEST_CASE("disabled and non-intersecting masks preserve baseline deposited paths", "[libslicer_api][fiber-mask][slice]")
{
    auto library = mask_library();
    auto request = mask_request(*library);
    const auto baseline = library->slice(request);
    require_slice(baseline);
    const bool disabled = GENERATE(true, false);
    auto mask = box_mask();
    mask.enabled = !disabled;
    if (!disabled) mask.mask_to_object[3] = 300;
    mask.settings = {{"generate_reinforced_infills", "1"}, {"reinforced_infill_filament", "2"}};
    request.objects[0].fiber_masks = {mask};
    const auto result = library->slice(request);
    require_slice(result);
    check_mask_deposition(mask_deposition_signature(*result.preview), mask_deposition_signature(*baseline.preview));
    remove_slice(baseline); remove_slice(result);
}

TEST_CASE("masked layer preserves a distant active fiber component and its process", "[libslicer_api][fiber-mask][slice]")
{
    auto library = mask_library();
    REQUIRE(library->apply_active_config_patch({
        {"generate_reinforced_infills", "1"}, {"reinforced_infill_filament", "2"},
        {"reinforced_infill_pattern", "rectilinear"}, {"reinforced_infill_density", "40%"},
        {"sparse_infill_density", "15%"}}).success);
    auto request = mask_request(*library);
    auto& combined = request.objects[0].volumes[0];
    auto far_volume = combined;
    for (auto& vertex : far_volume.vertices) vertex.x += 80;
    const auto vertex_offset = static_cast<std::uint32_t>(combined.vertices.size());
    combined.vertices.insert(combined.vertices.end(), far_volume.vertices.begin(), far_volume.vertices.end());
    for (auto triangle : far_volume.triangles) {
        triangle.vertex_a += vertex_offset;
        triangle.vertex_b += vertex_offset;
        triangle.vertex_c += vertex_offset;
        combined.triangles.push_back(triangle);
    }
    const auto baseline = library->slice(request);
    require_slice(baseline);
    std::size_t far_fibers = 0;
    std::size_t all_fibers = 0;
    for (const auto& segment : baseline.preview->segments)
        if (depositing_fiber(segment)) {
            ++all_fibers;
            if (segment.start_mm.x >= 180 && segment.end_mm.x >= 180) ++far_fibers;
        }
    INFO("baseline=" << baseline.output.path << " fiber_segments=" << all_fibers);
    REQUIRE(far_fibers > 0);
    auto mask = box_mask();
    mask.settings = {{"reinforced_infill_density", "60%"}, {"fiber_resin_fill_density", "50%"}};
    request.objects[0].fiber_masks = {mask};
    const auto result = library->slice(request);
    require_slice(result);
    const auto left_fiber_signature = [](const auto& preview) {
        auto signature = mask_deposition_signature(preview, -std::numeric_limits<double>::infinity(), true);
        signature.erase(std::remove_if(signature.begin(), signature.end(), [](const auto& segment) {
            return segment[0] >= 180 || segment[3] >= 180;
        }), signature.end());
        return signature;
    };
    REQUIRE(left_fiber_signature(*result.preview) != left_fiber_signature(*baseline.preview));
    // Compare the untouched fiber domain, including its powered/passive phases.
    // Ordinary wall seams may be moved or subdivided by native travel ordering.
    // Disabled/non-intersecting masks above still compare all deposited materials.
    check_distant_fiber(*result.preview, *baseline.preview);
    remove_slice(baseline); remove_slice(result);
}


TEST_CASE("height limited Box overrides fiber only within its sliced Z range", "[libslicer_api][fiber-mask][height-range][slice]")
{
    auto library = mask_library();
    auto request = mask_request(*library);
    auto base = libslicer::Config::for_overrides(request.config, libslicer::ConfigScope::Full);
    REQUIRE(base.apply_patch({
        {"generate_reinforced_perimeters", "1"}, {"generate_reinforced_infills", "1"},
        {"reinforced_perimeters_filament", "2"}, {"reinforced_infill_filament", "2"},
        {"reinforced_infill_density", "40%"}, {"fiber_width", "0.8"},
        {"top_shell_thickness", "0"}, {"bottom_shell_thickness", "0"}}));
    request.config = base.snapshot();
    if (const char* model = std::getenv("LIBSLICER_FIBER_VALIDATION_MODEL"))
        request.objects[0] = binary_stl_on_bed(model);
    double min_x = std::numeric_limits<double>::max(), min_y = min_x, min_z = min_x;
    double max_x = std::numeric_limits<double>::lowest(), max_y = max_x, max_z = max_x;
    for (const auto& vertex : request.objects[0].volumes.front().vertices) {
        min_x = std::min(min_x, double(vertex.x)); max_x = std::max(max_x, double(vertex.x));
        min_y = std::min(min_y, double(vertex.y)); max_y = std::max(max_y, double(vertex.y));
        min_z = std::min(min_z, double(vertex.z)); max_z = std::max(max_z, double(vertex.z));
    }
    const double height = max_z - min_z;
    REQUIRE(height >= 2);
    // Put faces midway between adjacent 0.2 mm slice planes.
    const double lower_z = std::floor(height / 3 / .2) * .2 + .05;
    const double upper_z = std::floor(height * 2 / 3 / .2) * .2 + .05;
    auto mask = box_mask("middle-height");
    // Cover the entire XY footprint, but only the middle of the model.
    // Boundaries are between slice planes, so no plane lies on a modifier face.
    mask.dimensions_mm = {max_x - min_x + 2, max_y - min_y + 2, upper_z - lower_z};
    mask.mask_to_object[3] = (min_x + max_x) * .5;
    mask.mask_to_object[7] = (min_y + max_y) * .5;
    mask.mask_to_object[11] = min_z + (lower_z + upper_z) * .5;
    mask.settings = {{"fiber_width", "1.2"}};
    request.objects[0].fiber_masks = {mask};
    const auto result = library->slice(request);
    require_slice(result);
    const auto imported = library->load_gcode_preview({result.output.path});
    REQUIRE(imported.success);
    REQUIRE(imported.preview);
    for (const auto* preview : {result.preview.get(), imported.preview.get()}) {
        std::array<std::size_t, 3> deposits{};
        std::size_t contours = 0, infills = 0;
        for (const auto& segment : preview->segments) {
            if (!depositing_fiber(segment)) continue;
            // Slice planes are at layer mid-height; landing Z hops do not
            // describe modifier ownership and must not classify these paths.
            const double slice_z = segment.print_z_mm - .5 * segment.height_mm;
            const auto band = slice_z < lower_z ? 0u : slice_z > upper_z ? 2u : 1u;
            ++deposits[band];
            INFO("slice_z=" << slice_z << " layer=" << segment.layer_index);
            CHECK(segment.width_mm == Catch::Approx(band == 1 ? 1.2 : .8).margin(.00001));
            if (segment.extrusion_role == libslicer::ToolpathExtrusionRole::ContinuousFiberContour) ++contours;
            if (segment.extrusion_role == libslicer::ToolpathExtrusionRole::ContinuousFiberInfill) ++infills;
        }
        INFO("below=" << deposits[0] << " middle=" << deposits[1] << " above=" << deposits[2]);
        for (const auto count : deposits) CHECK(count > 0);
        CHECK(contours > 0);
        CHECK(infills > 0);
    }
    remove_slice(result);
}

TEST_CASE("masked skipped fiber layers retain their own residual resin", "[libslicer_api][fiber-mask][slice]")
{
    auto library = mask_library();
    auto request = mask_request(*library);
    auto mask = box_mask();
    mask.settings = {{"generate_reinforced_infills", "1"}, {"reinforced_infill_filament", "2"},
        {"reinforced_infill_density", "50%"}, {"fiber_layer_height_ratio", "2"},
        {"fiber_resin_fill_density", "50%"}, {"fiber_resin_fill_speed", "17"}};
    request.objects[0].fiber_masks = {mask};
    const auto result = library->slice(request);
    require_slice(result);
    std::set<unsigned> fiber_layers, resin_layers;
    for (const auto& segment : result.preview->segments) {
        if (depositing_fiber(segment)) fiber_layers.insert(segment.layer_index);
        if (segment.extrusion_role == libslicer::ToolpathExtrusionRole::ResinInfill &&
            segment.deposition == libslicer::ToolpathDepositionKind::Thermoplastic) {
            resin_layers.insert(segment.layer_index);
            CHECK(segment.nominal_speed_mm_s == Catch::Approx(17).margin(0.01));
        }
    }
    CHECK_FALSE(fiber_layers.empty());
    CHECK(resin_layers.size() > fiber_layers.size());
    remove_slice(result);
}

TEST_CASE("native modifier overlap uses later values and inherits unpatched fiber settings", "[libslicer_api][fiber-mask][slice]")
{
    auto library = mask_library();
    auto request = mask_request(*library);
    auto first = box_mask("first");
    first.dimensions_mm = {40, 24, 2};
    first.settings = {{"generate_reinforced_infills", "1"}, {"reinforced_infill_filament", "2"},
        {"reinforced_infill_density", "40%"}, {"fiber_width", "0.8"}};
    auto last = box_mask("last");
    last.dimensions_mm = {20, 24, 2};
    last.settings = {{"fiber_width", "1.2"}};
    // Exercise the same SDK-owned patch export used by App documents.
    for (auto* mask : {&first, &last}) {
        auto editor = libslicer::Config::for_overrides(request.config, libslicer::ConfigScope::FiberMask);
        REQUIRE(editor.apply_patch(mask->settings));
        auto restored = libslicer::Config::for_overrides(request.config, libslicer::ConfigScope::FiberMask);
        REQUIRE(restored.load_overrides(editor.serialize_overrides()));
        mask->settings = restored.overrides();
    }
    request.objects[0].fiber_masks = {first, last};
    const auto result = library->slice(request);
    require_slice(result);
    size_t narrow = 0, wide = 0, internal_walls = 0;
    for (const auto& segment : result.preview->segments) {
        if (depositing_fiber(segment)) {
            if (segment.width_mm > 1) {
                ++wide;
                for (const auto& point : {segment.start_mm, segment.end_mm}) {
                    CHECK(point.x - segment.width_mm * .5 >= 120 - .00001);
                    CHECK(point.x + segment.width_mm * .5 <= 140 + .00001);
                }
            } else ++narrow;
        }
        if (segment.extrusion_role == libslicer::ToolpathExtrusionRole::InnerWall &&
            segment.start_mm.x > 115 && segment.start_mm.x < 145 &&
            segment.start_mm.y > 105 && segment.start_mm.y < 135) ++internal_walls;
    }
    CHECK(narrow > 0); CHECK(wide > 0); CHECK(internal_walls == 0);
    remove_slice(result);
}


TEST_CASE("curved native modifiers contain the emitted fiber footprint", "[libslicer_api][fiber-mask][slice]")
{
    auto library = mask_library();
    auto request = mask_request(*library);
    auto mask = box_mask();
    mask.shape = GENERATE(libslicer::FiberMaskShape::Cylinder, libslicer::FiberMaskShape::Sphere);
    mask.dimensions_mm = mask.shape == libslicer::FiberMaskShape::Sphere ?
        std::array<double, 3>{12,12,12} : std::array<double, 3>{24,24,2};
    mask.settings = {{"generate_reinforced_infills", "1"}, {"reinforced_infill_filament", "2"},
        {"reinforced_infill_density", "40%"}};
    request.objects[0].fiber_masks = {mask};
    const auto result = library->slice(request);
    require_slice(result);
    size_t deposits = 0;
    for (const auto& segment : result.preview->segments) if (depositing_fiber(segment)) {
        ++deposits;
        for (const auto& point : {segment.start_mm, segment.end_mm}) {
            const double distance = std::hypot(point.x-130, point.y-120);
            const double slice_z = segment.print_z_mm - 0.5 * segment.height_mm;
            const double radius = mask.shape == libslicer::FiberMaskShape::Sphere ?
                std::sqrt(36 - (slice_z-1)*(slice_z-1)) : 12;
            CHECK(distance + segment.width_mm*.5 <= radius + .00002);
        }
    }
    CHECK(deposits > 0);
    remove_slice(result);
}

TEST_CASE("masked box follows mirrored scaled parent and non-grid nozzle phase", "[libslicer_api][fiber-mask][slice]")
{
    auto library = mask_library();
    REQUIRE(library->apply_active_config_patch({{"extruder_offset", "0x0,0.1734x0.2876"}}).success);
    auto request = mask_request(*library);
    const double cosine = std::cos(0.5), sine = std::sin(0.5);
    const double a=-1.2*cosine, b=-.8*sine, c=-1.2*sine, d=.8*cosine;
    request.objects[0].transform = {a,b,0,300, c,d,0,130, 0,0,1,0, 0,0,0,1};
    auto mask = box_mask();
    mask.settings = {{"generate_reinforced_infills", "1"}, {"reinforced_infill_filament", "2"},
        {"reinforced_infill_density", "40%"}};
    request.objects[0].fiber_masks = {mask};
    const auto result = library->slice(request);
    require_slice(result);
    size_t deposits = 0;
    for (const auto& segment : result.preview->segments) if (depositing_fiber(segment)) {
        ++deposits;
        for (const auto& point : {segment.start_mm, segment.end_mm}) {
            const double x = point.x-300, y=point.y-130, det=a*d-b*c;
            const double local_x=(d*x-b*y)/det, local_y=(-c*x+a*y)/det;
            // Inverse row lengths convert a world-space circular footprint to each local bound.
            CHECK(local_x - segment.width_mm*.5/1.2 >= 116 - .00002);
            CHECK(local_x + segment.width_mm*.5/1.2 <= 144 + .00002);
            CHECK(local_y - segment.width_mm*.5/.8 >= 108 - .00002);
            CHECK(local_y + segment.width_mm*.5/.8 <= 132 + .00002);
        }
    }
    CHECK(deposits > 0);
    remove_slice(result);
}


TEST_CASE("empty same-value and restored masks preserve connected body fiber and resin", "[libslicer_api][fiber-mask][slice]")
{
    auto library = mask_library();
    const int interval = GENERATE(1, 2);
    REQUIRE(library->apply_active_config_patch({
        {"generate_reinforced_perimeters", "1"}, {"generate_reinforced_infills", "1"},
        {"reinforced_perimeters_filament", "2"}, {"reinforced_infill_filament", "2"},
        {"reinforced_infill_density", "40%"}, {"fiber_resin_fill_density", "40%"},
        {"fiber_layer_height_ratio", std::to_string(interval)}}).success);
    auto request = mask_request(*library);
    const auto baseline = library->slice(request);
    require_slice(baseline);
    const auto expected = mask_deposition_signature(*baseline.preview);
    REQUIRE_FALSE(mask_deposition_signature(*baseline.preview, -1e9, true).empty());
    auto mask = box_mask();
    SECTION("empty explicit patch") { request.objects[0].fiber_masks = {mask}; }
    SECTION("same canonical process values") {
        mask.settings = {{"fiber_width", "0.800"}, {"reinforced_infill_density", "40%"},
                         {"fiber_resin_fill_density", "40%"}};
        request.objects[0].fiber_masks = {mask};
    }
    SECTION("later overlap restores the common baseline") {
        mask.settings = {{"fiber_width", "1.2"}};
        auto restore = box_mask("restore");
        restore.settings = {{"fiber_width", "0.8"}};
        request.objects[0].fiber_masks = {mask, restore};
    }
    const auto actual = library->slice(request);
    require_slice(actual);
    check_mask_deposition(mask_deposition_signature(*actual.preview), expected);
    remove_slice(baseline); remove_slice(actual);
}

TEST_CASE("resin-only mask changes local residual use while preserving connected fiber geometry", "[libslicer_api][fiber-mask][slice]")
{
    auto library = mask_library();
    const int interval = GENERATE(1, 2);
    REQUIRE(library->apply_active_config_patch({
        {"generate_reinforced_perimeters", "1"}, {"generate_reinforced_infills", "1"},
        {"reinforced_perimeters_filament", "2"}, {"reinforced_infill_filament", "2"},
        // No upper solid layer: compare the complete fiber baseline without
        // native bridge-support classification changing at a modifier interface.
        {"top_shell_layers", "0"},
        {"reinforced_infill_density", "40%"}, {"fiber_resin_fill_density", "0%"},
        {"fiber_layer_height_ratio", std::to_string(interval)}}).success);
    auto request = mask_request(*library);
    const auto baseline = library->slice(request);
    require_slice(baseline);
    for (const auto role : {libslicer::ToolpathExtrusionRole::ContinuousFiberContour,
                            libslicer::ToolpathExtrusionRole::ContinuousFiberInfill})
        REQUIRE(std::any_of(baseline.preview->segments.begin(), baseline.preview->segments.end(),
            [&](const auto& segment) { return depositing_fiber(segment) && segment.extrusion_role == role; }));
    auto mask = box_mask();
    mask.settings = {{"fiber_resin_fill_density", "50%"}, {"fiber_resin_fill_speed", "17"}};
    request.objects[0].fiber_masks = {mask};
    const auto actual = library->slice(request);
    require_slice(actual);
    REQUIRE(same_resin_only_fiber_geometry(*actual.preview, *baseline.preview));
    const auto original = mask_fiber_commands(baseline.output.path);
    const auto changed = mask_fiber_commands(actual.output.path);
    INFO("baseline="<<baseline.output.path<<" actual="<<actual.output.path);
    REQUIRE(same_resin_only_fiber_commands(changed, original));
    auto wrong_speed=changed;
    bool changed_speed=false;
    for(auto& path:wrong_speed) {
        for(auto& command:path.commands) if(std::get<0>(command)==";FIBER_START" && std::get<3>(command)) {
            std::get<1>(command)+=1;changed_speed=true;break;
        }
        if(changed_speed)break;
    }
    REQUIRE(changed_speed);
    CHECK_FALSE(same_resin_only_fiber_commands(wrong_speed,original));
    auto translated = *actual.preview;
    for (auto& segment : translated.segments) if (depositing_fiber(segment)) {
        segment.start_mm.x += .01; segment.end_mm.x += .01;
    }
    CHECK_FALSE(same_resin_only_fiber_geometry(translated, *baseline.preview));
    auto missing = *actual.preview;
    const auto occurrence = std::find_if(missing.segments.begin(), missing.segments.end(), depositing_fiber)->fiber_occurrence;
    missing.segments.erase(std::remove_if(missing.segments.begin(), missing.segments.end(),
        [&](const auto& segment) { return segment.fiber_occurrence == occurrence; }), missing.segments.end());
    CHECK_FALSE(same_resin_only_fiber_geometry(missing, *baseline.preview));
    auto wrong_phase = *actual.preview;
    std::find_if(wrong_phase.segments.begin(), wrong_phase.segments.end(), depositing_fiber)->fiber_phase =
        libslicer::ToolpathFiberPhase::None;
    CHECK_FALSE(same_resin_only_fiber_geometry(wrong_phase, *baseline.preview));
    auto excessive_feed = changed;
    const auto powered = excessive_feed.front().feed.find(";FIBER_START");
    REQUIRE(powered != excessive_feed.front().feed.end());
    powered->second.first += .1;
    CHECK_FALSE(same_resin_only_fiber_commands(excessive_feed, original));
    std::set<int> resin_layers;
    for (const auto& segment : actual.preview->segments)
        if (segment.extrusion_role == libslicer::ToolpathExtrusionRole::ResinInfill &&
            segment.deposition == libslicer::ToolpathDepositionKind::Thermoplastic) {
            resin_layers.insert(int(segment.layer_index));
            for (const auto& point : {segment.start_mm, segment.end_mm}) {
                CHECK(point.x >= 116 - .01); CHECK(point.x <= 144 + .01);
                CHECK(point.y >= 108 - .01); CHECK(point.y <= 132 + .01);
            }
        }
    REQUIRE_FALSE(resin_layers.empty());
    if (interval == 2)
        CHECK(std::any_of(resin_layers.begin(), resin_layers.end(), [](int layer) { return layer % 2 == 1; }));
    remove_slice(baseline); remove_slice(actual);
}

TEST_CASE("resin-only mask supplies zero-density wall-free core above a native bottom foundation", "[libslicer_api][fiber-mask][slice]")
{
    auto library = mask_library();
    REQUIRE(library->apply_active_config_patch({{"wall_loops", "0"},
        {"top_shell_layers", "0"}, {"bottom_shell_layers", "1"}}).success);
    auto request = mask_request(*library);
    auto mask = box_mask();
    mask.settings = {{"fiber_resin_fill_density", "50%"}};
    request.objects[0].fiber_masks = {mask};
    bool has_foundation=true;
    SECTION("native bottom foundation supports local residual resin") {}
    SECTION("native empty first layer remains rejected") {
        REQUIRE(library->apply_active_config_patch({{"bottom_shell_layers", "0"}}).success);
        request.config=*library->active_config_snapshot();
        has_foundation=false;
    }
    const auto result = library->slice(request);
    if(!has_foundation) {
        CHECK_FALSE(result.success);
        REQUIRE_FALSE(result.diagnostics.empty());
        CHECK_FALSE(result.cancelled);
        remove_slice(result);
        return;
    }
    require_slice(result);
    size_t resin = 0;
    for (const auto& segment : result.preview->segments) {
        CHECK_FALSE(depositing_fiber(segment));
        if (segment.extrusion_role == libslicer::ToolpathExtrusionRole::ResinInfill &&
            segment.deposition == libslicer::ToolpathDepositionKind::Thermoplastic) {
            ++resin;
            for(const auto& point:{segment.start_mm,segment.end_mm}) {
                CHECK(point.x>=116-.01); CHECK(point.x<=144+.01);
                CHECK(point.y>=108-.01); CHECK(point.y<=132+.01);
            }
        }
    }
    CHECK(resin > 0);
    remove_slice(result);
}

TEST_CASE("open ring connection settings persist their explicit software trim multipliers", "[libslicer_api][config][fiber][OpenRingConnection]")
{
    auto config=libslicer::Config::defaults();
    CHECK(config.snapshot().value("fiber_connect_adjacent_rings")=="0");
    CHECK(config.snapshot().value("fiber_connection_tail_trim_widths")=="1.5");
    CHECK(config.snapshot().value("fiber_connection_head_trim_widths")=="1");
    REQUIRE(config.apply_patch(libslicer::ConfigScope::FiberMask,{{"fiber_connect_adjacent_rings","1"},
        {"fiber_connection_tail_trim_widths","2.25"},{"fiber_connection_head_trim_widths","0"}}).success);
    const auto snapshot=config.snapshot();
    CHECK(snapshot.value("fiber_connect_adjacent_rings")=="1");
    CHECK(snapshot.value("fiber_connection_tail_trim_widths")=="2.25");
    CHECK(snapshot.value("fiber_connection_head_trim_widths")=="0");
    const auto before=config.snapshot().values();
    CHECK_FALSE(config.apply_patch(libslicer::ConfigScope::FiberMask,{{"fiber_connection_head_trim_widths","-1"}}).success);
    CHECK(config.snapshot().values()==before);
}

TEST_CASE("wipe tower exclusions preserve model tools and prime a single ordinary material",
          "[libslicer_api][p3][fiber][wipe-tower-exclusion]")
{
    const std::string tower_type = GENERATE(std::string("type1"), std::string("type2"));
    const unsigned ordinary_materials = GENERATE(1u, 2u);
    const unsigned shell_layers = GENERATE(0u, 1u);
    const bool machine_hooks = GENERATE(false, true);
    CAPTURE(tower_type, ordinary_materials, shell_layers, machine_hooks);
    libslicer::LibraryOptions options;
    options.resource_directory = LIBSLICER_TEST_RESOURCE_DIR;
    options.vendors = {"P3"};
    auto library = libslicer::Library::open(options);
    REQUIRE(library);
    libslicer::ConfigSelection selection;
    selection.machine_model_id = "P3";
    selection.machine_variant_id = "0.4";
    selection.process_preset_id = "0.20mm Standard @P3";
    selection.filament_preset_ids.assign(6, "Generic PLA @P3");
    selection.filament_preset_ids[5] = "CFSYS CCF";
    auto created = library->create_config(selection);
    REQUIRE(created.success);
    REQUIRE(created.config->snapshot().value("filament_exclude_from_wipe_tower") == "0,0,0,0,0,1");
    CHECK_FALSE(created.config->set("filament_exclude_from_wipe_tower", "0,0,0,0,0,0").success);
    const auto settings = created.config->settings();
    const auto flag = std::find_if(settings.begin(), settings.end(), [](const auto& item) {
        return item.key == "filament_exclude_from_wipe_tower";
    });
    REQUIRE(flag != settings.end());
    CHECK(flag->visible);
    CHECK_FALSE(flag->enabled);
    REQUIRE(created.config->apply_patch({
        {"filament_colour", "#E02222;#F6F6F3;#3F3F3F;#A0A0A0;#A0A0A0;#FF0080"},
        {"enable_prime_tower", "1"}, {"wipe_tower_type", tower_type},
        {"wipe_tower_wall_type", "rectangle"},
        {"generate_reinforced_perimeters", "1"}, {"outer_reinforced_perimeters_counts", "1"},
        {"reinforced_perimeters_filament", "6"},
        {"top_shell_layers", std::to_string(shell_layers)}, {"bottom_shell_layers", std::to_string(shell_layers)},
        {"top_shell_thickness", "0"}, {"bottom_shell_thickness", "0"},
        {"fiber_layer_height_ratio", "1"}, {"fiber_minimum_path_length", "2"}
    }).success);
    if (machine_hooks)
        REQUIRE(created.config->apply_patch({
            {"flush_into_infill", "1"}, {"timelapse_type", "0"},
            {"time_lapse_gcode", ";TEST_FIBER_TIMELAPSE {layer_num}"},
            {"enable_wrapping_detection", "1"},
            {"wrapping_detection_gcode", ";TEST_FIBER_WRAPPING {layer_num}"}
        }).success);
    libslicer::SliceRequest request;
    for (unsigned i = 0; i < ordinary_materials; ++i) {
        libslicer::SliceObjectInput object;
        object.name = "tower-exclusion-coupon-" + std::to_string(i);
        libslicer::SliceVolumeInput volume;
        const float x = 100.f + 75.f * i;
        volume.vertices = {{x,100,0},{x+60,100,0},{x+60,140,0},{x,140,0},
                           {x,100,2},{x+60,100,2},{x+60,140,2},{x,140,2}};
        volume.triangles = {{0,2,1},{0,3,2},{4,5,6},{4,6,7},{0,1,5},{0,5,4},
                            {1,2,6},{1,6,5},{2,3,7},{2,7,6},{3,0,4},{3,4,7}};
        volume.default_filament_slot = int(i + 1);
        object.volumes.push_back(std::move(volume));
        request.objects.push_back(std::move(object));
    }
    request.config = created.config->snapshot();
    const auto output = std::filesystem::temp_directory_path() /
        ("libslicer-wipe-exclusion-" + tower_type + "-" + std::to_string(ordinary_materials) + "-" + std::to_string(shell_layers) + ".gcode");
    std::filesystem::remove(output);
    request.output_gcode_path = output.string();
    const auto result = library->slice(request);
    std::string diagnostics;
    for (const auto& d : result.diagnostics) diagnostics += d.code + ": " + d.message + "\n";
    INFO(diagnostics);
    REQUIRE(result.success);
    REQUIRE(result.preview);
    size_t tower_segments = 0, fiber_segments = 0;
    std::set<unsigned> model_tools;
    for (const auto& s : result.preview->segments) {
        if (s.extrusion_role == libslicer::ToolpathExtrusionRole::WipeTower &&
            s.motion == libslicer::ToolpathMotionKind::Extrusion) {
            ++tower_segments;
            CHECK(s.tool_id < ordinary_materials);
            CHECK(s.filament_id < ordinary_materials);
        }
        if (s.deposition == libslicer::ToolpathDepositionKind::ContinuousFiberPowered ||
            s.deposition == libslicer::ToolpathDepositionKind::ContinuousFiberPassive) {
            ++fiber_segments;
            CHECK(s.tool_id == 5);
        }
        if (s.motion == libslicer::ToolpathMotionKind::Extrusion &&
            s.extrusion_role != libslicer::ToolpathExtrusionRole::WipeTower)
            model_tools.insert(s.tool_id);
    }
    CHECK(tower_segments > 0);
    CHECK(fiber_segments > 0);
    CHECK(model_tools.count(0) == 1);
    CHECK(model_tools.count(5) == 1);
    if (ordinary_materials == 2) CHECK(model_tools.count(1) == 1);
    std::ifstream generated(output);
    const std::string gcode((std::istreambuf_iterator<char>(generated)), {});
    CHECK(gcode.find(";FIBER_CUT\nM400\nS0\nM400\n") != std::string::npos);
    CHECK(gcode.find("filament_exclude_from_wipe_tower = 0,0,0,0,0,1") != std::string::npos);
    if (machine_hooks) {
        const auto tower_start = gcode.find("; WIPE_TOWER_START");
        REQUIRE(tower_start != std::string::npos);
        CHECK(gcode.find(";TEST_FIBER_TIMELAPSE 0\n") < tower_start);
        CHECK(gcode.find(";TEST_FIBER_WRAPPING 0\n") < tower_start);
    }
    int previous_tool = -1, physical_changes = 0;
    std::istringstream commands(gcode);
    std::string line;
    const std::regex tool_command("T([0-9]+)");
    std::smatch match;
    while (std::getline(commands, line))
        if (std::regex_match(line, match, tool_command)) {
            const int tool = std::stoi(match[1].str());
            if (previous_tool >= 0 && previous_tool != tool) ++physical_changes;
            previous_tool = tool;
        }
    REQUIRE(std::regex_search(gcode, match, std::regex("; total filament change = ([0-9]+)")));
    CHECK(std::stoi(match[1].str()) == physical_changes);
    if (tower_type == "type2" && ordinary_materials == 1 && shell_layers == 1) {
        // Trusting kernel-generated tower motion must not relax user-script checks.
        REQUIRE(created.config->set("change_filament_gcode", "UNVERIFIED_FIBER_MACRO").success);
        request.config = created.config->snapshot();
        request.output_gcode_path = (std::filesystem::temp_directory_path()/"libslicer-wipe-exclusion-unsafe.gcode").string();
        std::filesystem::remove(request.output_gcode_path);
        const auto unsafe = library->slice(request);
        CHECK_FALSE(unsafe.success);
        CHECK(std::any_of(unsafe.diagnostics.begin(), unsafe.diagnostics.end(), [](const auto& d) {
            return d.message.find("Unsupported P3") != std::string::npos;
        }));
        CHECK_FALSE(std::filesystem::exists(request.output_gcode_path));
    }
}

TEST_CASE("ordinary wipe tower uses the legacy path with absent or false exclusion",
          "[libslicer_api][wipe-tower-exclusion][legacy]")
{
    const std::string tower_type = GENERATE(std::string("type1"), std::string("type2"));
    const bool tower_enabled = GENERATE(false, true);
    const bool machine_hooks = GENERATE(false, true);
    CAPTURE(tower_type, tower_enabled, machine_hooks);
    libslicer::LibraryOptions options;
    options.resource_directory = LIBSLICER_TEST_RESOURCE_DIR;
    options.vendors = {"P3"};
    auto library = libslicer::Library::open(options);
    REQUIRE(library);
    libslicer::ConfigSelection selection;
    selection.machine_model_id = "P3"; selection.machine_variant_id = "0.4";
    selection.process_preset_id = "0.20mm Standard @P3";
    selection.filament_preset_ids.assign(6, "Generic PLA @P3");
    auto created = library->create_config(selection);
    REQUIRE(created.success);
    CHECK(created.config->snapshot().value("filament_exclude_from_wipe_tower") == "0,0,0,0,0,0");
    REQUIRE(created.config->apply_patch({{"enable_prime_tower", tower_enabled ? "1" : "0"},
        {"wipe_tower_type", tower_type},
        // Object comment IDs can be unset when object cancellation is disabled.
        // Keep this fixture deterministic without masking output bytes.
        {"gcode_label_objects", "0"},
        {"filament_colour", "#E02222;#F6F6F3;#3F3F3F;#A0A0A0;#A0A0A0;#A0A0A0"}}).success);
    if (machine_hooks)
        REQUIRE(created.config->apply_patch({
            {"timelapse_type", "0"}, {"time_lapse_gcode", ";TEST_LEGACY_TIMELAPSE {layer_num}"},
            {"enable_wrapping_detection", "1"},
            {"wrapping_detection_gcode", ";TEST_LEGACY_WRAPPING {layer_num}"}
        }).success);
    libslicer::SliceRequest request;
    for (unsigned i = 0; i < 2; ++i) {
        libslicer::SliceObjectInput object;
        object.name = "legacy-two-color-" + std::to_string(i);
        libslicer::SliceVolumeInput v;
        const float x = 100.f + 25.f * i;
        v.vertices = {{x,100,0},{x+20,100,0},{x+20,120,0},{x,120,0},
                      {x,100,1},{x+20,100,1},{x+20,120,1},{x,120,1}};
        v.triangles = {{0,2,1},{0,3,2},{4,5,6},{4,6,7},{0,1,5},{0,5,4},
                       {1,2,6},{1,6,5},{2,3,7},{2,7,6},{3,0,4},{3,4,7}};
        v.default_filament_slot = int(i + 1); object.volumes.push_back(std::move(v));
        request.objects.push_back(std::move(object));
    }
    request.output_gcode_path = (std::filesystem::temp_directory_path()/"libslicer-legacy-exclusion.gcode").string();
    auto run = [&] {
        request.config = created.config->snapshot();
        const auto result = library->slice(request);
        std::string diagnostics;
        for (const auto& d : result.diagnostics) diagnostics += d.message + "\n";
        INFO(diagnostics);
        REQUIRE(result.success);
        std::ifstream input(request.output_gcode_path);
        std::string text((std::istreambuf_iterator<char>(input)), {});
        CHECK(text.find("filament_exclude_from_wipe_tower") == std::string::npos);
        // Only the existing timestamp is volatile; retain every other byte.
        return std::regex_replace(text, std::regex("on [0-9]{4}-[0-9]{2}-[0-9]{2} at [0-9:]+"), "on TIMESTAMP");
    };
    const auto inherited = run();
    REQUIRE(created.config->set("filament_exclude_from_wipe_tower", "0,0,0,0,0,0").success);
    CHECK(run() == inherited);
}


TEST_CASE("wipe tower exclusion handles unused and unavailable participants", "[libslicer_api][wipe-tower-exclusion][boundary]")
{
    libslicer::LibraryOptions options;
    options.resource_directory = LIBSLICER_TEST_RESOURCE_DIR; options.vendors = {"P3"};
    auto library = libslicer::Library::open(options); REQUIRE(library);
    libslicer::ConfigSelection selection;
    selection.machine_model_id = "P3"; selection.machine_variant_id = "0.4";
    selection.process_preset_id = "0.20mm Standard @P3";
    selection.filament_preset_ids.assign(6, "Generic PLA @P3");
    selection.filament_preset_ids[5] = "CFSYS CCF";
    auto created = library->create_config(selection); REQUIRE(created.success);
    REQUIRE(created.config->apply_patch({{"enable_prime_tower", "1"},
        {"filament_colour", "#E02222;#F6F6F3;#3F3F3F;#A0A0A0;#A0A0A0;#FF0080"}}).success);
    libslicer::SliceObjectInput object;
    object.name = "unused-fiber-slot";
    libslicer::SliceVolumeInput v;
    v.vertices = {{100,100,0},{120,100,0},{120,120,0},{100,120,0},
                  {100,100,1},{120,100,1},{120,120,1},{100,120,1}};
    v.triangles = {{0,2,1},{0,3,2},{4,5,6},{4,6,7},{0,1,5},{0,5,4},
                   {1,2,6},{1,6,5},{2,3,7},{2,7,6},{3,0,4},{3,4,7}};
    object.volumes = {v};
    libslicer::SliceRequest request;
    request.objects = {object}; request.config = created.config->snapshot();
    request.output_gcode_path = (std::filesystem::temp_directory_path()/"libslicer-unused-fiber.gcode").string();
    const auto unused = library->slice(request);
    std::string diagnostics;
    for (const auto& d : unused.diagnostics) diagnostics += d.message + "\n";
    INFO(diagnostics);
    REQUIRE(unused.success); REQUIRE(unused.preview);
    for (const auto& segment : unused.preview->segments) {
        CHECK(segment.extrusion_role != libslicer::ToolpathExtrusionRole::WipeTower);
        if (segment.motion == libslicer::ToolpathMotionKind::Extrusion) CHECK(segment.tool_id == 0);
    }
    REQUIRE(created.config->set("wipe_tower_filament", "6").success);
    request.config = created.config->snapshot();
    const auto invalid_tower = library->slice(request);
    CHECK_FALSE(invalid_tower.success);
    CHECK(std::any_of(invalid_tower.diagnostics.begin(), invalid_tower.diagnostics.end(), [](const auto& d) {
        return d.message.find("excluded from the wipe tower") != std::string::npos;
    }));
    REQUIRE(created.config->set("wipe_tower_filament", "0").success);
    REQUIRE(created.config->set("filament_exclude_from_wipe_tower", "1,1,1,1,1,1").success);
    request.config = created.config->snapshot();
    const auto none = library->slice(request);
    CHECK_FALSE(none.success);
    CHECK(std::any_of(none.diagnostics.begin(), none.diagnostics.end(), [](const auto& d) {
        return d.message.find("no participating material") != std::string::npos;
    }));
}


namespace {
std::filesystem::path project_candidate_fixture(const libslicer::ConfigSnapshot& config, bool foreign, const libslicer::ProjectSettings& overrides={}, std::string application="BambuStudio-2.0.0", bool mandatory_extension=false) {
    nlohmann::json settings=nlohmann::json::object();settings["name"]="project_settings";
    for(const auto& v:config.values())settings[v.first]=v.second;
    if(foreign){settings["printer_model"]="Independent Project Printer";settings["printer_settings_id"]="Independent Project Printer 0.4";settings["inherits_group"]="";}
    for(const auto& value:overrides)settings[value.first]=value.second;
    const auto path=std::filesystem::temp_directory_path()/(foreign?"libslicer-project-candidate-foreign.3mf":"libslicer-project-candidate.3mf");
    std::string mesh=R"xml(<?xml version="1.0"?><model unit="millimeter" xmlns="http://schemas.microsoft.com/3dmanufacturing/core/2015/02"><metadata name="Application">BambuStudio-2.0.0</metadata><metadata name="BambuStudio:3mfVersion">1</metadata><resources><object id="1" type="model"><mesh><vertices><vertex x="0" y="0" z="0"/><vertex x="10" y="0" z="0"/><vertex x="0" y="10" z="0"/><vertex x="0" y="0" z="10"/></vertices><triangles><triangle v1="0" v2="2" v3="1"/><triangle v1="0" v2="1" v3="3"/><triangle v1="0" v2="3" v3="2"/><triangle v1="1" v2="2" v3="3"/></triangles></mesh></object></resources><build><item objectid="1" transform="1 0 0 0 1 0 0 0 1 50 50 0"/></build></model>)xml";
    mesh.replace(mesh.find("BambuStudio-2.0.0"),std::string("BambuStudio-2.0.0").size(),application);
    if(mandatory_extension)mesh.insert(mesh.find(" unit=")," xmlns:unsupported=\"https://example.test/unsupported\" requiredextensions=\"unsupported\"");
    const std::string relationships=R"xml(<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships"><Relationship Target="/3D/3dmodel.model" Id="rel0" Type="http://schemas.microsoft.com/3dmanufacturing/2013/01/3dmodel"/></Relationships>)xml";
    const std::string content=R"xml(<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types"><Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/><Default Extension="model" ContentType="application/vnd.ms-package.3dmanufacturing-3dmodel+xml"/></Types>)xml";
    const auto text=settings.dump();mz_zip_archive archive{};REQUIRE(mz_zip_writer_init_file(&archive,path.string().c_str(),0));
    for(const auto& entry:std::vector<std::pair<std::string,std::string>>{{"_rels/.rels",relationships},{"[Content_Types].xml",content},{"3D/3dmodel.model",mesh},{"Metadata/project_settings.config",text}})
        REQUIRE(mz_zip_writer_add_mem(&archive,entry.first.c_str(),entry.second.data(),entry.second.size(),MZ_DEFAULT_COMPRESSION));
    REQUIRE(mz_zip_writer_finalize_archive(&archive));REQUIRE(mz_zip_writer_end(&archive));return path;
}
std::unique_ptr<libslicer::Library> project_test_library() {
    libslicer::LibraryOptions options;options.resource_directory=LIBSLICER_TEST_RESOURCE_DIR;options.vendors={"Flashforge"};
    auto library=libslicer::Library::open(options);REQUIRE(library);
    libslicer::ConfigSelection selection;selection.machine_model_id="Flashforge AD5X";selection.machine_variant_id="0.4";
    const auto activated=library->activate_config(selection);for(const auto& d:activated.diagnostics)INFO(d.message);REQUIRE(activated.success);return library;
}
}
TEST_CASE("installed material capacity is independent of the current palette", "[libslicer_api][configuration-machine-capacity]") {
    auto library=project_test_library();
    const auto& models=library->machine_models();
    const auto model=std::find_if(models.begin(),models.end(),[](const auto& m){return m.id=="Flashforge AD5X";});
    REQUIRE(model!=models.end());
    const auto catalog=std::find_if(model->variants.begin(),model->variants.end(),[](const auto& v){return v.id=="0.4";});
    REQUIRE(catalog!=model->variants.end());REQUIRE(catalog->max_filament_slots>1);
    const auto check=[&](std::size_t count){
        const auto machine=library->active_project_machine();REQUIRE(machine);CHECK_FALSE(machine->project_local);
        CHECK(machine->model.id==model->id);CHECK(machine->variant.id==catalog->id);
        CHECK(machine->variant.max_filament_slots==catalog->max_filament_slots);
        CHECK(machine->variant.physical_tool_count==catalog->physical_tool_count);
        CHECK(machine->variant.variable_filament_slots==catalog->variable_filament_slots);
        CHECK(machine->variant.filament_slots_bound_to_physical_tools==catalog->filament_slots_bound_to_physical_tools);
        CHECK(machine->variant.toolhead_filament_capacity==catalog->toolhead_filament_capacity);
        REQUIRE(library->active_config());CHECK(library->active_config()->filament_slots.size()==count);
    };
    check(1);
    const auto source=project_candidate_fixture(*library->active_config_snapshot(),false);
    const auto candidate=library->read_project({source.string(),libslicer::ProjectReadMode::Project});
    for(const auto& d:candidate.diagnostics)INFO(d.code<<": "<<d.message);
    REQUIRE(candidate.success);CHECK_FALSE(candidate.configuration.machine().project_local);
    CHECK(candidate.configuration.machine().variant.max_filament_slots==catalog->max_filament_slots);
    CHECK(candidate.configuration.view().filament_slots.size()==1);check(1);
    std::filesystem::remove(source);
    REQUIRE(library->set_active_filament_color(0,{0x12,0x34,0x56,0xff}).success);check(1);
    REQUIRE(library->set_active_config_value("layer_height","0.16").success);check(1);
    const auto one=library->capture_configuration();
    REQUIRE(library->resize_active_filament_slots(catalog->max_filament_slots).success);check(catalog->max_filament_slots);
    const auto before=*library->active_config_snapshot();const auto revision=library->configuration_revision();
    CHECK_FALSE(library->resize_active_filament_slots(catalog->max_filament_slots+1).success);
    CHECK(library->configuration_revision()==revision);CHECK(library->active_config_snapshot()->values()==before.values());
    REQUIRE(library->resize_active_filament_slots(1).success);check(1);
    REQUIRE(library->resize_active_filament_slots(2).success);
    REQUIRE(library->install_configuration(one,library->configuration_revision()).success);check(1);
}
TEST_CASE("known project machine overrides retain their effective capabilities", "[libslicer_api][project-candidate][configuration-machine-capacity]") {
    const bool two_tools=GENERATE(false,true);CAPTURE(two_tools);
    auto library=project_test_library();
    if(two_tools)REQUIRE(library->resize_active_filament_slots(2).success);
    libslicer::ProjectSettings overrides={{"printable_area","0x0,190x0,190x170,0x170"},{"printable_height","210"},
        {"nozzle_diameter",two_tools?"0.6,0.6":"0.6"},{"filament_slots_bound_to_physical_tools","0"},
        {"toolhead_filament_capacity",two_tools?"":"2"},{"filament_map",two_tools?"1,2":"1"},
        {"filament_map_mode","Manual"},{"physical_extruder_map",two_tools?"0,1":"0"},
        {"toolhead_process_capabilities",two_tools?"thermoplastic;thermoplastic":"thermoplastic"}};
    // A known-printer project must declare its printer edits as dirty, just as
    // native Orca does; otherwise native preset reconciliation restores them.
    std::string dirty(two_tools?"\"\";\"\";\"\";\"":"\"\";\"\";\"");
    for(const auto& entry:overrides){if(dirty.back()!='"')dirty+=';';dirty+=entry.first;}
    dirty+='"';overrides.emplace_back("different_settings_to_system",std::move(dirty));
    const auto source=project_candidate_fixture(*library->active_config_snapshot(),false,overrides);
    const auto candidate=library->read_project({source.string(),libslicer::ProjectReadMode::Project});
    for(const auto& d:candidate.diagnostics)INFO(d.code<<": "<<d.message);
    REQUIRE(candidate.success);REQUIRE(candidate.configuration.valid());
    const auto check=[&](const libslicer::ProjectMachine& machine){
        CHECK_FALSE(machine.project_local);CHECK(machine.model.id=="Flashforge AD5X");
        CHECK(machine.variant.nozzle_diameter==Catch::Approx(0.6));CHECK(machine.variant.printable_width==190);
        CHECK(machine.variant.printable_depth==170);CHECK(machine.variant.printable_height==210);
        CHECK(machine.variant.physical_tool_count==(two_tools?2:1));CHECK(machine.variant.max_filament_slots==2);
        CHECK(machine.variant.toolhead_filament_capacity==(two_tools?std::vector<int>{}:std::vector<int>{2}));
    };
    check(candidate.configuration.machine());
    REQUIRE(library->install_configuration(candidate.configuration,library->configuration_revision()).success);
    REQUIRE(library->set_active_config_value("layer_height","0.16").success);
    REQUIRE(library->active_project_machine());check(*library->active_project_machine());
    const auto before=*library->active_config_snapshot();const auto revision=library->configuration_revision();
    CHECK_FALSE(library->resize_active_filament_slots(3).success);
    CHECK(library->active_config_snapshot()->values()==before.values());CHECK(library->configuration_revision()==revision);
    if(!two_tools){
        REQUIRE(library->add_active_filament(0).success);check(*library->active_project_machine());
        CHECK(library->active_config()->filament_slots.size()==2);CHECK_FALSE(library->add_active_filament(0).success);
    }
    std::filesystem::remove(source);
}
TEST_CASE("project preparation is readonly and configuration installation is explicit", "[libslicer_api][project-candidate]") {
    auto library=project_test_library();const auto before=*library->active_config_snapshot();const auto revision=library->active_config()->revision;
    const auto source=project_candidate_fixture(before,false);
    libslicer::ProjectReadRequest request{source.string(),libslicer::ProjectReadMode::Project};
    const auto candidate=library->read_project(request);std::ostringstream diagnostics;for(const auto& d:candidate.diagnostics)diagnostics<<d.code<<": "<<d.message<<"\n";INFO(diagnostics.str());REQUIRE(candidate.success);REQUIRE(candidate.scene);REQUIRE(candidate.configuration.valid());
    CHECK(library->active_config_snapshot()->values()==before.values());CHECK(library->active_config()->revision==revision);
    std::filesystem::remove(source); // Prepared data must remain usable without the source file.
    CHECK(candidate.scene->objects.front().parts.front().mesh->triangles.size()==4);
    CHECK_FALSE(library->install_configuration(candidate.configuration,revision+1).success);
    const auto installed=library->install_configuration(candidate.configuration,revision);REQUIRE(installed.success);CHECK(installed.revision>revision);
    const auto restored=library->install_configuration(installed.previous,installed.revision);REQUIRE(restored.success);CHECK(restored.revision>installed.revision);
    CHECK(library->active_config_snapshot()->values()==before.values());
    CHECK_FALSE(candidate.configuration.snapshot().values().empty());
}
TEST_CASE("empty configuration restore preserves the compare-and-swap revision", "[libslicer_api][project-candidate]") {
    auto source_library=project_test_library();
    const auto source=project_candidate_fixture(*source_library->active_config_snapshot(),false);
    libslicer::LibraryOptions options;options.resource_directory=LIBSLICER_TEST_RESOURCE_DIR;options.vendors={"Flashforge"};
    auto library=libslicer::Library::open(options);REQUIRE(library);REQUIRE_FALSE(library->active_config());
    const auto empty=library->capture_configuration();const auto initial=library->configuration_revision();
    const auto candidate=library->read_project({source.string(),libslicer::ProjectReadMode::Project});REQUIRE(candidate.success);
    const auto installed=library->install_configuration(candidate.configuration,initial);REQUIRE(installed.success);
    const auto restored=library->install_configuration(empty,installed.revision);REQUIRE(restored.success);
    CHECK_FALSE(library->active_config());CHECK(library->configuration_revision()==restored.revision);
    CHECK_FALSE(library->install_configuration(candidate.configuration,initial).success);
    const auto reinstalled=library->install_configuration(candidate.configuration,library->configuration_revision());REQUIRE(reinstalled.success);
    CHECK(reinstalled.revision>restored.revision);CHECK(library->active_config()->revision==reinstalled.revision);
    std::filesystem::remove(source);
}
TEST_CASE("geometry mapping and cancellation preserve active state", "[libslicer_api][project-candidate]") {
    auto library=project_test_library();const auto before=*library->active_config_snapshot();const auto revision=library->active_config()->revision;
    const auto source=project_candidate_fixture(before,false);libslicer::ProjectReadRequest request{source.string(),libslicer::ProjectReadMode::Geometry};
    libslicer::SliceCallbacks cancelled;cancelled.is_cancelled=[] {return true;};
    const auto aborted=library->read_project(request,cancelled);CHECK(aborted.cancelled);CHECK_FALSE(aborted.success);
    const auto candidate=library->read_project(request);for(const auto& d:candidate.diagnostics)INFO(d.message);REQUIRE(candidate.success);CHECK_FALSE(candidate.configuration.valid());
    std::vector<unsigned> mapping(candidate.scene->source_filaments.size(),1);
    const auto mapped=library->remap_project_materials(candidate,mapping,before);REQUIRE(mapped.success);
    CHECK(mapped.scene->objects.front().parts.front().mesh==candidate.scene->objects.front().parts.front().mesh);
    mapping.front()=999;CHECK_FALSE(library->remap_project_materials(candidate,mapping,before).success);
    bool cancel_at_finish=false;libslicer::SliceCallbacks late_cancel;
    late_cancel.progress=[&](float progress,std::string_view){if(progress>=1.0f)cancel_at_finish=true;};late_cancel.is_cancelled=[&]{return cancel_at_finish;};
    const auto cancelled_candidate=library->read_project(request,late_cancel);CHECK(cancelled_candidate.cancelled);CHECK_FALSE(cancelled_candidate.success);CHECK_FALSE(cancelled_candidate.scene);
    CHECK(library->active_config_snapshot()->values()==before.values());CHECK(library->active_config()->revision==revision);std::filesystem::remove(source);
}
TEST_CASE("foreign project printer supports editing without installing the model", "[libslicer_api][project-candidate]") {
    auto library=project_test_library();const auto before=*library->active_config_snapshot();const auto source=project_candidate_fixture(before,true);
    const auto candidate=library->read_project({source.string(),libslicer::ProjectReadMode::Project});std::ostringstream diagnostics;for(const auto& d:candidate.diagnostics)diagnostics<<d.code<<": "<<d.message<<"\n";INFO(diagnostics.str());REQUIRE(candidate.success);REQUIRE(candidate.configuration.machine().project_local);
    auto installed=library->install_configuration(candidate.configuration,library->active_config()->revision);REQUIRE(installed.success);
    CHECK(library->active_project_machine()->model.id=="Independent Project Printer");
    REQUIRE(library->set_active_config_value("layer_height","0.16").success);CHECK(library->active_config_snapshot()->value("layer_height")=="0.16");
    // Native update_compatible(Never) keeps the selected filament even when
    // the foreign printer has no compatible installed alternatives.
    const auto view=*library->active_config();REQUIRE_FALSE(view.filament_slots.empty());
    CHECK(view.filament_slots.front().preset_id==candidate.configuration.view().filament_slots.front().preset_id);
    CHECK(view.selection.filament_preset_ids==candidate.configuration.view().selection.filament_preset_ids);
    CHECK(library->active_project_machine()->model.id=="Independent Project Printer");CHECK(library->active_config_snapshot()->value("layer_height")=="0.16");
    CHECK(std::none_of(library->machine_models().begin(),library->machine_models().end(),[](const auto& m){return m.id=="Independent Project Printer";}));std::filesystem::remove(source);
}
TEST_CASE("project codecs separate material ownership and topology dependent annotations", "[libslicer_api][project-candidate]") {
    libslicer::ProjectObject object;object.default_filament_slot=2;object.settings={{"extruder","2"},{"layer_height","0.12"}};
    object.layer_ranges.push_back({1,2,{{"extruder","2"},{"layer_height","0.1"}}});
    const auto object_json=nlohmann::json::parse(libslicer::encode_project_object_semantics(object));CHECK_FALSE(object_json["settings"].contains("extruder"));CHECK(object_json["settings"]["layer_height"]=="0.12");CHECK(object_json["layer_ranges"][0]["settings"]["extruder"]=="2");
    libslicer::ProjectPart part;part.settings={{"extruder","3"},{"sparse_infill_density","25%"}};part.support_facets.roots={{0,0}};part.support_facets.bitstream={0,0,1,0};
    const auto part_json=nlohmann::json::parse(libslicer::encode_project_part_semantics(part));CHECK_FALSE(part_json["settings"].contains("extruder"));CHECK_FALSE(part_json.contains("support_facets"));
    const auto annotations=nlohmann::json::parse(libslicer::encode_project_part_annotations(part));CHECK(annotations["scope"]=="part_annotations");CHECK_FALSE(annotations.contains("settings"));CHECK(annotations["support_facets"]["bitstream"].size()==4);
}


TEST_CASE("native project semantics preserve mirrored painted volume frames", "[libslicer_api][project-candidate][slice]") {
    auto library=project_test_library();
    REQUIRE(library->apply_active_config_patch({{"layer_height","0.2"},{"initial_layer_print_height","0.2"},{"sparse_infill_density","100%"},{"enable_prime_tower","0"}}).success);
    const auto config=*library->active_config_snapshot();
    libslicer::SliceVolumeInput body;
    body.vertices={{0,0,0},{10,0,0},{10,10,0},{0,10,0},{0,0,10},{10,0,10},{10,10,10},{0,10,10}};
    body.triangles={{0,2,1},{0,3,2},{4,5,6},{4,6,7},{0,1,5},{0,5,4},{1,2,6},{1,6,5},{2,3,7},{2,7,6},{3,0,4},{3,4,7}};
    body.facet_labels.roots={{2,0},{3,4}};body.facet_labels.bitstream={0,0,1,0,0,0,1,0};
    body.local_transform={-1,0,0,10,0,1,0,0,0,0,1,0,0,0,0,1};
    body.inherit_object_filament=true;
    libslicer::SliceObjectInput object;object.name="mirrored-project-cube";object.default_filament_slot=1;object.volumes={body};object.transform[3]=50;object.transform[7]=50;
    libslicer::ProjectPlate plate;plate.custom_gcode_mode=1;plate.custom_gcodes.push_back({1.0,4,0,"",";PROJECT_CUSTOM_GCODE"});
    libslicer::SliceRequest request;request.objects={object};request.config=config;request.generate_preview=false;request.center_on_build_plate=false;request.project_semantics=libslicer::encode_project_plate_semantics(plate);
    auto baseline=library->slice(request);for(const auto& d:baseline.diagnostics)INFO(d.message);REQUIRE(baseline.success);CHECK(baseline.summary.layer_count>0);
    std::ifstream input(baseline.output.path);const std::string gcode((std::istreambuf_iterator<char>(input)),{});CHECK(gcode.find(";PROJECT_CUSTOM_GCODE")!=std::string::npos);
    auto modifier=body;modifier.facet_labels={};modifier.inherit_object_filament=true;modifier.role=libslicer::SliceVolumeRole::NegativeVolume;
    modifier.local_transform={0.6,0,0,2,0,0.6,0,2,0,0,1,0,0,0,0,1};
    request.objects.front().volumes.push_back(modifier);
    auto negative=library->slice(request);for(const auto& d:negative.diagnostics)INFO(d.message);REQUIRE(negative.success);CHECK(negative.summary.filament_used_mm<baseline.summary.filament_used_mm);
    request.objects.front().volumes.pop_back();
    libslicer::ProjectObject semantics;semantics.layer_height_profile={0,0.2,1,0.1,10,0.1};request.objects.front().project_semantics=libslicer::encode_project_object_semantics(semantics);
    auto variable=library->slice(request);for(const auto& d:variable.diagnostics)INFO(d.message);REQUIRE(variable.success);CHECK(variable.summary.layer_count>baseline.summary.layer_count);
    request.objects.front().volumes.front().facet_labels.bitstream={0,0,1,1}; // Extension marker without its following nibble.
    CHECK_FALSE(library->slice(request).success);
    for(const auto* result:{&baseline,&negative,&variable})if(result->output.ownership==libslicer::OutputArtifactOwnership::LibraryTemporary)std::filesystem::remove(result->output.path);
}

TEST_CASE("native project plate index selects its own wipe tower coordinates", "[libslicer_api][project-candidate][slice]") {
    auto library=project_test_library();
    REQUIRE(library->resize_active_filament_slots(2).success);
    REQUIRE(library->apply_active_config_patch({{"enable_prime_tower","1"},{"wipe_tower_x","15,140"},{"wipe_tower_y","15,25"},{"prime_tower_width","30"},{"wipe_tower_rotation_angle","0"}}).success);
    libslicer::SliceRequest request;request.config=*library->active_config_snapshot();request.center_on_build_plate=false;
    for(unsigned slot=1;slot<=2;++slot){
        libslicer::SliceObjectInput object;object.name="plate-tower-coupon";
        const float x=60.f+25.f*(slot-1);libslicer::SliceVolumeInput volume;
        volume.vertices={{x,80,0},{x+15,80,0},{x+15,95,0},{x,95,0},{x,80,1},{x+15,80,1},{x+15,95,1},{x,95,1}};
        volume.triangles={{0,2,1},{0,3,2},{4,5,6},{4,6,7},{0,1,5},{0,5,4},{1,2,6},{1,6,5},{2,3,7},{2,7,6},{3,0,4},{3,4,7}};
        volume.default_filament_slot=int(slot);object.volumes={volume};request.objects.push_back(object);
    }
    double minimum_x[2];
    for(int index=0;index<2;++index){
        libslicer::ProjectPlate plate;plate.native_plate_index=index;request.project_semantics=libslicer::encode_project_plate_semantics(plate);
        const auto result=library->slice(request);for(const auto& d:result.diagnostics)INFO(d.message);REQUIRE(result.success);REQUIRE(result.preview);
        minimum_x[index]=std::numeric_limits<double>::max();
        for(const auto& segment:result.preview->segments)if(segment.extrusion_role==libslicer::ToolpathExtrusionRole::WipeTower&&segment.motion==libslicer::ToolpathMotionKind::Extrusion)
            minimum_x[index]=std::min(minimum_x[index],double(std::min(segment.start_mm.x,segment.end_mm.x)));
        REQUIRE(minimum_x[index]!=std::numeric_limits<double>::max());
        if(result.output.ownership==libslicer::OutputArtifactOwnership::LibraryTemporary)std::filesystem::remove(result.output.path);
    }
    CHECK(minimum_x[1]-minimum_x[0]==Catch::Approx(125.0).margin(0.1));
}

TEST_CASE("legacy project unknown settings are reported without an import gate", "[libslicer_api][project-candidate]") {
    auto library=project_test_library();
    const auto before=*library->active_config_snapshot();
    const std::string source=std::string(LIBSLICER_TEST_RESOURCE_DIR)+"/calib/pressure_advance/pa_pattern.3mf";
    const auto candidate=library->read_project({source,libslicer::ProjectReadMode::Project});
    std::ostringstream diagnostics;for(const auto& d:candidate.diagnostics)diagnostics<<d.code<<": "<<d.message<<"\n";INFO(diagnostics.str());
    REQUIRE(candidate.success);CHECK(candidate.configuration.valid());CHECK(candidate.required_decisions.empty());
    CHECK(std::any_of(candidate.diagnostics.begin(),candidate.diagnostics.end(),[](const auto& d){return d.code=="project.unknown_setting_ignored"&&d.context=="end_print_exhaust_fan_speed";}));
    CHECK(std::any_of(candidate.diagnostics.begin(),candidate.diagnostics.end(),[](const auto& d){return d.code=="project.obsolete_setting"&&d.context=="bed_temperature_difference";}));
    const auto geometry=library->read_project({source,libslicer::ProjectReadMode::Geometry});REQUIRE(geometry.success);REQUIRE(geometry.scene);CHECK_FALSE(geometry.scene->objects.empty());
    CHECK(library->active_config_snapshot()->values()==before.values());
}

TEST_CASE("unknown settings do not require opt in and retain complete diagnostics", "[libslicer_api][project-candidate]") {
    auto library=project_test_library();const auto before=*library->active_config_snapshot();const auto revision=library->configuration_revision();
    libslicer::ProjectReadRequest request{std::string(LIBSLICER_TEST_RESOURCE_DIR)+"/calib/pressure_advance/pa_pattern.3mf",libslicer::ProjectReadMode::Project};
    const auto strict=library->read_project(request);REQUIRE(strict.success);
    CHECK(strict.required_decisions.empty());
    request.accept_unknown_settings=true;const auto accepted=library->read_project(request);
    std::ostringstream diagnostics;for(const auto& d:accepted.diagnostics)diagnostics<<d.code<<": "<<d.message<<"\n";INFO(diagnostics.str());
    REQUIRE(accepted.success);CHECK(accepted.required_decisions.empty());
    CHECK(accepted.ignored_settings==strict.ignored_settings);
    CHECK(accepted.configuration.snapshot().values()==strict.configuration.snapshot().values());
    const std::vector<std::string> ignored={"chamber_temp_control","end_print_exhaust_fan_speed","end_print_exhaust_fan_time"};
    CHECK(accepted.ignored_settings==ignored);CHECK(accepted.configuration.valid());
    REQUIRE(accepted.scene->plates.front().custom_gcodes.size()==4);
    // Original PA XML stores literal attribute newlines. Orca's RapidXML
    // reader preserves them; an XML-normalizing adapter would turn the entire
    // custom program into a comment. Counts come from the fixed raw fixture.
    const std::array<std::size_t,4> custom_sizes{17813,12396,7400,7388};
    const std::array<std::size_t,4> custom_newlines{953,602,380,378};
    for(std::size_t i=0;i<custom_sizes.size();++i){
        const auto& extra=accepted.scene->plates.front().custom_gcodes[i].extra;
        CHECK(extra.size()==custom_sizes[i]);CHECK(std::size_t(std::count(extra.begin(),extra.end(),'\n'))==custom_newlines[i]);
    }
    CHECK(accepted.scene->plates.front().custom_gcodes.front().extra.find("\nG1 E-.8 F1800\n")!=std::string::npos);
    CHECK(accepted.scene->plates.front().custom_gcodes.front().type==4);
    CHECK(accepted.scene->plates.front().custom_gcodes.front().filament_slot==-858993460);
    for(const auto& key:ignored){CHECK_FALSE(accepted.configuration.snapshot().value(key));CHECK(std::any_of(accepted.diagnostics.begin(),accepted.diagnostics.end(),[&](const auto& d){return d.code=="project.unknown_setting_ignored"&&d.severity!="error"&&d.context==key;}));}
    bool finish=false;libslicer::SliceCallbacks cancel;cancel.progress=[&](float p,std::string_view){finish=p>=1.f;};cancel.is_cancelled=[&]{return finish;};
    const auto aborted=library->read_project(request,cancel);CHECK(aborted.cancelled);CHECK_FALSE(aborted.success);CHECK(aborted.ignored_settings.empty());
    const auto invalid=project_candidate_fixture(before,false,{{"future_setting","1"},{"nozzle_diameter","-1"},{"different_settings_to_system","\"\";\"\";\"nozzle_diameter\""}});
    request.path=invalid.string();const auto editable=library->read_project(request);REQUIRE(editable.success);
    CHECK(editable.configuration.snapshot().value("nozzle_diameter")=="-1");
    CHECK(std::any_of(editable.diagnostics.begin(),editable.diagnostics.end(),[](const auto& d){return d.code=="project.invalid_configuration";}));
    const auto installed=library->install_configuration(editable.configuration,revision);REQUIRE(installed.success);
    const auto normal=project_candidate_fixture(before,false);
    CHECK(library->read_project({normal.string(),libslicer::ProjectReadMode::Geometry}).success);
    CHECK(library->read_project({normal.string(),libslicer::ProjectReadMode::Project}).success);
    REQUIRE(library->install_configuration(installed.previous,installed.revision).success);
    const auto structural=project_candidate_fixture(before,false,{{"future_setting","1"}},"BambuStudio-2.0.0",true);
    request.path=structural.string();const auto extension=library->read_project(request);REQUIRE(extension.success);
    CHECK(extension.required_decisions.empty()); // Native reader accepts this extension declaration.
    CHECK(library->configuration_revision()>revision);CHECK(library->active_config_snapshot()->values()==before.values());std::filesystem::remove(structural);
}

TEST_CASE("import preserves native custom tool references for later validation", "[libslicer_api][project-candidate]") {
    const int type=GENERATE(0,2);const int slot=GENERATE(-1,999);CAPTURE(type,slot);
    auto library=project_test_library();const auto before=*library->active_config_snapshot();const auto revision=library->configuration_revision();
    const auto source=project_candidate_fixture(before,false,{{"future_setting","1"}});
    const std::string custom="<custom_gcodes_per_layer><plate><plate_info id=\"1\"/><layer top_z=\"0.2\" type=\""+std::to_string(type)+"\" extruder=\""+std::to_string(slot)+"\" color=\"#FF0000\" extra=\"\"/><mode value=\"MultiExtruder\"/></plate></custom_gcodes_per_layer>";
    REQUIRE(mz_zip_add_mem_to_archive_file_in_place(source.string().c_str(),"Metadata/custom_gcode_per_layer.xml",custom.data(),custom.size(),nullptr,0,MZ_DEFAULT_COMPRESSION));
    libslicer::ProjectReadRequest request{source.string(),libslicer::ProjectReadMode::Project};request.accept_unknown_settings=true;
    const auto candidate=library->read_project(request);REQUIRE(candidate.success);REQUIRE(candidate.configuration.valid());
    REQUIRE(candidate.scene->plates.front().custom_gcodes.size()==1);
    CHECK(candidate.scene->plates.front().custom_gcodes.front().filament_slot==slot);
    CHECK(library->configuration_revision()==revision);CHECK(library->active_config_snapshot()->values()==before.values());std::filesystem::remove(source);
}

TEST_CASE("legacy rotation supports yes no cancellation and one file read", "[libslicer_api][project-candidate][project-orca-parity]") {
    auto library=project_test_library();const auto before=*library->active_config_snapshot();
    const libslicer::ProjectSettings rotation_settings={{"future_setting","1"},{"sparse_infill_pattern","gyroid"},{"sparse_infill_rotate_template","1"},
        {"different_settings_to_system","\"sparse_infill_pattern;sparse_infill_rotate_template\";\"\";\"\""}};
    const auto source=project_candidate_fixture(before,false,rotation_settings,"OrcaSlicer-2.3.1-alpha");
    libslicer::ProjectReadRequest request{source.string(),libslicer::ProjectReadMode::Project};
    const auto strict=library->read_project(request);REQUIRE_FALSE(strict.success);
    CHECK(strict.required_decisions==std::vector<std::string>{"legacy_infill_rotation_fix"});
    request.accept_unknown_settings=true;const auto unknown=library->read_project(request);CHECK_FALSE(unknown.success);CHECK(unknown.ignored_settings.empty());CHECK(unknown.required_decisions==std::vector<std::string>{"legacy_infill_rotation_fix"});
    request.accept_unknown_settings=false;request.accept_legacy_infill_rotation_fix=true;
    const auto legacy=library->read_project(request);REQUIRE(legacy.success);CHECK(legacy.required_decisions.empty());
    request.accept_unknown_settings=true;const auto both=library->read_project(request);for(const auto& d:both.diagnostics)INFO(d.message);REQUIRE(both.success);
    CHECK(both.ignored_settings==std::vector<std::string>{"future_setting"});CHECK(both.configuration.snapshot().value("sparse_infill_rotate_template")=="");
    int called=0;
    // A callback No overrides the transitional true bool. Removing the source
    // at the decision proves that this workflow never opens the archive again.
    const auto kept=library->read_project(request,{},[&](const libslicer::ProjectDecision& decision) {
        ++called; CHECK(decision.kind==libslicer::ProjectDecisionKind::LegacyInfillRotation);
        std::filesystem::remove(source); return libslicer::ProjectDecisionAnswer::No;
    });
    REQUIRE(kept.success);CHECK(called==1);CHECK(kept.configuration.snapshot().value("sparse_infill_rotate_template")=="1");
    project_candidate_fixture(before,false,rotation_settings,"OrcaSlicer-2.3.1-alpha");
    const auto cancelled=library->read_project(request,{},[](const auto&) { return libslicer::ProjectDecisionAnswer::Cancel; });
    CHECK(cancelled.cancelled);CHECK_FALSE(cancelled.success);CHECK_FALSE(cancelled.scene);CHECK_FALSE(cancelled.configuration.valid());
    const auto failed=library->read_project(request,{},[](const auto&)->libslicer::ProjectDecisionAnswer { throw std::runtime_error("decision failed"); });
    CHECK_FALSE(failed.success);CHECK_FALSE(failed.cancelled);CHECK_FALSE(failed.scene);
    const auto nonstandard=library->read_project(request,{},[](const auto&)->libslicer::ProjectDecisionAnswer { throw 7; });
    CHECK_FALSE(nonstandard.success);CHECK_FALSE(nonstandard.cancelled);CHECK_FALSE(nonstandard.scene);CHECK_FALSE(nonstandard.configuration.valid());
    CHECK(library->active_config_snapshot()->values()==before.values());std::filesystem::remove(source);
}

TEST_CASE("external project fixture preserves complete native scene semantics", "[libslicer_api][project-external]") {
    const char* source=std::getenv("LIBSLICER_PROJECT_TEST_INPUT");
    if(!source)SKIP("Set LIBSLICER_PROJECT_TEST_INPUT to the independently generated complete-project fixture");
    auto library=project_test_library();const auto before=*library->active_config_snapshot();
    const auto candidate=library->read_project({source,libslicer::ProjectReadMode::Project});
    std::ostringstream messages;for(const auto& d:candidate.diagnostics)messages<<d.code<<": "<<d.message<<"\n";INFO(messages.str());
    REQUIRE(candidate.success);REQUIRE(candidate.scene);REQUIRE(candidate.configuration.valid());
    CHECK(candidate.configuration.machine().project_local);CHECK(candidate.scene->plates.size()==2);
    REQUIRE_FALSE(candidate.scene->objects.empty());
    std::set<libslicer::ProjectPartRole> roles;std::set<std::string> plate_ids;
    bool object_settings=false,part_settings=false,ranges=false,profile=false,brim=false,mmu=false,support=false,seam=false,fuzzy=false,custom=false;
    std::size_t instances=0;
    for(const auto& object:candidate.scene->objects){
        object_settings|=!object.settings.empty();ranges|=!object.layer_ranges.empty();profile|=!object.layer_height_profile.empty();brim|=!object.brim_points.empty();
        for(const auto& part:object.parts){roles.insert(part.role);part_settings|=!part.settings.empty();mmu|=!part.material_facets.empty();support|=!part.support_facets.empty();seam|=!part.seam_facets.empty();fuzzy|=!part.fuzzy_skin_facets.empty();}
        for(const auto& instance:object.instances){++instances;if(!instance.plate_id.empty())plate_ids.insert(instance.plate_id);}
    }
    for(const auto& plate:candidate.scene->plates)custom|=!plate.custom_gcodes.empty();
    CHECK(roles.size()==5);CHECK(instances>=2);CHECK(plate_ids.size()==2);CHECK(object_settings);CHECK(part_settings);CHECK(ranges);CHECK(profile);CHECK(brim);CHECK(mmu);CHECK(support);CHECK(seam);CHECK(fuzzy);CHECK(custom);
    CHECK(library->active_config_snapshot()->values()==before.values());
}

TEST_CASE("production model references require their declared package relationships", "[libslicer_api][project-candidate][project-relationships]") {
    // Reproduce the production_ext shape without depending on Downloads. The
    // valid archive differs only by the required source model relationship.
    const int relationship=GENERATE(0,1,2,3,4); // missing part, wrong target, valid, unrelated/shadowed namespace
    const auto mode=GENERATE(libslicer::ProjectReadMode::Geometry,libslicer::ProjectReadMode::Project);
    CAPTURE(relationship,mode);
    auto library=project_test_library();const auto before=*library->active_config_snapshot();
    const auto revision=library->configuration_revision();
    const auto source=project_candidate_fixture(before,false);
    const auto configuration=read_zip_entry(source,"Metadata/project_settings.config");
    const auto normal_model=read_zip_entry(source,"3D/3dmodel.model");
    const std::string content=R"xml(<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types"><Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/><Default Extension="model" ContentType="application/vnd.ms-package.3dmanufacturing-3dmodel+xml"/></Types>)xml";
    const std::string root_rels=R"xml(<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships"><Relationship Target="/3D/3dmodel.model" Id="root" Type="http://schemas.microsoft.com/3dmanufacturing/2013/01/3dmodel"/></Relationships>)xml";
    std::string root=R"xml(<model unit="millimeter" xmlns="http://schemas.microsoft.com/3dmanufacturing/core/2015/02" xmlns:p="http://schemas.microsoft.com/3dmanufacturing/production/2015/06" requiredextensions="p"><resources/><build><item objectid="2" p:path="/3D/Objects/sub.model" transform="1 0 0 0 1 0 0 0 1 50 50 0"/></build></model>)xml";
    if(relationship>=3){
        root.assign(normal_model.begin(),normal_model.end());
        root.insert(root.find(" unit="),relationship==3?" xmlns:q=\"https://example.test/optional-metadata\"":" xmlns:q=\"http://schemas.microsoft.com/3dmanufacturing/production/2015/06\"");
        root.insert(root.find(" objectid="),relationship==3?" q:path=\"/unrelated/model.model\"":" xmlns:q=\"https://example.test/optional-metadata\" q:path=\"/unrelated/model.model\"");
    }
    const std::string child=R"xml(<model unit="millimeter" xmlns="http://schemas.microsoft.com/3dmanufacturing/core/2015/02"><resources><object id="1" type="model"><mesh><vertices><vertex x="0" y="0" z="0"/><vertex x="10" y="0" z="0"/><vertex x="0" y="10" z="0"/><vertex x="0" y="0" z="10"/></vertices><triangles><triangle v1="0" v2="2" v3="1"/><triangle v1="0" v2="1" v3="3"/><triangle v1="0" v2="3" v3="2"/><triangle v1="1" v2="2" v3="3"/></triangles></mesh></object><object id="2" type="model"><components><component objectid="1"/></components></object></resources></model>)xml";
    const std::string child_rels=std::string(R"xml(<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships"><Relationship Target=")xml")+(relationship==1?"/3D/Objects/wrong.model":"/3D/Objects/sub.model")+R"xml(" Id="child" Type="http://schemas.microsoft.com/3dmanufacturing/2013/01/3dmodel"/></Relationships>)xml";
    mz_zip_archive archive{};REQUIRE(mz_zip_writer_init_file(&archive,source.string().c_str(),0));
    const auto add=[&](const char* name,const std::string& text){REQUIRE(mz_zip_writer_add_mem(&archive,name,text.data(),text.size(),MZ_DEFAULT_COMPRESSION));};
    add("[Content_Types].xml",content);add("_rels/.rels",root_rels);add("3D/3dmodel.model",root);add("3D/Objects/sub.model",child);
    REQUIRE(mz_zip_writer_add_mem(&archive,"Metadata/project_settings.config",configuration.data(),configuration.size(),MZ_DEFAULT_COMPRESSION));
    if(relationship==1||relationship==2)add("3D/_rels/3dmodel.model.rels",child_rels);
    REQUIRE(mz_zip_writer_finalize_archive(&archive));REQUIRE(mz_zip_writer_end(&archive));
    const auto result=library->read_project({source.string(),mode});
    for(const auto& diagnostic:result.diagnostics)INFO(diagnostic.code<<": "<<diagnostic.message);
    CHECK(library->configuration_revision()==revision);CHECK(library->active_config_snapshot()->values()==before.values());
    if(relationship<2){
        CHECK_FALSE(result.success);CHECK_FALSE(result.cancelled);CHECK_FALSE(result.scene);CHECK_FALSE(result.configuration.valid());
        REQUIRE(result.diagnostics.size()==1);CHECK(result.diagnostics.front().code=="project.read_failed");
    }else{
        REQUIRE(result.success);REQUIRE(result.scene);REQUIRE(result.scene->objects.size()==1);
        const auto& object=result.scene->objects.front();REQUIRE(object.parts.size()==1);REQUIRE(object.instances.size()==1);
        const auto& part=object.parts.front();REQUIRE(part.mesh);CHECK(part.mesh->vertices.size()==4);CHECK(part.mesh->triangles.size()==4);
        std::array<double,3> minimum{1e9,1e9,1e9},maximum{-1e9,-1e9,-1e9};
        for(const auto& vertex:part.mesh->vertices){
            const std::array<double,3> point{vertex.x,vertex.y,vertex.z};std::array<double,3> local{},world{};
            for(std::size_t i=0;i<3;++i){local[i]=part.local_transform[i*4+3];for(std::size_t j=0;j<3;++j)local[i]+=part.local_transform[i*4+j]*point[j];}
            for(std::size_t i=0;i<3;++i){world[i]=object.instances.front().transform[i*4+3];for(std::size_t j=0;j<3;++j)world[i]+=object.instances.front().transform[i*4+j]*local[j];minimum[i]=std::min(minimum[i],world[i]);maximum[i]=std::max(maximum[i],world[i]);}
        }
        if(mode==libslicer::ProjectReadMode::Geometry){CHECK(minimum==std::array<double,3>{105,105,0});CHECK(maximum==std::array<double,3>{115,115,10});}
        else {CHECK(minimum==std::array<double,3>{50,50,0});CHECK(maximum==std::array<double,3>{60,60,10});}
        CHECK(result.configuration.valid()==(mode==libslicer::ProjectReadMode::Project));
    }
    std::filesystem::remove(source);
}

TEST_CASE("geometry default material inheritance follows the explicit source material map", "[libslicer_api][project-material-inherit][slice]") {
    const bool explicit_legacy_zero=GENERATE(false,true);CAPTURE(explicit_legacy_zero);
    auto library=project_test_library();REQUIRE(library->resize_active_filament_slots(1).success);
    REQUIRE(library->apply_active_config_patch({{"enable_prime_tower","0"}}).success);
    const auto source=project_candidate_fixture(*library->active_config_snapshot(),false);
    if(explicit_legacy_zero){
        const std::string metadata=R"xml(<config><object id="1"><metadata key="extruder" value="0"/><part id="1" subtype="normal_part"><metadata key="extruder" value="1"/></part></object></config>)xml";
        REQUIRE(mz_zip_add_mem_to_archive_file_in_place(source.string().c_str(),"Metadata/model_settings.config",metadata.data(),metadata.size(),nullptr,0,MZ_DEFAULT_COMPRESSION));
    }
    const auto geometry=library->read_project({source.string(),libslicer::ProjectReadMode::Geometry});REQUIRE(geometry.success);
    REQUIRE(geometry.scene->source_filaments.size()==1);REQUIRE(geometry.scene->objects.front().default_filament_slot==1);
    REQUIRE_FALSE(geometry.scene->objects.front().parts.front().explicit_filament_binding);
    REQUIRE(library->resize_active_filament_slots(2).success);const auto target=*library->active_config_snapshot();
    const auto mapped=library->remap_project_materials(geometry,{2},target);REQUIRE(mapped.success);
    const auto& object=mapped.scene->objects.front();
    CHECK(object.default_filament_slot==2);
    libslicer::SliceObjectInput input;input.name="mapped-inherited-material";input.default_filament_slot=object.default_filament_slot;input.transform=object.instances.front().transform;
    for(const auto& part:object.parts){
        libslicer::SliceVolumeInput volume;for(const auto& v:part.mesh->vertices)volume.vertices.push_back({v.x,v.y,v.z});
        for(const auto& t:part.mesh->triangles)volume.triangles.push_back({t.vertex_a,t.vertex_b,t.vertex_c});
        volume.inherit_object_filament=!part.explicit_filament_binding;volume.default_filament_slot=part.default_filament_slot;volume.local_transform=part.local_transform;input.volumes.push_back(volume);
    }
    libslicer::SliceRequest request;request.objects={input};request.config=target;request.default_filament_slot=1;request.center_on_build_plate=false;
    const auto sliced=library->slice(request);for(const auto& d:sliced.diagnostics)INFO(d.message);REQUIRE(sliced.success);REQUIRE(sliced.preview);
    std::set<unsigned> used;for(const auto& segment:sliced.preview->segments)if(segment.motion==libslicer::ToolpathMotionKind::Extrusion&&segment.extrusion_role>=libslicer::ToolpathExtrusionRole::InnerWall&&segment.extrusion_role<=libslicer::ToolpathExtrusionRole::GapInfill)used.insert(segment.filament_id);
    CHECK(used==std::set<unsigned>{1}); // Preview IDs are zero based: source material 1 was explicitly mapped to target slot 2.
    if(sliced.output.ownership==libslicer::OutputArtifactOwnership::LibraryTemporary)std::filesystem::remove(sliced.output.path);std::filesystem::remove(source);
}

TEST_CASE("Prusa inherited source material follows an explicit geometry material map", "[libslicer_api][project-material-prusa][slice]") {
    auto library=project_test_library();REQUIRE(library->resize_active_filament_slots(1).success);
    REQUIRE(library->apply_active_config_patch({{"enable_prime_tower","0"}}).success);
    const char* external=std::getenv("LIBSLICER_PRUSA_PROJECT_TEST_INPUT");
    const auto source=external?std::filesystem::path(external):project_candidate_fixture(*library->active_config_snapshot(),false,{},"PrusaSlicer-2.8.0");
    const auto geometry=library->read_project({source.string(),libslicer::ProjectReadMode::Geometry});
    for(const auto& d:geometry.diagnostics)INFO(d.message);
    REQUIRE(geometry.success);REQUIRE(geometry.scene);REQUIRE(geometry.scene->producer=="PrusaSlicer");
    REQUIRE(geometry.scene->source_filaments.size()==1);REQUIRE(geometry.scene->objects.size()==1);
    REQUIRE(geometry.scene->objects.front().default_filament_slot==0);
    REQUIRE_FALSE(geometry.scene->objects.front().parts.front().explicit_filament_binding);
    REQUIRE(library->resize_active_filament_slots(2).success);const auto target=*library->active_config_snapshot();
    auto empty_scene=std::make_shared<libslicer::ProjectScene>(*geometry.scene);empty_scene->source_filaments.clear();
    auto empty_candidate=geometry;empty_candidate.scene=empty_scene;
    const auto empty_mapping=library->remap_project_materials(empty_candidate,{},target);CHECK_FALSE(empty_mapping.success);
    const auto mapped=library->remap_project_materials(geometry,{2},target);REQUIRE(mapped.success);
    const auto& object=mapped.scene->objects.front();CHECK(object.default_filament_slot==2);
    CHECK_FALSE(object.parts.front().explicit_filament_binding);
    // Project candidates are not remapped, and retain their own dynamic
    // default. Explicit Geometry part assignments map independently.
    auto project_scene=std::make_shared<libslicer::ProjectScene>(*geometry.scene);project_scene->mode=libslicer::ProjectReadMode::Project;
    project_scene->objects.front().parts.front().explicit_filament_binding=true;project_scene->objects.front().parts.front().default_filament_slot=1;
    auto project_candidate=geometry;project_candidate.scene=project_scene;
    const auto mapped_project=library->remap_project_materials(project_candidate,{2},target);CHECK_FALSE(mapped_project.success);
    CHECK(project_candidate.scene->objects.front().default_filament_slot==0);
    project_scene->mode=libslicer::ProjectReadMode::Geometry;
    const auto explicit_part=library->remap_project_materials(project_candidate,{2},target);REQUIRE(explicit_part.success);
    CHECK(explicit_part.scene->objects.front().parts.front().explicit_filament_binding);
    CHECK(explicit_part.scene->objects.front().parts.front().default_filament_slot==2);
    libslicer::SliceObjectInput input;input.name="prusa-source-inherited-material";input.default_filament_slot=object.default_filament_slot;input.transform=object.instances.front().transform;
    input.project_semantics=libslicer::encode_project_object_semantics(object);
    // Keep the complete external indexed mesh/annotations; print it at 10% size
    // so the actual filament-consumption regression remains a focused test.
    if(external)for(std::size_t r=0;r<3;++r)for(std::size_t c=0;c<4;++c)input.transform[r*4+c]*=.1;
    for(const auto& part:object.parts){
        libslicer::SliceVolumeInput volume;for(const auto& v:part.mesh->vertices)volume.vertices.push_back({v.x,v.y,v.z});
        for(const auto& t:part.mesh->triangles)volume.triangles.push_back({t.vertex_a,t.vertex_b,t.vertex_c});
        volume.inherit_object_filament=!part.explicit_filament_binding;volume.default_filament_slot=part.default_filament_slot;volume.local_transform=part.local_transform;
        volume.project_semantics=libslicer::encode_project_part_semantics(part);volume.surface_annotations=libslicer::encode_project_part_annotations(part);volume.facet_labels=part.material_facets;
        input.volumes.push_back(std::move(volume));
    }
    libslicer::SliceRequest request;request.objects={input};request.config=target;request.default_filament_slot=1;request.center_on_build_plate=false;
    const auto sliced=library->slice(request);for(const auto& d:sliced.diagnostics)INFO(d.message);REQUIRE(sliced.success);REQUIRE(sliced.preview);
    std::set<unsigned> used;for(const auto& segment:sliced.preview->segments)if(segment.motion==libslicer::ToolpathMotionKind::Extrusion&&segment.extrusion_role>=libslicer::ToolpathExtrusionRole::InnerWall&&segment.extrusion_role<=libslicer::ToolpathExtrusionRole::GapInfill)used.insert(segment.filament_id);
    CHECK(used==std::set<unsigned>{1});
    if(sliced.output.ownership==libslicer::OutputArtifactOwnership::LibraryTemporary)std::filesystem::remove(sliced.output.path);
    if(!external)std::filesystem::remove(source);
}

namespace {
void rewrite_project_fixture(const std::filesystem::path& path,
    std::map<std::string,std::string> replacements, const std::set<std::string>& removed={})
{
    mz_zip_archive reader{}; REQUIRE(mz_zip_reader_init_file(&reader,path.string().c_str(),0));
    for (mz_uint i=0;i<mz_zip_reader_get_num_files(&reader);++i) {
        mz_zip_archive_file_stat stat{}; REQUIRE(mz_zip_reader_file_stat(&reader,i,&stat));
        const std::string name=stat.m_filename;
        if (removed.count(name)||replacements.count(name)) continue;
        std::string bytes(std::size_t(stat.m_uncomp_size),'\0');
        REQUIRE(mz_zip_reader_extract_to_mem(&reader,i,bytes.data(),bytes.size(),0));
        replacements.emplace(name,std::move(bytes));
    }
    REQUIRE(mz_zip_reader_end(&reader));
    mz_zip_archive writer{}; REQUIRE(mz_zip_writer_init_file(&writer,path.string().c_str(),0));
    for (const auto& [name,bytes]:replacements)
        REQUIRE(mz_zip_writer_add_mem(&writer,name.c_str(),bytes.data(),bytes.size(),MZ_DEFAULT_COMPRESSION));
    REQUIRE(mz_zip_writer_finalize_archive(&writer)); REQUIRE(mz_zip_writer_end(&writer));
}
std::string project_model_xml(const std::filesystem::path& path) {
    const auto bytes=read_zip_entry(path,"3D/3dmodel.model"); return {bytes.begin(),bytes.end()};
}
void replace_project_text(std::string& text,const std::string& from,const std::string& to) {
    const auto offset=text.find(from); REQUIRE(offset!=std::string::npos); text.replace(offset,from.size(),to);
}
std::array<double,6> project_world_bounds(const libslicer::ProjectScene& scene) {
    std::array<double,6> bounds{1e100,1e100,1e100,-1e100,-1e100,-1e100};
    for (const auto& object:scene.objects) for (const auto& instance:object.instances) for (const auto& part:object.parts) {
        std::array<double,3> origin{};
        for (const auto& plate:scene.plates) if (plate.id==instance.plate_id) origin=plate.origin_mm;
        for (const auto& vertex:part.mesh->vertices) {
            const std::array<double,4> local{vertex.x,vertex.y,vertex.z,1}; std::array<double,4> world{}, volume{};
            for (int r=0;r<4;++r) for(int c=0;c<4;++c) volume[r]+=part.local_transform[r*4+c]*local[c];
            for (int r=0;r<4;++r) for(int c=0;c<4;++c) world[r]+=instance.transform[r*4+c]*volume[c];
            for(int r=0;r<3;++r) {bounds[r]=std::min(bounds[r],world[r]+origin[r]);bounds[r+3]=std::max(bounds[r+3],world[r]+origin[r]);}
        }
    }
    return bounds;
}
}

TEST_CASE("project contexts reject stale and foreign captures", "[libslicer_api][project-orca-parity]") {
    auto library=project_test_library();
    const auto source=project_candidate_fixture(*library->active_config_snapshot(),false);
    libslicer::ProjectReadRequest request{source.string(),libslicer::ProjectReadMode::Geometry};
    request.configuration=library->capture_configuration();
    const auto revision=library->configuration_revision();
    REQUIRE(library->read_project(request).success);
    REQUIRE(library->set_active_config_value("layer_height","0.16").success);
    const auto after=*library->active_config_snapshot();
    const auto stale=library->read_project(request);CHECK_FALSE(stale.success);CHECK_FALSE(stale.scene);
    auto other=project_test_library(); request.configuration=other->capture_configuration();
    CHECK_FALSE(library->read_project(request).success);
    request.configuration=library->capture_configuration();
    const auto fresh=library->read_project(request); REQUIRE(fresh.success);
    CHECK(fresh.configuration_revision==library->configuration_revision());CHECK(fresh.configuration_revision>revision);
    CHECK(library->active_config_snapshot()->values()==after.values());
    // Deliberately share the exact immutable state and match both revisions.
    // Pointer/revision alone must not turn another Library's capture into ours.
    const auto shared=library->capture_configuration();
    REQUIRE(other->install_configuration(shared,other->configuration_revision()).success);
    REQUIRE(library->install_configuration(shared,library->configuration_revision()).success);
    REQUIRE(other->install_configuration(shared,other->configuration_revision()).success);
    REQUIRE(library->configuration_revision()==other->configuration_revision());
    request.configuration=other->capture_configuration();
    CHECK_FALSE(library->read_project(request).success);
    std::filesystem::remove(source);
}

TEST_CASE("project entry lifecycle is independent of geometry fallback", "[libslicer_api][project-orca-parity]") {
    auto library=project_test_library(); const auto before=*library->active_config_snapshot();
    const auto source=project_candidate_fixture(before,false,{},"CAD-1.0");
    auto model=project_model_xml(source);
    replace_project_text(model,"<metadata name=\"BambuStudio:3mfVersion\">1</metadata>","");
    rewrite_project_fixture(source,{{"3D/3dmodel.model",model}},{"Metadata/project_settings.config"});
    for (const auto mode:{libslicer::ProjectReadMode::Geometry,libslicer::ProjectReadMode::Project}) {
        const auto candidate=library->read_project({source.string(),mode});
        for(const auto& d:candidate.diagnostics) INFO(d.code<<": "<<d.message);
        REQUIRE(candidate.success);CHECK(candidate.scene->producer=="Other");CHECK_FALSE(candidate.load_config);
        CHECK(candidate.replace_document==(mode==libslicer::ProjectReadMode::Project));
        CHECK(candidate.scene->mode==libslicer::ProjectReadMode::Geometry);CHECK(candidate.required_decisions.empty());
        CHECK(candidate.configuration_changed==(mode==libslicer::ProjectReadMode::Project));
    }
    CHECK(library->active_config_snapshot()->values()==before.values());std::filesystem::remove(source);
}

TEST_CASE("native auto drop and project sinking rules survive DTO conversion", "[libslicer_api][project-orca-parity]") {
    const bool auto_drop=GENERATE(false,true); const double z=GENERATE(-2.,5.);
    const auto mode=GENERATE(libslicer::ProjectReadMode::Geometry,libslicer::ProjectReadMode::Project);
    CAPTURE(auto_drop,z,mode);
    auto library=project_test_library();const auto source=project_candidate_fixture(*library->active_config_snapshot(),false);
    auto model=project_model_xml(source);
    replace_project_text(model,"<item objectid=\"1\"",std::string("<item auto_drop=\"")+(auto_drop?"1":"0")+"\" objectid=\"1\"");
    replace_project_text(model,"50 50 0\"","50 50 "+std::to_string(z)+"\"");
    rewrite_project_fixture(source,{{"3D/3dmodel.model",model}});
    const auto candidate=library->read_project({source.string(),mode});REQUIRE(candidate.success);
    const auto bounds=project_world_bounds(*candidate.scene);
    const double expected=auto_drop?(mode==libslicer::ProjectReadMode::Project&&z<0?z:0.):z;
    CHECK(bounds[2]==Catch::Approx(expected).margin(1e-6)); CHECK(bounds[5]-bounds[2]==Catch::Approx(10.));
    std::filesystem::remove(source);
}

TEST_CASE("geometry retains native object settings and expands logical filaments", "[libslicer_api][project-orca-parity]") {
    auto library=project_test_library(); const auto before=*library->active_config_snapshot();
    const auto revision=library->configuration_revision();
    const auto source=project_candidate_fixture(before,false);
    const std::string settings=R"(<config><object id="1"><metadata key="extruder" value="3"/><metadata key="layer_height" value="0.12"/><part id="1" subtype="normal_part"><metadata key="extruder" value="3"/><metadata key="sparse_infill_density" value="45%"/></part></object></config>)";
    rewrite_project_fixture(source,{{"Metadata/model_settings.config",settings}});
    libslicer::ProjectReadRequest request{source.string(),libslicer::ProjectReadMode::Geometry};request.filament_color_cursor=2;
    const auto candidate=library->read_project(request);for(const auto& d:candidate.diagnostics) INFO(d.code<<": "<<d.message);
    REQUIRE(candidate.success); REQUIRE(candidate.configuration_changed); REQUIRE(candidate.configuration.valid());
    CHECK_FALSE(candidate.load_config); CHECK_FALSE(candidate.replace_document);
    REQUIRE(candidate.scene->source_filaments.size()==3);CHECK(candidate.filament_color_cursor==4);
    CHECK(candidate.scene->source_filaments[1].color.red==0xED);CHECK(candidate.scene->source_filaments[2].color.green==0xFF);
    CHECK(candidate.scene->objects.front().default_filament_slot==3);
    CHECK(candidate.scene->objects.front().settings==libslicer::ProjectSettings{{"layer_height","0.12"}});
    CHECK(library->configuration_revision()==revision);CHECK(library->active_config_snapshot()->values()==before.values());
    const auto installed=library->install_configuration(candidate.configuration,revision);REQUIRE(installed.success);
    REQUIRE(library->active_config()->filament_slots.size()==3);
    auto expanded_settings=settings;
    for(auto at=expanded_settings.find("value=\"3\"");at!=std::string::npos;at=expanded_settings.find("value=\"3\"",at+9))
        expanded_settings.replace(at,9,"value=\"5\"");
    rewrite_project_fixture(source,{{"Metadata/model_settings.config",expanded_settings}});
    request.filament_color_cursor.reset();
    const auto continued=library->read_project(request);REQUIRE(continued.success);
    CHECK(continued.filament_color_cursor==6);REQUIRE(continued.scene->source_filaments.size()==5);
    CHECK(continued.scene->source_filaments[3].color.red==0xF2);
    REQUIRE(library->install_configuration(installed.previous,installed.revision).success);
    const auto restarted=library->read_project(request);REQUIRE(restarted.success);
    CHECK(restarted.filament_color_cursor==4);REQUIRE(restarted.scene->source_filaments.size()==5);
    CHECK(restarted.scene->source_filaments[1].color.red==0x00);
    CHECK(library->active_config_snapshot()->values()==before.values());std::filesystem::remove(source);
}

TEST_CASE("readable invalid project settings import but cannot slice", "[libslicer_api][project-orca-parity]") {
    auto library=project_test_library();
    const auto source=project_candidate_fixture(*library->active_config_snapshot(),false,{{"layer_height","-0.1"},{"different_settings_to_system","layer_height;\"\";\"\""}});
    const auto candidate=library->read_project({source.string(),libslicer::ProjectReadMode::Project});
    for(const auto& d:candidate.diagnostics) INFO(d.code<<": "<<d.message);
    REQUIRE(candidate.success);REQUIRE(candidate.configuration.valid());
    CHECK(candidate.configuration.snapshot().value("layer_height")=="-0.1");
    CHECK(std::any_of(candidate.diagnostics.begin(),candidate.diagnostics.end(),[](const auto& d){return d.code=="project.invalid_configuration";}));
    libslicer::SliceRequest request;request.config=candidate.configuration.snapshot();
    const auto& object=candidate.scene->objects.front();libslicer::SliceObjectInput input;
    input.transform=object.instances.front().transform;
    for(const auto& part:object.parts) {
        libslicer::SliceVolumeInput volume;volume.local_transform=part.local_transform;
        for(const auto& p:part.mesh->vertices)volume.vertices.push_back({p.x,p.y,p.z});
        for(const auto& t:part.mesh->triangles)volume.triangles.push_back({t.vertex_a,t.vertex_b,t.vertex_c});
        input.volumes.push_back(std::move(volume));
    }
    request.objects.push_back(std::move(input));
    const auto sliced=library->slice(request);CHECK_FALSE(sliced.success);
    CHECK(std::any_of(sliced.diagnostics.begin(),sliced.diagnostics.end(),[](const auto& d){return d.message.find("layer_height")!=std::string::npos||d.message.find("layer height")!=std::string::npos||d.message.find("Layer height")!=std::string::npos;}));
    std::filesystem::remove(source);
}

TEST_CASE("native unit questions support no yes and external cancellation", "[libslicer_api][project-orca-parity]") {
    auto library=project_test_library(); const auto source=project_candidate_fixture(*library->active_config_snapshot(),false);
    auto model=project_model_xml(source);
    for(const auto axis:{"x","y","z"}) {
        const auto from=std::string(axis)+"=\"10\"";const auto to=std::string(axis)+"=\"0.01\"";
        for(auto at=model.find(from);at!=std::string::npos;at=model.find(from,at+to.size())) model.replace(at,from.size(),to);
    }
    rewrite_project_fixture(source,{{"3D/3dmodel.model",model}});
    const libslicer::ProjectReadRequest request{source.string(),libslicer::ProjectReadMode::Geometry};
    const auto absent=library->read_project(request);CHECK_FALSE(absent.success);CHECK(absent.required_decisions==std::vector<std::string>{"convert_meters"});
    for(const bool yes:{false,true}) {
        int decisions=0;
        const auto candidate=library->read_project(request,{},[&](const auto& decision){++decisions;CHECK(decision.kind==libslicer::ProjectDecisionKind::ConvertMeters);return yes?libslicer::ProjectDecisionAnswer::Yes:libslicer::ProjectDecisionAnswer::No;});
        REQUIRE(candidate.success);CHECK(decisions==1);const auto bounds=project_world_bounds(*candidate.scene);
        CHECK(bounds[3]-bounds[0]==Catch::Approx(yes?10.:.01).margin(1e-5));
    }
    bool cancelled=false;libslicer::SliceCallbacks callbacks;callbacks.is_cancelled=[&]{return cancelled;};
    const auto result=library->read_project(request,callbacks,[&](const auto&){cancelled=true;return libslicer::ProjectDecisionAnswer::No;});
    CHECK(result.cancelled);CHECK_FALSE(result.scene);CHECK_FALSE(result.configuration.valid());std::filesystem::remove(source);
}

TEST_CASE("native gcode job validity is separate from drawable preview", "[libslicer_api][project-orca-parity]") {
    const bool drawable=GENERATE(false,true);
    auto library=project_test_library(); const auto source=project_candidate_fixture(*library->active_config_snapshot(),false);
    auto model=project_model_xml(source);
    const auto begin=model.find("<resources>");const auto end=model.find("</build>")+8;REQUIRE(begin!=std::string::npos);
    model.replace(begin,end-begin,"<resources/><build/>");
    const std::string plates=R"(<config><plate><metadata key="plater_id" value="1"/><metadata key="gcode_file" value="Metadata/plate_1.gcode"/></plate></config>)";
    const std::string gcode=drawable?
        "; standalone minimal G-code\nG90\nM83\n;LAYER_CHANGE\n;Z:0.2\nG1 Z0.2\nG1 X10 Y10 F1200\n;TYPE:Outer wall\nG1 X30 Y10 E1\nG1 X30 Y30 E1\n":
        "; a valid job file with no drawable extrusion\nG90\n";
    rewrite_project_fixture(source,{{"3D/3dmodel.model",model},{"Metadata/model_settings.config",plates},{"Metadata/plate_1.gcode",gcode}});
    const auto candidate=library->read_project({source.string(),libslicer::ProjectReadMode::Project});
    for(const auto& d:candidate.diagnostics) INFO(d.code<<": "<<d.message);
    REQUIRE(candidate.success);REQUIRE(candidate.scene);CHECK(candidate.scene->objects.empty());
    REQUIRE(candidate.scene->plates.size()==1);CHECK(candidate.scene->plates[0].slice_result_valid);
    CHECK(candidate.scene->plates[0].gcode==gcode);
    if(drawable){REQUIRE(candidate.scene->plates[0].preview);CHECK_FALSE(candidate.scene->plates[0].preview->segments.empty());}
    else CHECK_FALSE(candidate.scene->plates[0].preview);
    CHECK(candidate.scene->kind==libslicer::ProjectContainerKind::GCodeJob);CHECK(candidate.scene->preview_only);
    std::filesystem::remove(source);CHECK(candidate.scene->plates[0].gcode==gcode);
    if(drawable)CHECK_FALSE(candidate.scene->plates[0].preview->segments.empty());
}

TEST_CASE("downloaded 3mf corpus exercises both native import entries", "[libslicer_api][project-corpus]") {
    const char* directory=std::getenv("LIBSLICER_PROJECT_CORPUS");
    if(!directory) SKIP("Set LIBSLICER_PROJECT_CORPUS to the archived Downloads corpus");
    auto library=project_test_library();std::vector<std::filesystem::path> files;
    for(const auto& entry:std::filesystem::recursive_directory_iterator(directory)) if(entry.is_regular_file()&&entry.path().extension()==".3mf")files.push_back(entry.path());
    std::sort(files.begin(),files.end());REQUIRE_FALSE(files.empty());
    nlohmann::json results=nlohmann::json::array();
    for(const auto& path:files) for(const auto mode:{libslicer::ProjectReadMode::Geometry,libslicer::ProjectReadMode::Project}) {
        const auto before=*library->active_config_snapshot();const auto revision=library->configuration_revision();
        nlohmann::json decisions=nlohmann::json::array();
        const auto candidate=library->read_project({path.string(),mode},{},[&](const auto& decision){decisions.push_back(int(decision.kind));return libslicer::ProjectDecisionAnswer::No;});
        nlohmann::json diagnostics=nlohmann::json::array();for(const auto& d:candidate.diagnostics)diagnostics.push_back({{"code",d.code},{"message",d.message}});
        results.push_back({{"file",path.string()},{"mode",int(mode)},{"success",candidate.success},{"load_config",candidate.load_config},{"replace_document",candidate.replace_document},{"decisions_no",decisions},{"diagnostics",diagnostics},{"objects",candidate.scene?candidate.scene->objects.size():0}});
        CHECK(library->configuration_revision()==revision);CHECK(library->active_config_snapshot()->values()==before.values());
        // Malformed fixtures may correctly fail; the report preserves the exact
        // native outcome. GUI equivalence is checked independently below.
        CHECK_FALSE(candidate.cancelled);CHECK(candidate.required_decisions.empty());
        if(candidate.success){REQUIRE(candidate.scene);CHECK(candidate.replace_document==(mode==libslicer::ProjectReadMode::Project));}
    }
    if(const char* output=std::getenv("LIBSLICER_PROJECT_CORPUS_REPORT")){std::ofstream file(output);REQUIRE(file.good());file<<results.dump(2)<<'\n';}
}

TEST_CASE("empty project configuration respects the original producer branch order", "[libslicer_api][project-orca-parity]") {
    const bool orca=GENERATE(false,true);const std::string version=GENERATE(std::string("99.0.0"),std::string("2.3.1-alpha"),std::string("1.0.0"));
    CAPTURE(orca,version);
    auto library=project_test_library();const auto source=project_candidate_fixture(*library->active_config_snapshot(),false,{},"BambuStudio-"+version);
    auto model=project_model_xml(source);
    if(orca)model.insert(model.find("<resources>"),"<metadata name=\"OrcaSlicer\">"+version+"</metadata>");
    rewrite_project_fixture(source,{{"3D/3dmodel.model",model}},{"Metadata/project_settings.config"});
    const auto candidate=library->read_project({source.string(),libslicer::ProjectReadMode::Project});
    for(const auto& d:candidate.diagnostics)INFO(d.code<<": "<<d.message);
    if(version=="2.3.1-alpha") {
        // Preserve the alpha branch instead of inventing an empty-config
        // fallback. Its missing required field is a read error, never a null
        // dereference through original DynamicConfig::opt_string.
        CHECK_FALSE(candidate.success);CHECK_FALSE(candidate.scene);
        CHECK(std::any_of(candidate.diagnostics.begin(),candidate.diagnostics.end(),[](const auto& d){return d.message.find("missing sparse_infill_rotate_template")!=std::string::npos;}));
    } else {
        REQUIRE(candidate.success);CHECK(candidate.replace_document);
        CHECK(candidate.scene->producer==(orca?"OrcaSlicer":"BambuStudio"));
        CHECK(candidate.load_config==(orca&&version=="99.0.0"));
    }
    std::filesystem::remove(source);
}

TEST_CASE("Prusa project follows original geometry fallback", "[libslicer_api][project-orca-parity]") {
    auto library=project_test_library();const auto before=*library->active_config_snapshot();
    const auto source=project_candidate_fixture(before,false,{},"PrusaSlicer-2.9.0");
    auto model=project_model_xml(source);replace_project_text(model,"<metadata name=\"BambuStudio:3mfVersion\">1</metadata>","");
    rewrite_project_fixture(source,{{"3D/3dmodel.model",model}},{"Metadata/project_settings.config"});
    const auto candidate=library->read_project({source.string(),libslicer::ProjectReadMode::Project});
    for(const auto& d:candidate.diagnostics)INFO(d.code<<": "<<d.message);
    REQUIRE(candidate.success);CHECK(candidate.scene->producer=="PrusaSlicer");CHECK_FALSE(candidate.load_config);
    CHECK(candidate.replace_document);CHECK(candidate.required_decisions.empty());CHECK_FALSE(candidate.scene->objects.empty());
    CHECK(library->active_config_snapshot()->values()==before.values());std::filesystem::remove(source);
}

TEST_CASE("legacy multi plate frames use native integer dimensions and loaded printer", "[libslicer_api][project-orca-parity]") {
    const bool legacy=GENERATE(false,true);CAPTURE(legacy);
    auto library=project_test_library();const auto before=*library->active_config_snapshot();
    const auto source=project_candidate_fixture(before,true,{{"printable_area","0x0,190.9x0,190.9x170.9,0x170.9"},{"printable_height","210"}},legacy?"BambuStudio-1.0.0":"BambuStudio-2.0.0");
    auto model=project_model_xml(source);
    // New printer's native width is int(190.9)=190. Its plate 2 frame starts at
    // 228, irrespective of the previous printer's 220 mm bed or the legacy
    // temporary int(220+1.25)=221 frame. The test point intersects only plate 2.
    replace_project_text(model,"50 50 0\"","300 50 0\"");
    const std::string plates=R"(<config><plate><metadata key="plater_id" value="1"/></plate><plate><metadata key="plater_id" value="2"/></plate></config>)";
    rewrite_project_fixture(source,{{"3D/3dmodel.model",model},{"Metadata/model_settings.config",plates}});
    libslicer::ProjectReadRequest request{source.string(),libslicer::ProjectReadMode::Project};
    libslicer::ProjectBedContext bed;bed.maximum_x=220;bed.maximum_y=220;bed.native_plate_size=std::array<int,3>{220,220,220};request.bed=bed;
    const auto candidate=library->read_project(request);for(const auto& d:candidate.diagnostics)INFO(d.code<<": "<<d.message);
    REQUIRE(candidate.success);REQUIRE(candidate.scene->plates.size()==2);REQUIRE(candidate.scene->objects.size()==1);
    CHECK(candidate.scene->plates[1].origin_mm[0]==Catch::Approx(legacy?264.:228.));
    CHECK(candidate.scene->plates[1].native_size_mm[0]==(legacy?220:190));
    const auto& instance=candidate.scene->objects.front().instances.front();CHECK(instance.plate_id=="plate-2");
    const auto bounds=project_world_bounds(*candidate.scene);
    CHECK(bounds[0]==Catch::Approx(legacy?336.:300.).margin(1e-5));
    CHECK(bounds[3]-bounds[0]==Catch::Approx(10.));
    CHECK(candidate.configuration.snapshot().value("printable_area")=="0x0,190.9x0,190.9x170.9,0x170.9");
    std::filesystem::remove(source);
}

TEST_CASE("small loaded beds consume the previous axes before integer plate sizing", "[libslicer_api][project-orca-parity]") {
    const bool offset=GENERATE(false,true);CAPTURE(offset);
    auto library=project_test_library();
    const auto source=project_candidate_fixture(*library->active_config_snapshot(),true,
        {{"printable_area",offset?"100x0,105.5x0,105.5x5.5,100x5.5":"0x0,5.5x0,5.5x5.5,0x5.5"},{"printable_height","210"}});
    // Original reset leaves the old AD5X axes at 0.1*220=22. set_shape
    // calculates new bounds before updating axes to 0.1*210=21.
    const int native_width=offset?83:22;const double x=native_width*1.2+(offset?101:1);
    auto model=project_model_xml(source);replace_project_text(model,"50 50 0\"",std::to_string(x)+" 1 0\"");
    const std::string plates=R"(<config><plate><metadata key="plater_id" value="1"/></plate><plate><metadata key="plater_id" value="2"/></plate></config>)";
    rewrite_project_fixture(source,{{"3D/3dmodel.model",model},{"Metadata/model_settings.config",plates}});
    const auto candidate=library->read_project({source.string(),libslicer::ProjectReadMode::Project});
    for(const auto& d:candidate.diagnostics)INFO(d.code<<": "<<d.message);
    REQUIRE(candidate.success);REQUIRE(candidate.scene->plates.size()==2);
    CHECK(candidate.scene->plates[1].native_size_mm[0]==native_width);
    CHECK(candidate.scene->plates[1].origin_mm[0]==Catch::Approx(native_width*1.2));
    CHECK(candidate.scene->objects.front().instances.front().plate_id=="plate-2");
    CHECK(project_world_bounds(*candidate.scene)[0]==Catch::Approx(x).margin(1e-5));
    std::filesystem::remove(source);
}

TEST_CASE("throwing cancellation callbacks return structured read failures", "[libslicer_api][project-orca-parity]") {
    auto library=project_test_library();const auto before=*library->active_config_snapshot();
    const auto source=project_candidate_fixture(before,false);libslicer::SliceCallbacks callbacks;
    callbacks.is_cancelled=[]()->bool {throw 7;};
    const auto candidate=library->read_project({source.string(),libslicer::ProjectReadMode::Project},callbacks);
    CHECK_FALSE(candidate.success);CHECK_FALSE(candidate.cancelled);CHECK_FALSE(candidate.scene);CHECK_FALSE(candidate.configuration.valid());
    REQUIRE(candidate.diagnostics.size()==1);CHECK(candidate.diagnostics.front().code=="project.read_failed");
    CHECK(library->active_config_snapshot()->values()==before.values());std::filesystem::remove(source);
}

TEST_CASE("project reset changes an embedded printer before consuming the bed snapshot", "[libslicer_api][project-orca-parity]") {
    auto library=project_test_library();const auto original=*library->active_config_snapshot();
    const std::string child="Project reset child";const std::string parent="Flashforge AD5X 0.4 nozzle";
    const auto embedded=project_candidate_fixture(original,false,{
        {"printer_settings_id",child},{"printer_model",child},
        {"printable_area","0x0,100x0,100x100,0x100"},{"printable_height","100"},
        {"inherits_group","\"\";\"\";\""+parent+"\""},
        {"different_settings_to_system","\"\";\"\";\"printable_area;printable_height;printer_model\""}});
    const nlohmann::json preset={{"name",child},{"printer_settings_id",child},{"inherits",parent},
        {"printer_model",child},{"printable_area","0x0,100x0,100x100,0x100"},{"printable_height","100"},
        {"type","machine"},{"version","2.0.0.0"},{"from","project"}};
    rewrite_project_fixture(embedded,{{"Metadata/machine_settings_1.config",preset.dump()}});
    const auto prepared=library->read_project({embedded.string(),libslicer::ProjectReadMode::Project});
    for(const auto& d:prepared.diagnostics)INFO(d.code<<": "<<d.message);
    REQUIRE(prepared.success);REQUIRE(prepared.configuration.snapshot().value("printer_settings_id")==child);
    REQUIRE(library->install_configuration(prepared.configuration,library->configuration_revision()).success);
    REQUIRE(library->active_config_snapshot()->value("printable_area")=="0x0,100x0,100x100,0x100");
    const auto source=project_candidate_fixture(*library->active_config_snapshot(),false,{},"CAD-1.0");
    rewrite_project_fixture(source,{}, {"Metadata/project_settings.config"});
    libslicer::ProjectBedContext bed;bed.maximum_x=bed.maximum_y=100;bed.native_plate_size=std::array<int,3>{100,100,100};bed.axes_length_mm=10;
    for(const auto mode:{libslicer::ProjectReadMode::Geometry,libslicer::ProjectReadMode::Project}) {
        libslicer::ProjectReadRequest request{source.string(),mode};request.bed=bed;
        const auto candidate=library->read_project(request);for(const auto& d:candidate.diagnostics)INFO(d.code<<": "<<d.message);
        REQUIRE(candidate.success);CHECK_FALSE(candidate.load_config);
        const auto bounds=project_world_bounds(*candidate.scene);const double expected=mode==libslicer::ProjectReadMode::Project?105:45;
        CHECK(bounds[0]==Catch::Approx(expected));CHECK(bounds[1]==Catch::Approx(expected));
        if(mode==libslicer::ProjectReadMode::Project) {
            CHECK(candidate.configuration.snapshot().value("printer_settings_id")==parent);
            CHECK(candidate.scene->plates.front().native_size_mm[0]==220);
        }
    }
    CHECK(library->active_config_snapshot()->value("printer_settings_id")==child);
    std::filesystem::remove(source);std::filesystem::remove(embedded);
}

TEST_CASE("native preset preparation failures keep configuration edits atomic", "[libslicer_api][project-candidate]") {
    auto library=project_test_library();REQUIRE(library->resize_active_filament_slots(2).success);
    const auto before=*library->active_config_snapshot();const auto revision=library->configuration_revision();
    const auto failed=library->apply_active_config_patch({{"filament_start_gcode",""}});
    CHECK_FALSE(failed.success);CHECK_FALSE(failed.diagnostics.empty());CHECK(failed.changed_items.empty());
    CHECK(library->configuration_revision()==revision);CHECK(library->active_config_snapshot()->values()==before.values());
    const auto selected=library->active_config()->selection;
    libslicer::ConfigSelection selection{selected.machine_model_id,selected.machine_variant_id,selected.process_preset_id,selected.filament_preset_ids,selected.filament_physical_tools};
    const auto activation=library->activate_config(selection,{{"filament_start_gcode",""}});
    CHECK_FALSE(activation.success);CHECK_FALSE(activation.diagnostics.empty());
    CHECK(library->configuration_revision()==revision);CHECK(library->active_config_snapshot()->values()==before.values());
    const auto valid=library->apply_active_config_patch({{"filament_start_gcode","\"\";\"\""}});
    CHECK(valid.success);CHECK(library->configuration_revision()==revision+1);
}

TEST_CASE("unchanged native beds preserve captured plate dimensions", "[libslicer_api][project-orca-parity]") {
    auto library=project_test_library();
    REQUIRE(library->apply_active_config_patch({{"printable_area","0x0,5.5x0,5.5x5.5,0x5.5"},{"printable_height","210"}}).success);
    const bool ancillary_change=GENERATE(false,true);
    const auto source=project_candidate_fixture(*library->active_config_snapshot(),false,
        {{"bed_exclude_area",ancillary_change?"0x0,1x0,1x1,0x1":""},
         {"different_settings_to_system","\"\";\"\";\"printable_area;printable_height;bed_exclude_area\""}});
    libslicer::ProjectBedContext bed;bed.maximum_x=bed.maximum_y=5.5;
    // A previous change from a 220 mm printer consumed axes22. Native cached
    // plate width is22 although the axes have now updated to21 for height210.
    bed.native_plate_size=std::array<int,3>{22,22,210};bed.axes_length_mm=21;
    libslicer::ProjectReadRequest request{source.string(),libslicer::ProjectReadMode::Project};request.bed=bed;
    const auto result=library->read_project(request);for(const auto& d:result.diagnostics)INFO(d.code<<": "<<d.message);
    REQUIRE(result.success);CHECK(result.load_config);REQUIRE_FALSE(result.scene->plates.empty());
    CHECK(result.scene->plates.front().native_size_mm==std::array<int,3>{22,22,210});
    std::filesystem::remove(source);
}

TEST_CASE("zero native print height remains editable across project entry points", "[libslicer_api][project-orca-parity][project-followup]") {
    auto library=project_test_library();const auto initial=*library->active_config_snapshot();
    const auto source=project_candidate_fixture(initial,false,{{"printable_height","0"},
        {"different_settings_to_system","\"\";\"\";\"printable_height\""}});
    const auto candidate=library->read_project({source.string(),libslicer::ProjectReadMode::Project});
    for(const auto& d:candidate.diagnostics)INFO(d.code<<": "<<d.message);
    REQUIRE(candidate.success);REQUIRE(candidate.configuration.valid());
    CHECK(candidate.configuration.snapshot().value("printable_height")=="0");
    CHECK(candidate.configuration.machine().variant.printable_height==0);
    REQUIRE_FALSE(candidate.scene->objects.empty());
    REQUIRE(library->install_configuration(candidate.configuration,library->configuration_revision()).success);
    CHECK(library->active_config_snapshot()->value("printable_height")=="0");
    REQUIRE(library->set_active_config_value("layer_height","0.16").success);
    CHECK(library->active_config_snapshot()->value("printable_height")=="0");
    CHECK(library->read_project({source.string(),libslicer::ProjectReadMode::Geometry}).success);
    REQUIRE(library->set_active_config_value("printable_height","180").success);
    const auto revision=library->configuration_revision();
    const auto legacy=library->import_project({source.string()});
    for(const auto& d:legacy.diagnostics)INFO(d.message);
    REQUIRE(legacy.success);CHECK(library->configuration_revision()==revision+1);
    CHECK(library->active_config_snapshot()->value("printable_height")=="0");
    CHECK_FALSE(library->set_active_config_value("printable_height","-1").success);
    CHECK(library->active_config_snapshot()->value("printable_height")=="0");
    std::filesystem::remove(source);
}

TEST_CASE("mapped geometry discards source palette installation and supports remapping", "[libslicer_api][project-candidate][project-followup]") {
    auto library=project_test_library();const auto target=*library->active_config_snapshot();
    const auto revision=library->configuration_revision();
    const auto source=project_candidate_fixture(target,false);
    const std::string settings=R"(<config><object id="1"><metadata key="extruder" value="3"/><part id="1" subtype="normal_part"><metadata key="extruder" value="3"/></part></object></config>)";
    rewrite_project_fixture(source,{{"Metadata/model_settings.config",settings}});
    const auto expanded=library->read_project({source.string(),libslicer::ProjectReadMode::Geometry});
    REQUIRE(expanded.success);REQUIRE(expanded.configuration_changed);
    REQUIRE(expanded.configuration.valid());REQUIRE(expanded.scene->source_filaments.size()==3);
    const auto mapped=library->remap_project_materials(expanded,{1,1,1},target);
    REQUIRE(mapped.success);CHECK_FALSE(mapped.configuration_changed);CHECK_FALSE(mapped.configuration.valid());
    CHECK(mapped.configuration_revision==expanded.configuration_revision);
    REQUIRE(mapped.scene->source_filaments.size()==1);
    CHECK(mapped.scene->source_filaments.front().preset_id==library->active_config()->filament_slots.front().preset_id);
    CHECK(mapped.scene->source_filaments.front().color.red==library->active_config()->filament_slots.front().color.red);
    CHECK(mapped.scene->objects.front().default_filament_slot==1);
    // Native single-volume loading removes the redundant volume binding.
    CHECK(mapped.scene->objects.front().parts.front().default_filament_slot==0);
    CHECK_FALSE(mapped.scene->objects.front().parts.front().explicit_filament_binding);
    CHECK(mapped.filament_color_cursor==0);CHECK(expanded.filament_color_cursor==2);
    const auto invalid=library->remap_project_materials(expanded,{1,1},target);
    CHECK_FALSE(invalid.success);CHECK_FALSE(invalid.scene);CHECK_FALSE(invalid.configuration.valid());
    libslicer::SliceCallbacks cancelled;cancelled.is_cancelled=[] { return true; };
    const auto aborted=library->remap_project_materials(expanded,{1,1,1},target,cancelled);
    CHECK(aborted.cancelled);CHECK_FALSE(aborted.scene);CHECK_FALSE(aborted.configuration.valid());
    CHECK(library->configuration_revision()==revision);CHECK(library->active_config_snapshot()->values()==target.values());
    REQUIRE(library->resize_active_filament_slots(2).success);
    const auto next_target=*library->active_config_snapshot();
    const auto again=library->remap_project_materials(mapped,{2},next_target);
    REQUIRE(again.success);REQUIRE(again.scene->source_filaments.size()==2);
    CHECK(again.scene->objects.front().default_filament_slot==2);
    CHECK(again.scene->objects.front().parts.front().default_filament_slot==0);
    CHECK_FALSE(again.scene->objects.front().parts.front().explicit_filament_binding);
    CHECK_FALSE(again.configuration_changed);CHECK_FALSE(again.configuration.valid());
    CHECK(library->configuration_revision()==revision+1);
    CHECK(library->active_config_snapshot()->values()==next_target.values());
    std::filesystem::remove(source);
}

TEST_CASE("model project gcode bytes do not activate a native slice result", "[libslicer_api][project-orca-parity][project-followup]") {
    auto library=project_test_library();const auto source=project_candidate_fixture(*library->active_config_snapshot(),false);
    const std::string plates=R"(<config><plate><metadata key="plater_id" value="1"/><metadata key="gcode_file" value="Metadata/plate_1.gcode"/></plate></config>)";
    const std::string gcode="G90\nM83\n;LAYER_CHANGE\n;Z:0.2\nG1 Z0.2\nG1 X10 Y10 F1200\n;TYPE:Outer wall\nG1 X30 Y10 E1\n";
    rewrite_project_fixture(source,{{"Metadata/model_settings.config",plates},{"Metadata/plate_1.gcode",gcode}});
    const auto candidate=library->read_project({source.string(),libslicer::ProjectReadMode::Project});
    REQUIRE(candidate.success);REQUIRE_FALSE(candidate.scene->objects.empty());
    REQUIRE(candidate.scene->plates.size()==1);const auto& plate=candidate.scene->plates.front();
    CHECK(plate.gcode==gcode);CHECK_FALSE(plate.slice_result_valid);CHECK_FALSE(plate.preview);
    CHECK_FALSE(candidate.scene->preview_only);CHECK(candidate.scene->kind!=libslicer::ProjectContainerKind::GCodeJob);
    std::filesystem::remove(source);CHECK(plate.gcode==gcode);
}

TEST_CASE("empty model job selects the first valid plate", "[libslicer_api][project-orca-parity][project-followup]") {
    auto library=project_test_library();const auto source=project_candidate_fixture(*library->active_config_snapshot(),false);
    auto model=project_model_xml(source);const auto begin=model.find("<resources>");const auto end=model.find("</build>")+8;
    model.replace(begin,end-begin,"<resources/><build/>");
    const std::string plates=R"(<config><plate><metadata key="plater_id" value="1"/></plate><plate><metadata key="plater_id" value="2"/><metadata key="gcode_file" value="Metadata/plate_2.gcode"/></plate></config>)";
    rewrite_project_fixture(source,{{"3D/3dmodel.model",model},{"Metadata/model_settings.config",plates},{"Metadata/plate_2.gcode","G90\n"}});
    const auto candidate=library->read_project({source.string(),libslicer::ProjectReadMode::Project});
    REQUIRE(candidate.success);REQUIRE(candidate.scene->plates.size()==2);
    CHECK_FALSE(candidate.scene->plates[0].slice_result_valid);CHECK(candidate.scene->plates[1].slice_result_valid);
    CHECK(candidate.scene->active_plate_index==1);CHECK(candidate.scene->preview_only);
    CHECK(candidate.scene->kind==libslicer::ProjectContainerKind::GCodeJob);
    std::filesystem::remove(source);
}

TEST_CASE("five project roles preserve their explicit SDK semantics", "[libslicer_api][project-orca-parity][project-followup]") {
    auto library=project_test_library();const auto source=project_candidate_fixture(*library->active_config_snapshot(),false);
    auto model=project_model_xml(source);const auto begin=model.find("<object id=\"1\"");const auto end=model.find("</object>",begin)+9;
    const auto original=model.substr(begin,end-begin);std::string resources;
    const std::array<const char*,5> types={"normal_part","negative_part","modifier_part","support_enforcer","support_blocker"};
    std::string settings="<config><object id=\"10\">";
    for(int i=1;i<=5;++i) {
        auto part=original;replace_project_text(part,"id=\"1\"","id=\""+std::to_string(i)+"\"");resources+=part;
        settings+="<part id=\""+std::to_string(i)+"\" subtype=\""+types[i-1]+"\"><metadata key=\"name\" value=\"role-"+types[i-1]+"\"/></part>";
    }
    resources+="<object id=\"10\" type=\"model\"><components>";
    for(int i=1;i<=5;++i)resources+="<component objectid=\""+std::to_string(i)+"\"/>";
    resources+="</components></object>";settings+="</object></config>";
    model.replace(begin,end-begin,resources);replace_project_text(model,"<item objectid=\"1\"","<item objectid=\"10\"");
    rewrite_project_fixture(source,{{"3D/3dmodel.model",model},{"Metadata/model_settings.config",settings}});
    const auto candidate=library->read_project({source.string(),libslicer::ProjectReadMode::Geometry});
    REQUIRE(candidate.success);REQUIRE(candidate.scene->objects.size()==1);
    const std::map<std::string,libslicer::ProjectPartRole> expected={
        {"role-normal_part",libslicer::ProjectPartRole::Model},
        {"role-negative_part",libslicer::ProjectPartRole::NegativeVolume},
        {"role-modifier_part",libslicer::ProjectPartRole::ParameterModifier},
        {"role-support_enforcer",libslicer::ProjectPartRole::SupportEnforcer},
        {"role-support_blocker",libslicer::ProjectPartRole::SupportBlocker}};
    REQUIRE(candidate.scene->objects.front().parts.size()==expected.size());
    std::set<libslicer::ProjectPartRole> actual;
    for(const auto& p:candidate.scene->objects.front().parts) {
        CAPTURE(p.name);REQUIRE(expected.count(p.name)==1);CHECK(p.role==expected.at(p.name));actual.insert(p.role);
    }
    CHECK(actual==std::set<libslicer::ProjectPartRole>{libslicer::ProjectPartRole::Model,libslicer::ProjectPartRole::NegativeVolume,
        libslicer::ProjectPartRole::ParameterModifier,libslicer::ProjectPartRole::SupportEnforcer,libslicer::ProjectPartRole::SupportBlocker});
    std::filesystem::remove(source);
}

TEST_CASE("explicit geometry mapping preserves an installed nonzero color cursor", "[libslicer_api][project-followup]") {
    auto library=project_test_library();const auto source=project_candidate_fixture(*library->active_config_snapshot(),false);
    const std::string settings=R"(<config><object id="1"><metadata key="extruder" value="3"/></object></config>)";
    rewrite_project_fixture(source,{{"Metadata/model_settings.config",settings}});
    libslicer::ProjectReadRequest request{source.string(),libslicer::ProjectReadMode::Geometry};request.filament_color_cursor=7;
    const auto expanded=library->read_project(request);REQUIRE(expanded.success);REQUIRE(expanded.filament_color_cursor==9);
    REQUIRE(library->install_configuration(expanded.configuration,library->configuration_revision()).success);
    const auto target=*library->active_config_snapshot();const auto revision=library->configuration_revision();
    request.filament_color_cursor=100;
    const auto input=library->read_project(request);REQUIRE(input.success);REQUIRE(input.filament_color_cursor==100);
    const auto result=library->remap_project_materials(input,{1,1,1},target);
    REQUIRE(result.success);CHECK(result.filament_color_cursor==9);CHECK_FALSE(result.configuration_changed);
    CHECK_FALSE(result.configuration.valid());CHECK(result.configuration_revision==input.configuration_revision);
    CHECK(library->configuration_revision()==revision);CHECK(library->active_config_snapshot()->values()==target.values());
    request.filament_color_cursor.reset();CHECK(library->read_project(request).filament_color_cursor==9);
    std::filesystem::remove(source);
}
