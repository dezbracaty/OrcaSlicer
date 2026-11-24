#include "OrcaCalibrationInternal.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/ContinuousFiber/ContinuousFiberConfig.hpp"
#include "libslic3r/ContinuousFiber/PreparedFiberPath.hpp"
#include "libslic3r/Flow.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Triangulation.hpp"
#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <iterator>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <tuple>
#include <boost/nowide/fstream.hpp>

namespace libslicer::fiber_calibration {
namespace {
using namespace Slic3r;
constexpr const char* heater_gate = "Fiber heater waiting is not verified for this machine";
constexpr const char* temperature_sweep_error =
    "Fiber temperature calibration requires descending temperatures and at least two test values";
constexpr const char* base_slot_error = "Select a thermoplastic outer wall material for fiber temperature calibration";
// Leave room for the wait band within the supported Alpha500 T1 temperature limits.
constexpr int minimum_temperature_c = 21;
constexpr int maximum_temperature_c = 399;
constexpr double label_height_in_strokes = 7;
constexpr double minimum_trial_spacing_mm = 2;
constexpr std::array<const char*, 2> wall_width_keys{"inner_wall_line_width", "outer_wall_line_width"};
constexpr std::array<const char*, 4> keys{"height", "length", "speed", "extra"};
constexpr std::array<const char*, 4> labels{"Fiber landing height", "Fiber landing length", "Fiber landing speed", "Fiber extra prefeed"};
constexpr std::array<const char*, 4> config_keys{"fiber_z_hop_height", "fiber_landing_length", "fiber_landing_speed", "fiber_prefeed_extra_length"};
constexpr std::array<const char*, 4> units{"mm", "mm", "mm/s", "mm"};
constexpr std::array<double FiberLandingValues::*, 4> members{
    &FiberLandingValues::height_mm, &FiberLandingValues::length_mm,
    &FiberLandingValues::speed_mm_s, &FiberLandingValues::extra_prefeed_mm};

DynamicPrintConfig native_config(const ConfigSnapshot& snapshot)
{
    if (!snapshot.valid()) throw std::invalid_argument("Calibration has no configuration");
    DynamicPrintConfig config;
    config.apply(FullPrintConfig::defaults());
    for (const auto& item : snapshot.values())
        if (print_config_def.get(item.first)) config.set_deserialize_strict(item.first, item.second);
    return config;
}

std::size_t index(std::string_view text)
{
    std::size_t value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size()) throw std::invalid_argument("Invalid fiber calibration selection");
    return value;
}

int64_t grid(double value)
{
    if (!std::isfinite(value) || value < 0 || value > 1e6)
        throw std::invalid_argument("Invalid fiber calibration value");
    const auto ticks = std::llround(value * 1000);
    if (std::abs(value * 1000 - double(ticks)) > 1e-6)
        throw std::invalid_argument("Fiber calibration values allow at most three decimal places");
    return ticks;
}

double parse_number(const std::string& text)
{
    std::istringstream input(text);
    input.imbue(std::locale::classic());
    double value = 0;
    if (!(input >> value) || !(input >> std::ws).eof()) throw std::invalid_argument("Invalid fiber calibration value");
    return value;
}

double read_number(const std::string& text) { return double(grid(parse_number(text))) / 1000; }

int read_temperature(const std::string& text)
{
    int value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size() || value <= 0)
        throw std::invalid_argument("Fiber temperatures and step must be positive integers");
    return value;
}

std::size_t temperature_base_slot(const DynamicPrintConfig& config)
{
    const auto& materials = config.option<ConfigOptionStrings>("filament_process_type")->values;
    const int wall_material = config.opt_int("outer_wall_filament_id");
    if (wall_material > 0) {
        const auto slot = std::size_t(wall_material - 1);
        if (slot < materials.size() && materials[slot] == "thermoplastic") return slot;
        throw std::invalid_argument(base_slot_error);
    }
    // The normal default material for an unassigned object is its first slot.
    if (!materials.empty() && materials[0] == "thermoplastic") return 0;
    const auto base = std::find(materials.begin(), materials.end(), "thermoplastic");
    if (base != materials.end() && std::find(std::next(base), materials.end(), "thermoplastic") == materials.end())
        return std::size_t(base - materials.begin());
    throw std::invalid_argument(base_slot_error);
}

std::pair<int, int> default_temperature_range(const DynamicPrintConfig& config, std::size_t material_slot)
{
    const auto* low = config.option<ConfigOptionInts>("nozzle_temperature_range_low");
    const auto* high = config.option<ConfigOptionInts>("nozzle_temperature_range_high");
    if (material_slot >= low->values.size() || material_slot >= high->values.size())
        return {0, 0};
    return {std::max(minimum_temperature_c, low->values[material_slot]),
        std::min(maximum_temperature_c, high->values[material_slot])};
}

std::string numeric_label(int64_t ticks)
{
    auto label = std::to_string(ticks / 1000);
    if (ticks % 1000 != 0) {
        auto fraction = std::to_string(1000 + ticks % 1000).substr(1);
        while (fraction.back() == '0') fraction.pop_back();
        label += '.' + fraction;
    }
    return label;
}

