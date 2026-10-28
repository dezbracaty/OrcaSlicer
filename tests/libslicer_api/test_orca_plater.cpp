#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <libslicer/Calibration.hpp>
#include "libslicer_api/Plater.hpp"
#include <libslic3r/Utils.hpp>
#include <libslic3r/GCode/GCodeProcessor.hpp>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <exception>
#include <cstdlib>
#include <iomanip>
#include <algorithm>
#include <tbb/global_control.h>
#include <tbb/task_arena.h>
#ifdef _WIN32
#include <Windows.h>
namespace {
// The DLL and this reference executable each link their own static TBB.
// global_control in the executable cannot constrain the DLL's runtime.
// Constrain later runtime work without changing production code. Persisted
// cross-process baselines additionally launch with affinity before DLL load
// and replay the same test filter/order (see the API documentation).
struct SerialProcess
{
    SerialProcess()
    {
        DWORD_PTR available = 0, system = 0;
        if (!GetProcessAffinityMask(GetCurrentProcess(), &available, &system) || !available ||
            !SetProcessAffinityMask(GetCurrentProcess(), available & (~available + 1)))
            throw std::runtime_error("Cannot establish serial reference scheduling");
    }
} serial_process;
} // namespace
#endif

namespace {
using namespace libslicer;
using namespace Slic3r;
std::string messages(const std::vector<SliceDiagnostic>& entries)
{
    std::string text;
    for (const auto& entry : entries)
        text += entry.code + ": " + entry.message + "\n";
    return text;
}
std::unique_ptr<Library> open_library()
{
    LibraryOptions options;
    options.resource_directory = LIBSLICER_TEST_RESOURCE_DIR;
    options.vendors            = {"Flashforge"};
    auto library               = Library::open(options);
    REQUIRE(library);
    return library;
}
libslicer::Config base_config()
{
    auto config       = libslicer::Config::defaults();
    const auto result = config.apply_patch({{"gcode_flavor", "klipper"},
                                            {"generate_reinforced_perimeters", "0"},
                                            {"generate_reinforced_infills", "0"},
                                            {"printable_area", "0x0,220x0,220x220,0x220"},
                                            {"printable_height", "500"},
                                            {"filament_flow_ratio", ".98"},
                                            {"filament_max_volumetric_speed", "20"},
                                            {"enable_prime_tower", "0"},
                                            {"machine_start_gcode", "G90\nM83"},
                                            {"machine_end_gcode", ""},
                                            {"filament_start_gcode", ""},
                                            {"filament_end_gcode", ""},
                                            {"layer_change_gcode", "G92 E0"},
                                            {"gcode_label_objects", "0"},
                                            {"disable_m73", "1"},
                                            {"thumbnails", ""}});
    for (const auto& d : result.diagnostics)
        UNSCOPED_INFO(d.key << ": " << d.message);
    REQUIRE(result);
    return config;
}
DynamicPrintConfig native_config(const ConfigSnapshot& snapshot)
{
    DynamicPrintConfig config;
    config.apply(FullPrintConfig::defaults());
    for (const auto& item : snapshot.values())
        config.set_deserialize_strict(item.first, item.second);
    return config;
}
std::unique_ptr<CalibrationTask> prepare(Library& library, const CalibrationRequest& request)
{
    auto input = request;
    // Test-only bridge for the preserved DLL from before the public enum was
    // aligned with Orca. Request layout is unchanged; only its mode ordinals
    // differed. This is never enabled by the product or used as an API alias.
    if (std::getenv("LIBSLICER_CALIBRATION_LEGACY_ENUM")) {
        int value = static_cast<int>(request.mode);
        if (value == 5)
            value = 4;
        else if (value >= 6 && value <= 13)
            --value;
        input.mode = static_cast<libslicer::CalibMode>(value);
    }
    auto result = library.prepare_calibration(input);
    INFO(messages(result.diagnostics));
    REQUIRE(result);
    REQUIRE(result.task);
    return std::move(result.task);
}
std::string read_file(const std::string& path)
{
    std::ifstream stream(path);
    REQUIRE(stream.good());
    return {std::istreambuf_iterator<char>(stream), {}};
}
std::vector<std::string> commands(const std::string& gcode)
{
    std::vector<std::string> result;
    std::istringstream stream(gcode);
    for (std::string line; std::getline(stream, line);) {
        line.resize(line.find(';') == std::string::npos ? line.size() : line.find(';'));
        const auto begin = line.find_first_not_of(" \t\r"), end = line.find_last_not_of(" \t\r");
        if (begin != std::string::npos)
            result.push_back(line.substr(begin, end - begin + 1));
    }
    return result;
}
struct Files
{
    std::vector<std::filesystem::path> paths;
    ~Files()
    {
        if (std::uncaught_exceptions() > 0)
            return;
        for (const auto& path : paths) {
            std::error_code ec;
            std::filesystem::remove(path, ec);
        }
    }
};
Calib_Params original_params(const CalibrationRequest& request)
{
    Calib_Params p{};
    p.extruder_id   = request.extruder_id;
    p.start         = request.start;
    p.end           = request.end;
    p.step          = request.step;
    p.print_numbers = request.print_numbers;
    p.freqStartX    = request.freqStartX;
    p.freqEndX      = request.freqEndX;
    p.freqStartY    = request.freqStartY;
    p.freqEndY      = request.freqEndY;
    p.shaper_type   = request.shaper_type;
    p.test_model    = request.test_model;
    p.accelerations.assign(request.accelerations.begin(), request.accelerations.end());
    p.speeds.assign(request.speeds.begin(), request.speeds.end());
    switch (request.mode) {
    case libslicer::CalibMode::Calib_Temp_Tower: p.mode = Slic3r::CalibMode::Calib_Temp_Tower; break;
    case libslicer::CalibMode::Calib_Vol_speed_Tower: p.mode = Slic3r::CalibMode::Calib_Vol_speed_Tower; break;
    case libslicer::CalibMode::Calib_VFA_Tower: p.mode = Slic3r::CalibMode::Calib_VFA_Tower; break;
    case libslicer::CalibMode::Calib_Retraction_tower: p.mode = Slic3r::CalibMode::Calib_Retraction_tower; break;
    case libslicer::CalibMode::Calib_PA_Line: p.mode = Slic3r::CalibMode::Calib_PA_Line; break;
    case libslicer::CalibMode::Calib_PA_Tower: p.mode = Slic3r::CalibMode::Calib_PA_Tower; break;
    case libslicer::CalibMode::Calib_PA_Pattern: p.mode = Slic3r::CalibMode::Calib_PA_Pattern; break;
    case libslicer::CalibMode::Calib_Input_shaping_freq: p.mode = Slic3r::CalibMode::Calib_Input_shaping_freq; break;
    case libslicer::CalibMode::Calib_Input_shaping_damp: p.mode = Slic3r::CalibMode::Calib_Input_shaping_damp; break;
    case libslicer::CalibMode::Calib_Cornering: p.mode = Slic3r::CalibMode::Calib_Cornering; break;
    default: p.mode = Slic3r::CalibMode::Calib_None; break;
    }
    return p;
}
// Preserve the pre-refactor output independently of the production adapter.
// Native CalibMode values stay stable when public SDK names are aligned.
std::string baseline_key(const CalibrationRequest& request)
{
    std::ostringstream key;
    key << std::setprecision(17) << int(original_params(request).mode) << request.flow_linear << request.flow_pass
        << int(request.flow_pattern) << (request.mode == libslicer::CalibMode::Calib_Tolerance_Model) << request.extruder_id << ':'
        << request.start << ':' << request.end << ':' << request.step << ':' << request.print_numbers << ':' << request.freqStartX << ':'
        << request.freqEndX << ':' << request.freqStartY << ':' << request.freqEndY << ':' << request.test_model << ':'
        << request.shaper_type;
    for (auto v : request.accelerations)
        key << ":a" << v;
    for (auto v : request.speeds)
        key << ":s" << v;
    for (const auto& item : request.config.values())
        key << ':' << item.first << '=' << item.second;
    std::uint64_t hash = 14695981039346656037ULL;
    for (unsigned char c : key.str()) {
        hash ^= c;
        hash *= 1099511628211ULL;
    }
    std::ostringstream name;
    name << std::hex << hash;
    return name.str();
}
// Reference: invoke the copied original Plater directly, then its native Print
// path. Candidate: the public DLL wrapper. Compare effective model/config and
// every executable G-code line without numeric or instruction normalization.
void compare_original(Library& library, const CalibrationRequest& request)
{
    INFO("mode=" << static_cast<int>(request.mode));
    auto task = prepare(library, request);
    set_resources_dir(std::filesystem::path(LIBSLICER_TEST_RESOURCE_DIR).make_preferred().string());
    set_data_dir((std::filesystem::temp_directory_path() / "orca-reference").string());
    orca::Plater reference(native_config(request.config));
    auto params = original_params(request);
    if (request.mode == libslicer::CalibMode::Calib_Flow_Rate)
        reference.prepare_flow(request.flow_linear, request.flow_pass,
                               request.flow_pattern == CalibrationRequest::FlowPattern::ArchimedeanChords ? ipArchimedeanChords :
                                                                                                            ipMonotonic);
    else if (request.mode == libslicer::CalibMode::Calib_Tolerance_Model)
        reference.prepare_tolerance();
    else
        reference.prepare(params);
    REQUIRE(task->plates().size() == reference.plate_count());
    Files files;
    for (std::size_t i = 0; i < reference.plate_count(); ++i) {
        const auto config  = reference.full_config();
        auto model         = reference.plate_model(i);
        const auto& actual = task->plates()[i];
        for (const auto& key : config.keys()) {
            INFO(key);
            REQUIRE(actual.config.value(key) == config.option(key)->serialize());
        }
        REQUIRE(actual.objects.size() == model.objects.size());
        for (std::size_t oi = 0; oi < model.objects.size(); ++oi) {
            auto* object         = model.objects[oi];
            const auto& observed = actual.objects[oi];
            REQUIRE(observed.geometry.name == object->name);
            REQUIRE(observed.geometry.volumes.size() == object->volumes.size());
            REQUIRE(object->instances.size() == 1);
            for (std::size_t vi = 0; vi < object->volumes.size(); ++vi) {
                auto mesh = object->volumes[vi]->mesh();
                mesh.transform(object->volumes[vi]->get_matrix());
                const auto& geometry = observed.geometry.volumes[vi];
                REQUIRE(geometry.vertices.size() == mesh.its.vertices.size());
                REQUIRE(geometry.triangles.size() == mesh.its.indices.size());
                for (std::size_t v = 0; v < mesh.its.vertices.size(); ++v) {
                    REQUIRE(geometry.vertices[v].x == mesh.its.vertices[v].x());
                    REQUIRE(geometry.vertices[v].y == mesh.its.vertices[v].y());
                    REQUIRE(geometry.vertices[v].z == mesh.its.vertices[v].z());
                }
                for (std::size_t f = 0; f < mesh.its.indices.size(); ++f) {
                    REQUIRE(geometry.triangles[f].vertex_a == mesh.its.indices[f].x());
                    REQUIRE(geometry.triangles[f].vertex_b == mesh.its.indices[f].y());
                    REQUIRE(geometry.triangles[f].vertex_c == mesh.its.indices[f].z());
                }
            }
            const auto matrix = object->instances.front()->get_matrix();
            for (int row = 0; row < 4; ++row)
                for (int col = 0; col < 4; ++col)
                    REQUIRE(observed.geometry.transform[row * 4 + col] == matrix(row, col));
            for (const auto& option : observed.object_config)
                REQUIRE(object->config.get().option(option.first)->serialize() == option.second);
        }
        CalibrationOutputOptions output;
        output.generate_preview = true;
        const auto sliced       = library.slice_calibration(*task, i, output);
        INFO(messages(sliced.diagnostics));
        REQUIRE(sliced);
        REQUIRE(sliced.preview);
        REQUIRE_FALSE(sliced.preview->segments.empty());
        files.paths.push_back(sliced.output.path);
        INFO("candidate=" << sliced.output.path);
        Slic3r::Print print;
        print.set_plate_origin(Vec3d::Zero());
        print.is_BBL_printer() = reference.presets().is_bbl_vendor();
        print.apply(model, config);
        print.set_calib_params(reference.print_params());
        Slic3r::StringObjectException warning;
        const auto error = print.validate(&warning);
        INFO(error.string);
        REQUIRE(error.string.empty());
        print.process();
        Slic3r::GCodeProcessorResult processor_result;
        const auto path = sliced.output.path + ".original.gcode";
        files.paths.push_back(path);
        print.export_gcode(path, &processor_result);
        const auto expected = commands(read_file(path)), observed = commands(read_file(sliced.output.path));
        const auto fixture = baseline_key(request) + "-" + std::to_string(i) + ".commands";
        if (const auto* directory = std::getenv("LIBSLICER_CALIBRATION_WRITE_BASELINE")) {
            std::filesystem::create_directories(directory);
            std::ofstream saved(std::filesystem::path(directory) / fixture);
            for (const auto& command : observed)
                saved << command << '\n';
            REQUIRE(saved.good());
        }
        if (const auto* directory = std::getenv("LIBSLICER_CALIBRATION_COMPARE_BASELINE")) {
            const auto baseline = commands(read_file((std::filesystem::path(directory) / fixture).string()));
            INFO("pre-refactor baseline=" << fixture);
            REQUIRE(observed.size() == baseline.size());
            for (std::size_t line = 0; line < baseline.size(); ++line) {
                INFO("baseline line=" << line);
                REQUIRE(observed[line] == baseline[line]);
            }
        }
        if (request.mode == libslicer::CalibMode::Calib_Retraction_tower) {
            Slic3r::Print repeated;
            repeated.set_plate_origin(Vec3d::Zero());
            repeated.is_BBL_printer() = reference.presets().is_bbl_vendor();
            repeated.apply(model, config);
            repeated.set_calib_params(reference.print_params());
            repeated.validate(&warning);
            repeated.process();
            const auto repeat_path = path + ".repeat.gcode";
            files.paths.push_back(repeat_path);
            repeated.export_gcode(repeat_path, &processor_result);
            const auto repeat = commands(read_file(repeat_path));
            INFO("original Print repeated with exactly the same native model and config");
            REQUIRE(repeat == expected);
        }
        REQUIRE(observed.size() == expected.size());
        for (std::size_t line = 0; line < expected.size(); ++line) {
            INFO("plate=" << i << " line=" << line);
            REQUIRE(observed[line] == expected[line]);
        }
    }
}
} // namespace