int glyph_width(char glyph) { return glyph == '.' ? 2 : 4; }

double label_width(const std::string& label, double stroke)
{
    int width = -1;
    for (char glyph : label) width += glyph_width(glyph) + 1;
    return width * stroke;
}

void append_label(Polygons& strokes, const std::string& label, const Vec2d& right_center, double stroke)
{
    // Seven-segment numerals with strokes sized for the base nozzle. Give
    // the decimal point a short printable bead instead of a square island.
    constexpr std::array<unsigned, 10> digits{0x3f, 0x06, 0x5b, 0x4f, 0x66, 0x6d, 0x7d, 0x07, 0x7f, 0x6f};
    constexpr std::array<std::array<int, 4>, 7> segments{{
        {0, 6, 4, 7}, {3, 3, 4, 7}, {3, 0, 4, 4}, {0, 0, 4, 1},
        {0, 0, 1, 4}, {0, 3, 1, 7}, {0, 3, 4, 4}}};
    double x = right_center.x() - label_width(label, stroke);
    const double y = right_center.y() - 0.5 * label_height_in_strokes * stroke;
    const auto rectangle = [&](const std::array<int, 4>& box) {
        const auto [left, bottom, right, top] = box;
        Polygon shape;
        for (const auto& [u, v] : {std::pair{left, bottom}, std::pair{right, bottom},
                                  std::pair{right, top}, std::pair{left, top}})
            shape.points.push_back(Point::new_scale(Vec2d(x + u * stroke, y + v * stroke)));
        strokes.push_back(std::move(shape));
    };
    for (char glyph : label) {
        if (glyph == '.') rectangle({0, 0, glyph_width(glyph), 1});
        else {
            const auto mask = digits[glyph - '0'];
            for (std::size_t i = 0; i < segments.size(); ++i)
                if (mask & (1u << i)) rectangle(segments[i]);
        }
        x += (glyph_width(glyph) + 1) * stroke;
    }
}

struct Layout {
    std::shared_ptr<FiberCalibrationPlan> plan;
    std::vector<FiberCalibrationSample> samples;
    std::array<Vec2d, 5> corners;
    std::vector<std::string> labels;
    std::vector<double> label_column_offsets_mm;
    std::array<double, wall_width_keys.size()> wall_widths_mm{};
    double label_stroke_mm = 0;
    double finish_reserve_mm = 0;
};

struct TurnRange {
    int64_t first, step, count;
};

TurnRange turn_range(const FiberTurnSweep& sweep, bool flow)
{
    const int64_t first = grid(sweep.start);
    const int64_t last = grid(sweep.end);
    const int64_t step = grid(sweep.step);
    const int64_t span = flow ? last - first : first - last;
    if (!first || !last || !step || span < step)
        throw std::invalid_argument(flow ? "Use ascending positive percentages and at least two test values" :
            "Use descending positive radii and at least two test values");
    return {first, flow ? step : -step, span / step + 1};
}

Layout layout(const FiberCalibrationInput& input, const DynamicPrintConfig& config, CalibMode mode)
{
    const auto& sweep = input.sweep;
    const auto& temperature = input.temperature;
    const auto& turn = input.turn;
    const bool turn_flow = mode == CalibMode::Calib_Fiber_Turn_Flow;
    const bool turn_radius = mode == CalibMode::Calib_Fiber_Turn_Radius;
    if (temperature && config.opt_string("printer_model") != "CFSYS Alpha500 Printer")
        throw std::invalid_argument(heater_gate);
    const auto parameter = static_cast<std::size_t>(sweep.parameter);
    if (parameter >= keys.size()) throw std::invalid_argument("Invalid fiber landing parameter");
    const auto& types = config.option<ConfigOptionStrings>("filament_process_type")->values;
    if (input.base_filament_slot >= types.size() || input.fiber_filament_slot >= types.size() ||
        types[input.base_filament_slot] != "thermoplastic" || types[input.fiber_filament_slot] != "continuous_fiber")
        throw std::invalid_argument("Select a thermoplastic base material and a continuous fiber material");
    if (config.opt_bool("enable_prime_tower"))
        throw std::invalid_argument("Fiber calibration requires a profile that can switch to fiber without a prime tower");
    if (const auto* structure = config.option<ConfigOptionEnum<PrinterStructure>>("printer_structure");
        structure && structure->value == PrinterStructure::psBelt)
        throw std::invalid_argument("Fiber calibration requires a flat build plate");

    PrintConfig print;
    print.apply(config, true);
    validate_material_tool_bindings(print);
    validate_fiber_cut_event(print.fiber_cut_gcode.value, fiber_machine_protocol(print));
    const auto tool = resolve_fiber_tool(print, unsigned(input.fiber_filament_slot));
    int64_t first, step, count;
    FiberLandingValues fixed = sweep.fixed;
    if (temperature) {
        if (fiber_machine_protocol(print) != FiberMachineProtocol::Cfsys || tool.physical_tool_id != 1)
            throw std::invalid_argument(heater_gate);
        if (input.base_filament_slot != temperature_base_slot(config))
            throw std::invalid_argument(base_slot_error);
        if (temperature->start_c < minimum_temperature_c || temperature->start_c > maximum_temperature_c ||
            temperature->end_c < minimum_temperature_c || temperature->end_c > maximum_temperature_c)
            throw std::invalid_argument("Fiber heater temperature limits: " + std::to_string(minimum_temperature_c) +
                "–" + std::to_string(maximum_temperature_c) + " °C");
        const int delta = temperature->start_c - temperature->end_c;
        if (temperature->step_c <= 0 || delta < temperature->step_c)
            throw std::invalid_argument(temperature_sweep_error);
        first = int64_t(temperature->start_c) * 1000;
        step = -int64_t(temperature->step_c) * 1000;
        count = delta / temperature->step_c + 1;
    } else if (turn) {
        const auto range = turn_range(*turn, turn_flow);
        first = range.first;
        step = range.step;
        count = range.count;
    } else {
        first = grid(sweep.start);
        const auto last = grid(sweep.end);
        step = grid(sweep.step);
        if (step == 0 || last - first < step)
            throw std::invalid_argument("Fiber calibration requires at least two values and a positive step");
        count = (last - first) / step + 1;
    }
    if (temperature || turn) {
        for (std::size_t i = 0; i < members.size(); ++i) {
            fixed.*members[i] = config.opt_float(config_keys[i]);
            if (!std::isfinite(fixed.*members[i]) || fixed.*members[i] < 0)
                throw std::invalid_argument("Invalid fiber calibration value");
        }
    }
    const auto base_logical = print.filament_map.get_at(input.base_filament_slot) - 1;
    if (base_logical < 0 || print.physical_extruder_map.get_at(base_logical) == int(tool.physical_tool_id))
        throw std::invalid_argument("Fiber calibration needs separate base and fiber tools");
    const auto fiber_limits = fiber_machine_limits(print, tool.logical_extruder_id);
    const auto base_limits = fiber_machine_limits(print, unsigned(base_logical));
    const double h = config.opt_float("layer_height");
    const double initial_h = config.opt_float("initial_layer_print_height");
    const double fiber_width = config.opt_float("fiber_width");
    const double nozzle = print.nozzle_diameter.get_at(base_logical);
    if (!std::isfinite(h) || !std::isfinite(initial_h) || !std::isfinite(fiber_width) ||
        !std::isfinite(nozzle) || h <= 0 || initial_h <= 0 || fiber_width <= 0 || nozzle <= 0)
        throw std::invalid_argument("Fiber calibration requires positive layer heights, fiber width, and base nozzle diameter");
    if (h < print.min_layer_height.get_at(tool.logical_extruder_id) ||
        h > print.max_layer_height.get_at(tool.logical_extruder_id))
        throw std::invalid_argument("Fiber calibration layer height is outside the fiber tool limits");
    const auto& bed = print.printable_area.values;
    if (bed.size() < 3) throw std::invalid_argument("Fiber calibration has no printable area");
    Vec2d lo = bed.front(), hi = bed.front();
    for (const auto& point : bed) { lo = lo.cwiseMin(point); hi = hi.cwiseMax(point); }
    Layout result;
    for (std::size_t i = 0; i < wall_width_keys.size(); ++i) {
        const double width = Flow::extrusion_width(wall_width_keys[i], config, unsigned(base_logical));
        if (!std::isfinite(width) || width <= 0)
            throw std::invalid_argument("Invalid base wall line width");
        result.wall_widths_mm[i] = width;
    }
    const double base_line_width = *std::max_element(result.wall_widths_mm.begin(), result.wall_widths_mm.end());
    result.label_stroke_mm = std::max(0.7, 1.75 * base_line_width);
    double max_radius = 0, min_radius = 0, pitch = 0;
    if (turn) {
        max_radius = turn_flow ? double(grid(turn->radius_mm)) / 1000 : double(first) / 1000;
        if (max_radius <= 0)
            throw std::invalid_argument("Fiber turn radius must be positive");
        if (turn_flow) {
            const double min_pitch = std::max(2 * max_radius, fiber_width);
            pitch = turn->pitch_mm.value_or(std::ceil(min_pitch * 1000) / 1000);
            if (!std::isfinite(pitch) || pitch < min_pitch)
                throw std::invalid_argument("Fiber turn pitch must fit both the bend diameter and fiber width");
        } else {
            min_radius = double(first + (count - 1) * step) / 1000;
            if (!std::isfinite(turn->group_spacing_mm) || turn->group_spacing_mm < 0)
                throw std::invalid_argument("Fiber radius group spacing must be non-negative");
        }
    }
    const double finish_length = config.opt_float("fiber_finish_extension_length");
    const double minimum_path = config.opt_float("fiber_minimum_path_length");
    const double start_stabilization = config.opt_float("fiber_start_stabilization_length");
    const double cut_budget = print.fiber_cut_to_contact_length.value + config.opt_float("fiber_minimum_effective_length");
    const double turn_leg = turn ? std::max({20., minimum_path / 2 + 2,
        std::max((fixed.height_mm > 0 ? fixed.length_mm : 0) + start_stabilization, cut_budget) +
            config.opt_float("fiber_corner_stabilization_length") + 5}) : 0;
    const auto trial_spacing = temperature || turn ? std::nullopt : sweep.trial_spacing_mm;
    const double spacing = trial_spacing.value_or(
        std::max(3 * fiber_width, (label_height_in_strokes + 2) * result.label_stroke_mm) + pitch);
    std::size_t label_columns = 1;
    if (trial_spacing) {
        if (!std::isfinite(spacing) || spacing <= 0)
            throw std::invalid_argument("Fiber trial spacing must be positive");
        // The manual pitch has a 2 mm floor. Wider strands must still have
        // separate physical footprints after XY scaling.
        const double minimum = std::max(minimum_trial_spacing_mm, fiber_width + 2 * SCALING_FACTOR);
        if (spacing + SCALING_FACTOR < minimum) {
            std::ostringstream message;
            message.imbue(std::locale::classic());
            message << "Minimum fiber trial spacing: " << std::ceil((minimum - SCALING_FACTOR) * 1000) / 1000 << " mm";
            throw std::invalid_argument(message.str());
        }
        // Keep every label centered on its trial. When several labels cannot
        // fit in one column at printable stroke width, alternate their X positions.
        const double single_column_pitch = (label_height_in_strokes + 1) * base_line_width;
        label_columns = std::size_t(std::clamp(
            std::ceil((single_column_pitch - SCALING_FACTOR) / spacing), 1.0, double(count)));
        const double available_stroke = (double(label_columns) * spacing - base_line_width) / label_height_in_strokes;
        result.label_stroke_mm = std::clamp(available_stroke, base_line_width, result.label_stroke_mm);
    }
    if (turn_radius) {
        const double minimum_label_pitch = 2 * min_radius + fiber_width + turn->group_spacing_mm;
        label_columns = std::size_t(std::clamp(
            std::ceil((label_height_in_strokes * result.label_stroke_mm + base_line_width) /
                minimum_label_pitch), 1., double(count)));
    }
    const double margin = std::max({5.0, 2 * fiber_width, 5.5 * result.label_stroke_mm});
    const double width = 2 * margin + (turn_radius ?
        (max_radius + min_radius) * double(count) +
            (fiber_width + turn->group_spacing_mm) * double(count - 1) :
        spacing * double(count - 1) + pitch);
    if (!(width + 2 < hi.y() - lo.y())) throw std::invalid_argument("Fiber calibration samples do not fit the build plate");
    result.plan = std::make_shared<FiberCalibrationPlan>();
    auto& plan = *result.plan;
    plan.base_slot = unsigned(input.base_filament_slot);
    plan.fiber_slot = unsigned(input.fiber_filament_slot);
    plan.filament_map = print.filament_map.values;
    plan.layer_height_mm = h;
    plan.base_top_mm = initial_h + 2 * h;
    if (temperature)
        // Keep adjacent temperature bands disjoint even with a 1 °C step.
        plan.temperature = FiberCalibrationPlan::TemperatureControl{
            tool.physical_tool_id, "extruder1", std::min(10, 4 * temperature->step_c)};
    double max_landing = 0;
    auto& column_offsets = result.label_column_offsets_mm;
    column_offsets.resize(label_columns);
    for (int64_t i = 0; i < count; ++i) {
        const auto ticks = first + i * step;
        const double value = double(ticks) / 1000;
        auto landing = fixed;
        if (!temperature && !turn) {
            landing.*members[parameter] = value;
            for (const auto member : members) (void)grid(landing.*member);
        }
        if ((!temperature && !turn && (parameter == 1 || parameter == 2) && landing.height_mm <= 0) ||
            (landing.height_mm > 0 && landing.speed_mm_s <= 0))
            throw std::invalid_argument("Landing length or speed calibration requires positive height and descent speed");
        if (fiber_limits->maximum_z_mm > 0 && plan.base_top_mm + h + print.z_offset.value + landing.height_mm > fiber_limits->maximum_z_mm)
            throw std::invalid_argument("Fiber calibration height exceeds the fiber tool limits");
        if (landing.height_mm > 0) max_landing = std::max(max_landing, landing.length_mm);
        result.labels.push_back(numeric_label(ticks));
        auto& column_width = column_offsets[std::size_t(i) % label_columns];
        column_width = std::max(column_width, label_width(result.labels.back(), result.label_stroke_mm));
        result.samples.push_back({std::size_t(i + 1), value,
            temperature ? "°C" : turn ? (turn_flow ? "%" : "mm") : units[parameter], landing, {}, {},
            turn ? std::optional<double>{turn_flow ? value / 100 : value} : std::nullopt});
    }

    const double line_length = turn ? turn_leg :
        std::max({20., minimum_path + 2, max_landing + start_stabilization + cut_budget + 5});
    if (turn)
        result.finish_reserve_mm = finish_length;
    double labels_width = 0;
    for (std::size_t column = 0; column < label_columns; ++column) {
        auto& offset = column_offsets[column];
        const double width = offset;
        if (column > 0) labels_width += base_line_width;
        offset = labels_width;
        labels_width += width;
    }
    const double label_extension = labels_width + margin;
    const double length = line_length + 2 * margin + label_extension + finish_length + max_radius;
    if (!(length + 2 < hi.x() - lo.x())) throw std::invalid_argument("Fiber calibration samples are too long for the build plate");
    const Vec2d start = (lo + hi - Vec2d(length, width)) / 2;
    const Vec2d stop = start + Vec2d(length, width);
    Polygon envelope;
    for (const Vec2d point : {start, Vec2d(stop.x(), start.y()), stop, Vec2d(start.x(), stop.y())})
        envelope.points.push_back(Point::new_scale(point));
    const auto with_margin = offset_ex(ExPolygons{ExPolygon(envelope)}, scale_(1.0));
    if (!diff_ex(with_margin, base_limits->tip_xy).empty() || !diff_ex(with_margin, fiber_limits->tip_xy).empty())
        throw std::invalid_argument("Fiber calibration model does not fit the shared printable area");
    result.corners = {start + Vec2d(3, 0), Vec2d(stop.x(), start.y()), stop,
                      Vec2d(start.x(), stop.y()), start + Vec2d(0, 3)};
    const double configured_turn_feed = config.opt_float("fiber_infill_turn_feed_multiplier");
    double sample_y = margin;
    for (std::size_t i = 0; i < result.samples.size(); ++i) {
        auto& sample = result.samples[i];
        const Vec2d a = start + Vec2d(margin + label_extension + result.finish_reserve_mm,
            turn_radius ? sample_y : margin + spacing * double(i));
        const auto& landing = sample.landing;
        std::optional<FiberCalibrationPlan::Turn> turn_trial;
        if (turn)
            turn_trial = FiberCalibrationPlan::Turn{turn_flow ? max_radius : sample.value, line_length,
                turn_flow ? *sample.process_value : configured_turn_feed,
                turn_flow ? std::optional<double>{pitch} : std::nullopt};
        Vec2d end = a + Vec2d(line_length, 0);
        if (turn_flow) end = a + Vec2d(0, pitch);
        if (turn_radius) {
            const double diameter = 2 * sample.value;
            end = a + Vec2d(0, diameter);
            // The requested gap is edge-to-edge between different radius groups.
            sample_y += diameter + fiber_width + turn->group_spacing_mm;
        }
        plan.trials.push_back({a, end, landing.height_mm, landing.length_mm, landing.speed_mm_s,
            landing.extra_prefeed_mm, temperature ? int(sample.value) : 0, turn_trial});
        sample.start_mm = {float(a.x()), float(a.y()), float(plan.base_top_mm + h)};
        sample.end_mm = {float(end.x()), float(end.y()), float(plan.base_top_mm + h)};
    }
    return result;
}