TEST_CASE("original Plater entries and public calibration emit identical commands", "[orca_plater][slice]")
{
    tbb::global_control threads(tbb::global_control::max_allowed_parallelism, 1);
    tbb::task_arena arena(1);
    arena.execute([&] {
        auto library = open_library();
        auto config  = base_config();
        CalibrationRequest r;
        r.config = config.snapshot();
        SECTION("temperature accepts original non-five-degree endpoints")
        {
            r.mode  = libslicer::CalibMode::Calib_Temp_Tower;
            r.start = 233;
            r.end   = 222;
        }
        SECTION("maximum volumetric speed")
        {
            r.mode  = libslicer::CalibMode::Calib_Vol_speed_Tower;
            r.start = 5;
            r.end   = 6.2;
            r.step  = .5;
        }
        SECTION("VFA")
        {
            r.mode  = libslicer::CalibMode::Calib_VFA_Tower;
            r.start = 40;
            r.end   = 62;
            r.step  = 10;
        }
        SECTION("retraction")
        {
            r.mode  = libslicer::CalibMode::Calib_Retraction_tower;
            r.start = 0;
            r.end   = .25;
            r.step  = .1;
        }
        SECTION("PA Line preserves the original ceil endpoint")
        {
            r.mode  = libslicer::CalibMode::Calib_PA_Line;
            r.start = 0;
            r.end   = .07;
            r.step  = .01;
        }
        SECTION("PA Tower")
        {
            r.mode  = libslicer::CalibMode::Calib_PA_Tower;
            r.start = 0;
            r.end   = .02;
            r.step  = .01;
        }
        SECTION("PA Pattern")
        {
            r.mode  = libslicer::CalibMode::Calib_PA_Pattern;
            r.start = 0;
            r.end   = .025;
            r.step  = .01;
        }
        SECTION("flow coarse")
        {
            r.mode        = libslicer::CalibMode::Calib_Flow_Rate;
            r.flow_linear = false;
            r.flow_pass   = 1;
        }
        SECTION("flow fine")
        {
            r.mode        = libslicer::CalibMode::Calib_Flow_Rate;
            r.flow_linear = false;
            r.flow_pass   = 2;
        }
        SECTION("flow YOLO")
        {
            r.mode        = libslicer::CalibMode::Calib_Flow_Rate;
            r.flow_linear = true;
            r.flow_pass   = 1;
        }
        SECTION("flow YOLO fine")
        {
            r.mode        = libslicer::CalibMode::Calib_Flow_Rate;
            r.flow_linear = true;
            r.flow_pass   = 2;
        }
        SECTION("input frequency")
        {
            r.mode        = libslicer::CalibMode::Calib_Input_shaping_freq;
            r.start       = .1;
            r.freqStartX  = 20;
            r.freqEndX    = 80;
            r.freqStartY  = 30;
            r.freqEndY    = 90;
            r.shaper_type = "MZV";
            r.test_model  = 1;
        }
        SECTION("input damping includes zero")
        {
            r.mode        = libslicer::CalibMode::Calib_Input_shaping_damp;
            r.start       = 0;
            r.end         = .3;
            r.freqStartX  = 40;
            r.freqStartY  = 45;
            r.shaper_type = "MZV";
            r.test_model  = 1;
        }
        SECTION("cornering includes zero")
        {
            r.mode       = libslicer::CalibMode::Calib_Cornering;
            r.start      = 0;
            r.end        = 10;
            r.test_model = 1;
        }
        SECTION("tolerance model") { r.mode = libslicer::CalibMode::Calib_Tolerance_Model; }
        REQUIRE(r.mode != libslicer::CalibMode::Calib_None);
        compare_original(*library, r);
    });
}

TEST_CASE("original PA Pattern keeps all arranged plates", "[orca_plater][slice]")
{
    auto library = open_library();
    auto config  = base_config();
    REQUIRE(config.set("printable_area", "0x0,100x0,100x100,0x100"));
    CalibrationRequest r;
    r.config        = config.snapshot();
    r.mode          = libslicer::CalibMode::Calib_PA_Pattern;
    r.start         = 0;
    r.end           = .1;
    r.step          = .01;
    r.speeds        = {60, 80};
    r.accelerations = {1000, 2000};
    auto task       = prepare(*library, r);
    REQUIRE(task->plates().size() > 1);
    std::size_t objects = 0;
    for (const auto& plate : task->plates())
        objects += plate.objects.size();
    CHECK(objects == 4);
    compare_original(*library, r);
}

TEST_CASE("calibration transport uses original dialog rules without product limits", "[orca_plater]")
{
    auto library     = open_library();
    const auto modes = library->calibration_modes();
    REQUIRE(modes.size() == 12);
    CHECK(std::find(modes.begin(), modes.end(), libslicer::CalibMode::Calib_Auto_PA_Line) == modes.end());
    CHECK(std::find(modes.begin(), modes.end(), libslicer::CalibMode::Calib_None) == modes.end());
    auto config = base_config();
    CalibrationRequest r;
    r.config = config.snapshot();
    r.mode   = libslicer::CalibMode::Calib_PA_Tower;
    r.start  = 0;
    r.end    = 2.1;
    r.step   = .1;
    CHECK(library->prepare_calibration(r));
    r.mode  = libslicer::CalibMode::Calib_Vol_speed_Tower;
    r.start = 201;
    r.end   = 202;
    r.step  = .5;
    CHECK(library->prepare_calibration(r));
    r.mode  = libslicer::CalibMode::Calib_Retraction_tower;
    r.start = 20;
    r.end   = 21;
    r.step  = .5;
    CHECK(library->prepare_calibration(r));
    r.mode  = libslicer::CalibMode::Calib_Temp_Tower;
    r.start = 233;
    r.end   = 192;
    CHECK(library->prepare_calibration(r));
    r.mode  = libslicer::CalibMode::Calib_PA_Line;
    r.start = 0;
    r.end   = .1;
    r.step  = 0;
    CHECK_FALSE(library->prepare_calibration(r));
    SliceCallbacks cancelled;
    cancelled.is_cancelled = [] { return true; };
    CHECK(library->prepare_calibration(r, cancelled).cancelled);
    r.step    = .01;
    auto task = prepare(*library, r);
    CHECK(library->slice_calibration(*task, 0, {}, cancelled).cancelled);
    CHECK_FALSE(library->slice_calibration(*task, 100, {}));
    CalibrationOutputOptions output;
    output.generate_gcode_3mf = true;
    auto sliced               = library->slice_calibration(*task, 0, output);
    INFO(messages(sliced.diagnostics));
    REQUIRE(sliced);
    Files files;
    files.paths = {sliced.output.path, sliced.gcode_3mf.path};
    REQUIRE(std::filesystem::file_size(sliced.gcode_3mf.path) > 0);
}