TriangleMesh base_mesh(const Layout& layout)
{
    Polygons strokes;
    const Vec2d& first_start = layout.plan->trials.front().start_mm;
    const double margin = first_start.y() - layout.corners[0].y();
    const auto& columns = layout.label_column_offsets_mm;
    for (std::size_t i = 0; i < layout.labels.size(); ++i) {
        const Vec2d& start = layout.plan->trials[i].start_mm;
        const Vec2d position = start - Vec2d(margin + layout.finish_reserve_mm + columns[i % columns.size()], 0);
        append_label(strokes, layout.labels[i], position, layout.label_stroke_mm);
    }
    Points corners;
    for (const auto& corner : layout.corners) corners.push_back(Point::new_scale(corner));
    const Point split_bottom = Point::new_scale(
        Vec2d(first_start.x() - layout.finish_reserve_mm - margin / 2, layout.corners[0].y()));
    const Point split_top(split_bottom.x(), corners[2].y());
    // Match the platform vertices on the foundation's outer edges.
    const Polygon outline({corners[0], split_bottom, corners[1], corners[2], split_top, corners[3], corners[4]});
    const Polygon platform({split_bottom, corners[1], corners[2], split_top});
    const ExPolygons foundation_top{ExPolygon(Polygon({corners[0], split_bottom, split_top, corners[3], corners[4]}))};
    const auto labels = union_ex(strokes);
    if (!diff_ex(labels, foundation_top).empty())
        throw std::invalid_argument("Fiber calibration labels must fit entirely on the foundation");
    const auto exposed_base = diff_ex(foundation_top, labels);
    const double platform_top = layout.plan->base_top_mm;
    const double label_top = platform_top - layout.plan->layer_height_mm;
    const double base_top = label_top - layout.plan->layer_height_mm;

    indexed_triangle_set mesh;
    const auto append_vertices = [&](const Points& points, double z) {
        for (const auto& point : points)
            mesh.vertices.emplace_back(unscale<float>(point.x()), unscale<float>(point.y()), float(z));
    };
    const auto append_cap = [&](const ExPolygons& shape, double z, bool faces_up) {
        const int offset = int(mesh.vertices.size());
        const auto points = to_points(shape);
        append_vertices(points, z);
        for (const auto& triangle : Triangulation::triangulate(shape)) {
            const auto a = points[triangle.x()].cast<double>().eval();
            const auto b = points[triangle.y()].cast<double>().eval();
            const auto c = points[triangle.z()].cast<double>().eval();
            const bool counter_clockwise = cross2(b - a, c - a) > 0;
            if (counter_clockwise == faces_up)
                mesh.indices.emplace_back(offset + triangle.x(), offset + triangle.y(), offset + triangle.z());
            else
                mesh.indices.emplace_back(offset + triangle.x(), offset + triangle.z(), offset + triangle.y());
        }
    };
    const auto append_wall = [&](const Polygon& ring, double from, double to) {
        const int offset = int(mesh.vertices.size());
        const int count = int(ring.points.size());
        append_vertices(ring.points, from);
        append_vertices(ring.points, to);
        for (int i = 0; i < count; ++i) {
            const int a = offset + i, b = offset + (i + 1) % count;
            mesh.indices.emplace_back(a, b, b + count);
            mesh.indices.emplace_back(a, b + count, a + count);
        }
    };
    append_cap(ExPolygons{ExPolygon(outline)}, 0, false);
    append_wall(outline, 0, base_top);
    append_cap(exposed_base, base_top, true);
    append_cap(labels, label_top, true);
    for (const auto& label : labels) {
        append_wall(label.contour, base_top, label_top);
        for (const auto& hole : label.holes) append_wall(hole, base_top, label_top);
    }
    append_wall(platform, base_top, platform_top);
    append_cap(ExPolygons{ExPolygon(platform)}, platform_top, true);
    its_merge_vertices(mesh);
    return TriangleMesh(std::move(mesh));
}
} // namespace

CalibrationParameters parameters(CalibMode mode, const ConfigSnapshot& snapshot,
    const std::vector<std::pair<std::string, std::string>>& values)
{
    CalibrationParameters form;
    form.mode = mode;
    form.config = snapshot;
    const bool temperature = mode == CalibMode::Calib_Fiber_Temperature;
    const bool turn = is_fiber_turn_calibration(mode);
    const bool turn_flow = mode == CalibMode::Calib_Fiber_Turn_Flow;
    auto config = native_config(snapshot);
    const auto add = [&](const char* key, const char* name, std::string value,
                         SettingType type, const char* unit = "") -> SettingItem& {
        SettingItem f;
        f.key = key; f.value = value; f.default_value = std::move(value); f.label = name;
        f.type = type; f.unit = unit;
        form.fields.push_back(std::move(f));
        return form.fields.back();
    };
    const auto& materials = config.option<ConfigOptionStrings>("filament_process_type")->values;
    const auto material_field = [&](const char* key, const char* label, const char* process) {
        auto& f = add(key, label, "", SettingType::Enum);
        for (std::size_t i = 0; i < materials.size(); ++i)
            if (materials[i] == process) f.enum_items.push_back({std::to_string(i), std::to_string(i + 1)});
        if (!f.enum_items.empty()) f.value = f.default_value = f.enum_items.front().value;
        f.enabled = f.enum_items.size() > 1;
    };
    if (!temperature) material_field("base_filament_slot", "Base material", "thermoplastic");
    material_field("fiber_filament_slot", temperature ? "Fiber calibration material" : "Fiber material", "continuous_fiber");
    if (temperature) {
        auto slot_text = form.fields.front().value;
        for (const auto& [key, value] : values)
            if (key == "fiber_filament_slot") slot_text = value;
        int low = 0, high = 0;
        if (!slot_text.empty()) {
            const auto slot = index(slot_text);
            if (slot < materials.size() && materials[slot] == "continuous_fiber")
                std::tie(low, high) = default_temperature_range(config, slot);
        }
        add("start", "Start temperature", high > low ? std::to_string(high) : "", SettingType::Integer, "°C");
        add("end", "End temperature", high > low ? std::to_string(low) : "", SettingType::Integer, "°C");
        add("step", "Temperature step", std::to_string(high > low ? std::min(5, high - low) : 5),
            SettingType::Integer, "°C");
        try { form.fiber_base_filament_slot = temperature_base_slot(config); }
        catch (const std::invalid_argument&) { /* The form validator reports the missing base material. */ }
    } else if (turn) {
        const char* unit = turn_flow ? "%" : "mm";
        const double width = config.opt_float("fiber_width");
        const int64_t end_radius = width > 0 && width <= 2e6 ?
            std::max<int64_t>(400, int64_t(std::ceil(width * 500))) : 500;
        add("start", "Start value", turn_flow ? "90" :
            numeric_label(std::max<int64_t>(1000, end_radius + 200)), SettingType::Float, unit);
        add("end", "End value", turn_flow ? "110" : numeric_label(end_radius), SettingType::Float, unit);
        add("step", "Step", turn_flow ? "5" : "0.1", SettingType::Float, unit);
        if (turn_flow) {
            const double radius = config.opt_float("fiber_infill_bend_radius");
            const std::string initial_radius = radius > 0 && radius <= 1e6 ?
                numeric_label(std::max<int64_t>(1, std::llround(radius * 1000))) : "0.5";
            add("fixed_radius", "Fixed bend radius", initial_radius, SettingType::Float, "mm");
            add("turn_pitch", "Parallel-leg center distance", "", SettingType::Float, "mm").nullable = true;
        } else {
            add("group_spacing", "Radius group spacing", "3", SettingType::Float, "mm");
        }
    } else {
        auto& choice = add("fiber_parameter", "Fiber landing parameter", "1", SettingType::Enum);
        for (std::size_t i = 0; i < keys.size(); ++i) choice.enum_items.push_back({std::to_string(i), labels[i]});
        add("start", "Start value", "1", SettingType::Float);
        add("end", "End value", "3", SettingType::Float);
        add("step", "Step", "0.5", SettingType::Float);
        auto& spacing = add("trial_spacing", "Fiber trial spacing", "", SettingType::Float, "mm");
        spacing.nullable = true;
        for (std::size_t i = 0; i < keys.size(); ++i)
            add(keys[i], labels[i], config.option(config_keys[i])->serialize(), SettingType::Float, units[i]);
    }
    for (const auto& field_value : values) {
        const auto it = std::find_if(form.fields.begin(), form.fields.end(), [&](const auto& f) { return f.key == field_value.first; });
        if (it == form.fields.end()) throw std::invalid_argument("Unknown fiber calibration field: " + field_value.first);
        it->value = field_value.second;
    }
    if (!temperature && !turn) {
        auto selection = std::find_if(form.fields.begin(), form.fields.end(), [](const auto& f) { return f.key == "fiber_parameter"; });
        const auto selected = index(selection->value);
        if (selected >= keys.size()) throw std::invalid_argument("Invalid fiber landing parameter");
        for (auto& f : form.fields) {
            if (f.key == "start" || f.key == "end" || f.key == "step") f.unit = units[selected];
            if (f.key == keys[selected]) f.visible = false;
        }
    }
    const auto checked = validate(form);
    form.diagnostics = checked.diagnostics;
    if (checked) {
        const auto& fiber = *checked.request.fiber;
        if (fiber.temperature) {
            const auto& sweep = *fiber.temperature;
            form.fiber_sample_count = std::size_t((sweep.start_c - sweep.end_c) / sweep.step_c + 1);
        } else if (fiber.turn) {
            form.fiber_sample_count = std::size_t(turn_range(*fiber.turn, turn_flow).count);
        } else {
            const auto& sweep = fiber.sweep;
            form.fiber_sample_count = std::size_t((grid(sweep.end) - grid(sweep.start)) / grid(sweep.step) + 1);
        }
    }
    return form;
}