namespace {
const libslicer::SettingItem& parameter(const libslicer::CalibrationParameters& p, const char* key)
{
    const auto it = std::find_if(p.fields.begin(), p.fields.end(), [&](const auto& f) { return f.key == key; });
    if (it == p.fields.end()) throw std::runtime_error(std::string("Missing field: ") + key);
    return *it;
}
}

TEST_CASE("calibration forms expose original defaults and firmware options without a GUI", "[calibration-parameters]")
{
    using M = libslicer::CalibMode;
    const std::vector<M> modes = {M::Calib_PA_Line,M::Calib_PA_Pattern,M::Calib_PA_Tower,M::Calib_Flow_Rate,
        M::Calib_Temp_Tower,M::Calib_Vol_speed_Tower,M::Calib_VFA_Tower,M::Calib_Retraction_tower,
        M::Calib_Input_shaping_freq,M::Calib_Input_shaping_damp,M::Calib_Cornering,M::Calib_Tolerance_Model};
    auto config = libslicer::Config::defaults();
    for (const char* firmware : {"klipper", "reprapfirmware", "marlin2", "marlin"}) {
        REQUIRE(config.apply_patch({{"gcode_flavor",firmware},{"machine_max_junction_deviation","0.02"}}));
        const auto snapshot = config.snapshot();
        for (const auto mode : modes) {
            INFO(firmware << " mode=" << int(mode));
            const auto p = libslicer::calibration_parameters(mode, snapshot);
            const auto checked = libslicer::validate_calibration_parameters(p);
            INFO(messages(checked.diagnostics));
            REQUIRE(checked);
            CHECK(checked.request.mode == mode);
            CHECK(checked.request.config.values() == snapshot.values());
            if (mode == M::Calib_Temp_Tower) {
                CHECK(parameter(p,"start").value == "230");
                CHECK(parameter(p,"end").value == "190");
                CHECK(parameter(p,"step").value == "5");
                CHECK_FALSE(parameter(p,"step").enabled);
            }
            if (mode == M::Calib_Input_shaping_freq) {
                const auto& shapers = parameter(p,"shaper_type").enum_items;
                CHECK(shapers.size() == (std::string(firmware)=="klipper"?7:std::string(firmware)=="reprapfirmware"?8:1));
                CHECK(parameter(p,"freqStartY").enabled == (std::string(firmware)!="reprapfirmware"));
                CHECK(parameter(p,"freqStartX").unit == "Hz");
                CHECK(checked.request.freqStartX == 15);
                CHECK(checked.request.freqEndX == 110);
            }
            if (mode == M::Calib_Cornering) {
                const bool jd = std::string(firmware)=="marlin2";
                CHECK(parameter(p,"start").value == (jd?"0.000":"1.000"));
                CHECK(parameter(p,"end").value == (jd?"0.250":"15.000"));
                CHECK(parameter(p,"start").unit == (jd?"mm":"mm/s"));
            }
        }
        CHECK(config.snapshot().values() == snapshot.values());
    }
    CHECK_THROWS(libslicer::calibration_parameters(M::Calib_Auto_PA_Line,config.snapshot()));
    CHECK_THROWS(libslicer::calibration_parameters(M::Calib_None,config.snapshot()));
}