CalibrationValidationResult validate(const CalibrationParameters& form)
{
    CalibrationValidationResult result;
    try {
        const auto get = [&](std::string_view key) -> const std::string& {
            const auto found = std::find_if(form.fields.begin(), form.fields.end(), [&](const auto& f) { return f.key == key; });
            if (found == form.fields.end()) throw std::invalid_argument("Missing fiber calibration field");
            return found->value;
        };
        const auto config = native_config(form.config);
        if (get("fiber_filament_slot").empty())
            throw std::invalid_argument("No continuous fiber material is available for calibration");
        FiberCalibrationInput input;
        input.fiber_filament_slot = index(get("fiber_filament_slot"));
        if (form.mode == CalibMode::Calib_Fiber_Temperature) {
            input.base_filament_slot = temperature_base_slot(config);
            input.temperature = FiberTemperatureSweep{read_temperature(get("start")),
                read_temperature(get("end")), read_temperature(get("step"))};
        } else {
            input.base_filament_slot = index(get("base_filament_slot"));
        }
        if (is_fiber_turn_calibration(form.mode)) {
            auto& turn = input.turn.emplace();
            turn.start = read_number(get("start"));
            turn.end = read_number(get("end"));
            turn.step = read_number(get("step"));
            if (form.mode == CalibMode::Calib_Fiber_Turn_Flow) {
                turn.radius_mm = read_number(get("fixed_radius"));
                if (!get("turn_pitch").empty()) turn.pitch_mm = read_number(get("turn_pitch"));
            } else {
                turn.group_spacing_mm = read_number(get("group_spacing"));
            }
        } else if (!input.temperature) {
            auto& sweep = input.sweep;
            const auto selected = index(get("fiber_parameter"));
            if (selected >= keys.size()) throw std::invalid_argument("Invalid fiber landing parameter");
            sweep.parameter = static_cast<FiberLandingParameter>(selected);
            sweep.start = read_number(get("start"));
            sweep.end = read_number(get("end"));
            sweep.step = read_number(get("step"));
            const auto& spacing = get("trial_spacing");
            if (!spacing.empty()) sweep.trial_spacing_mm = parse_number(spacing);
            for (std::size_t i = 0; i < keys.size(); ++i)
                if (i != selected) sweep.fixed.*members[i] = read_number(get(keys[i]));
        }
        (void)layout(input, config, form.mode);
        result.request.mode = form.mode;
        result.request.config = form.config;
        result.request.fiber = input;
        result.success = true;
    } catch (const std::exception& e) { result.diagnostics.push_back({"fiber_calibration", e.what(), false}); }
    return result;
}

std::shared_ptr<const FiberCalibrationPlan> prepare(const CalibrationRequest& request,
    DynamicPrintConfig& config, Model& model, std::vector<FiberCalibrationSample>& samples)
{
    if (!request.fiber ||
        request.fiber->temperature.has_value() != (request.mode == CalibMode::Calib_Fiber_Temperature) ||
        request.fiber->turn.has_value() != is_fiber_turn_calibration(request.mode))
        throw std::invalid_argument("Fiber calibration needs a sweep and material slots");
    auto prepared = layout(*request.fiber, config, request.mode);
    // PrintRegion resolves automatic widths by filament slot. Preserve the
    // selected nozzle's widths when its logical tool and slot differ.
    for (std::size_t i = 0; i < wall_width_keys.size(); ++i)
        config.set_key_value(wall_width_keys[i], new ConfigOptionFloatOrPercent(prepared.wall_widths_mm[i], false));
    for (const auto* key : {"generate_reinforced_perimeters", "generate_reinforced_infills", "enable_support",
                            "infill_combination", "spiral_mode", "zaa_enabled"})
        config.set_deserialize_strict(key, "0");
    // Keep variable-width walls in the one-bead strokes of raised labels.
    config.set_deserialize_strict("wall_generator", "arachne");
    const auto& nozzles = config.option<ConfigOptionFloats>("nozzle_diameter")->values;
    const double min_nozzle = *std::min_element(nozzles.begin(), nozzles.end());
    const double wall_width = *std::max_element(prepared.wall_widths_mm.begin(), prepared.wall_widths_mm.end());
    // A Classic profile may carry unused Arachne thresholds that erase the label strokes.
    const auto& defaults = PrintObjectConfig::defaults();
    const double width_ratio = std::min(1.0, wall_width / min_nozzle);
    auto* min_feature = config.option<ConfigOptionPercent>("min_feature_size");
    auto* min_bead = config.option<ConfigOptionPercent>("min_bead_width");
    min_feature->value = std::min(min_feature->value, defaults.min_feature_size.value * width_ratio);
    min_bead->value = std::min(min_bead->value, defaults.min_bead_width.value * width_ratio);
    for (const auto* key : {"raft_layers", "skirt_loops", "top_shell_thickness", "bottom_shell_thickness",
                            "elefant_foot_compensation", "fuzzy_skin_first_layer",
                            "xy_contour_compensation", "xy_hole_compensation", "slice_closing_radius"})
        config.set_deserialize_strict(key, "0");
    config.set_deserialize_strict("slicing_mode", "regular");
    config.set_deserialize_strict("brim_type", "no_brim");
    config.set_deserialize_strict("fuzzy_skin", "none");
    config.set_deserialize_strict("ironing_type", "no ironing");
    config.set_deserialize_strict("top_shell_layers", "3");
    config.set_deserialize_strict("bottom_shell_layers", "3");
    config.set_deserialize_strict("wall_loops", std::to_string(std::max(1, config.opt_int("wall_loops"))));
    config.set_deserialize_strict("sparse_infill_density", "100%");
    config.set_deserialize_strict("print_sequence", "by layer");
    config.set_deserialize_strict("filament_map_mode", "Manual");
    const auto slot = std::to_string(prepared.plan->base_slot + 1);
    for (const auto* key : {"outer_wall_filament_id", "inner_wall_filament_id", "sparse_infill_filament_id",
                           "internal_solid_filament_id", "top_surface_filament_id", "bottom_surface_filament_id"})
        config.set_deserialize_strict(key, slot);
    auto* object = model.add_object("Fiber calibration", "", base_mesh(prepared));
    object->config.set_key_value("extruder", new ConfigOptionInt(int(prepared.plan->base_slot + 1)));
    object->add_instance();
    samples = std::move(prepared.samples);
    return prepared.plan;
}