TEST_CASE("calibration field events preserve original resets and temperature focus semantics", "[calibration-parameters]")
{
    using M = libslicer::CalibMode;
    auto config = libslicer::Config::defaults();
    auto p = libslicer::calibration_parameters(M::Calib_PA_Tower, config.snapshot());
    CHECK(parameter(p,"step").value == "0.002");
    CHECK(parameter(p,"print_numbers").value == "0");
    CHECK_FALSE(parameter(p,"print_numbers").enabled);
    p = libslicer::update_calibration_parameters(p,"pa_method","1");
    CHECK(parameter(p,"print_numbers").enabled);
    p = libslicer::update_calibration_parameters(p,"end","0.07");
    p = libslicer::update_calibration_parameters(p,"pa_method","1");
    CHECK(parameter(p,"end").value == "0.07"); // No selection change, no reset.
    p = libslicer::update_calibration_parameters(p,"accelerations","1000,2000");
    p = libslicer::update_calibration_parameters(p,"pa_method","2");
    CHECK(p.mode == M::Calib_PA_Pattern);
    CHECK(parameter(p,"end").value == "0.08");
    CHECK(parameter(p,"step").value == "0.005");
    CHECK(parameter(p,"accelerations").value == "1000,2000");
    CHECK_FALSE(parameter(p,"print_numbers").enabled);
    p = libslicer::update_calibration_parameters(p,"extruder_type","1");
    CHECK(parameter(p,"end").value == "1.0");
    CHECK(parameter(p,"step").value == "0.05");
    p = libslicer::update_calibration_parameters(p,"end","0.33");
    std::vector<std::pair<std::string,std::string>> saved;
    for (const auto& f : p.fields) saved.emplace_back(f.key,f.value);
    const auto restored=libslicer::calibration_parameters(p.mode,config.snapshot(),saved);
    CHECK(parameter(restored,"extruder_type").value=="1");
    CHECK(parameter(restored,"end").value=="0.33");
    CHECK(parameter(restored,"step").value=="0.05");
    p = libslicer::update_calibration_parameters(p,"pa_method","0");
    CHECK(parameter(p,"step").value == "0.02");

    p = libslicer::calibration_parameters(M::Calib_Temp_Tower,config.snapshot());
    const int starts[] = {230,270,250,280,240,320,320,230};
    const int ends[] = {190,230,230,240,210,280,280,190};
    for (int material=0;material<8;++material) {
        p=libslicer::update_calibration_parameters(p,"filament_type",std::to_string(material));
        CHECK(parameter(p,"start").value==std::to_string(starts[material]));
        CHECK(parameter(p,"end").value==std::to_string(ends[material]));
    }
    p=libslicer::update_calibration_parameters(p,"start","233");
    CHECK(libslicer::validate_calibration_parameters(p).request.start==233);
    p=libslicer::update_calibration_parameters(p,"start","233",true);
    CHECK(parameter(p,"start").value=="230");
    p=libslicer::update_calibration_parameters(p,"end","152",true);
    CHECK(parameter(p,"end").value=="155");
    REQUIRE(p.diagnostics.size()==1);CHECK(p.diagnostics.front().warning);
    p=libslicer::update_calibration_parameters(p,"start","501",true);
    CHECK(parameter(p,"start").value=="500");
    CHECK_FALSE(p.diagnostics.empty());
    for (const char* text : {"", "0.", "-1", "nan", "231.0", "23x"}) {
        const auto partial=libslicer::update_calibration_parameters(p,"start",text,true);
        CHECK(parameter(partial,"start").value==text);
        CHECK_FALSE(libslicer::validate_calibration_parameters(partial));
    }
    CHECK_THROWS(libslicer::update_calibration_parameters(p,"misspelled_key","1"));
}