std::vector<std::uint64_t> read_executions(const std::string& path, std::size_t count,
    const std::function<void()>& check_cancelled)
{
    boost::nowide::ifstream input(path);
    if (!input) throw std::runtime_error("Unable to read fiber calibration output");
    std::vector<std::uint64_t> occurrences;
    std::size_t line_number = 0;
    constexpr std::string_view marker = ";CALIBRATION_FIBER_SAMPLE ";
    for (std::string line; std::getline(input, line);) {
        if (line_number++ % 1024 == 0) check_cancelled();
        if (line.compare(0, marker.size(), marker) != 0) continue;
        std::istringstream row(line.substr(marker.size()));
        std::size_t sample_id = 0;
        std::uint64_t occurrence = 0;
        if (!(row >> sample_id >> occurrence) || sample_id != occurrences.size() + 1 ||
            sample_id > count || occurrence == 0 ||
            (!occurrences.empty() && occurrence <= occurrences.back()))
            throw std::runtime_error("Fiber calibration output changed the sample order");
        occurrences.push_back(occurrence);
    }
    if (input.bad()) throw std::runtime_error("Unable to read fiber calibration output");
    if (occurrences.size() != count) throw std::runtime_error("Fiber calibration output is missing test lines");
    return occurrences;
}
} // namespace libslicer::fiber_calibration