TEST_CASE("calibration forms own flow mapping RRF linkage and preflight warnings", "[calibration-parameters]")
{
    using M=libslicer::CalibMode;
    auto config=libslicer::Config::defaults();
    auto p=libslicer::calibration_parameters(M::Calib_Flow_Rate,config.snapshot());
    CHECK(parameter(p,"flow_type").value=="2");CHECK(p.can_refine);
    for(int type=0;type<4;++type) {
        p=libslicer::update_calibration_parameters(p,"flow_type",std::to_string(type));
        const auto checked=libslicer::validate_calibration_parameters(p);REQUIRE(checked);
        CHECK(checked.request.flow_linear==(type>=2));CHECK(checked.request.flow_pass==type%2+1);
        CHECK(p.can_refine==(type==0||type==2));
        if(p.can_refine) {
            auto next=libslicer::update_calibration_parameters(p,"next_pass","");
            CHECK(parameter(next,"flow_type").value==std::to_string(type+1));CHECK_FALSE(next.can_refine);
        } else CHECK_THROWS(libslicer::update_calibration_parameters(p,"next_pass",""));
    }
    REQUIRE(config.set("gcode_flavor","reprapfirmware"));
    p=libslicer::calibration_parameters(M::Calib_Input_shaping_freq,config.snapshot());
    p=libslicer::update_calibration_parameters(p,"freqStartX","22");
    CHECK(parameter(p,"freqStartY").value=="22");CHECK(parameter(p,"freqStartY").read_only);
    REQUIRE(libslicer::validate_calibration_parameters(p));
    p=libslicer::update_calibration_parameters(p,"shaper_type","Disable");
    CHECK_FALSE(libslicer::validate_calibration_parameters(p));
    REQUIRE(config.apply_patch({{"gcode_flavor","marlin2"},{"machine_max_junction_deviation","0.02"}}));
    p=libslicer::calibration_parameters(M::Calib_Cornering,config.snapshot());
    p=libslicer::update_calibration_parameters(p,"end","0.26");
    const auto warning=libslicer::validate_calibration_parameters(p);
    REQUIRE(warning);REQUIRE(warning.diagnostics.size()==1);CHECK(warning.diagnostics.front().warning);
    p=libslicer::update_calibration_parameters(p,"end","0.31");CHECK_FALSE(libslicer::validate_calibration_parameters(p));
}

TEST_CASE("calibration form preflight and task preparation share range validation", "[calibration-parameters]")
{
    using M=libslicer::CalibMode;
    auto library=open_library();auto config=base_config();
    for(const auto mode : {M::Calib_PA_Line,M::Calib_Vol_speed_Tower,M::Calib_VFA_Tower,
                          M::Calib_Retraction_tower,M::Calib_Temp_Tower,M::Calib_Input_shaping_freq,M::Calib_Input_shaping_damp,M::Calib_Cornering}) {
        auto p=libslicer::calibration_parameters(mode,config.snapshot());
        // Finite parsed input, invalid for every original calibration range.
        p=libslicer::update_calibration_parameters(p,"start","-1");
        const auto checked=libslicer::validate_calibration_parameters(p);
        INFO(int(mode));REQUIRE_FALSE(checked);REQUIRE_FALSE(checked.diagnostics.empty());
        const auto prepared=library->prepare_calibration(checked.request);
        REQUIRE_FALSE(prepared);REQUIRE_FALSE(prepared.diagnostics.empty());
        CHECK(prepared.diagnostics.front().message==checked.diagnostics.front().message);
    }
}
