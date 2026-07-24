#include <libslicer/v1/Slice.hpp>

#include "ProjectInternal.hpp"
#include "RuntimeCoordinator.hpp"
#include "SliceInternal.hpp"

#include "libslic3r/BuildVolume.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/Utils.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace libslicer::v1 {

#ifdef LIBSLICER_SDK_TESTING
namespace detail::testing {
namespace {

struct StageBarrierState {
    struct Slot {
        std::uint64_t generation {0};
        bool active {false};
        bool reached {false};
        bool released {false};
    };

    std::mutex mutex;
    std::condition_variable condition;
    std::array<Slot, 5> slots;
};

StageBarrierState &stage_barrier_state()
{
    static StageBarrierState state;
    return state;
}

std::size_t stage_index(SliceStage stage)
{
    return static_cast<std::size_t>(stage);
}

} // namespace

StageBarrier::StageBarrier(SliceStage stage) : stage_(stage)
{
    auto &state = stage_barrier_state();
    std::lock_guard<std::mutex> lock(state.mutex);
    auto &slot = state.slots[stage_index(stage_)];
    if (slot.active)
        throw std::logic_error(
            "Only one SDK test stage barrier may be active per stage");
    slot.active = true;
    slot.reached = false;
    slot.released = false;
    generation_ = ++slot.generation;
}

StageBarrier::~StageBarrier()
{
    auto &state = stage_barrier_state();
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        auto &slot = state.slots[stage_index(stage_)];
        if (slot.generation != generation_) return;
        slot.released = true;
        slot.active = false;
        ++slot.generation;
    }
    state.condition.notify_all();
}

bool StageBarrier::wait_until_reached(std::chrono::milliseconds timeout)
{
    auto &state = stage_barrier_state();
    std::unique_lock<std::mutex> lock(state.mutex);
    auto &slot = state.slots[stage_index(stage_)];
    state.condition.wait_for(lock, timeout, [&] {
        return slot.generation != generation_ || slot.reached;
    });
    return slot.generation == generation_ && slot.reached;
}

void StageBarrier::release()
{
    auto &state = stage_barrier_state();
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        auto &slot = state.slots[stage_index(stage_)];
        if (slot.generation != generation_) return;
        slot.released = true;
    }
    state.condition.notify_all();
}

void wait_at_stage(SliceStage stage)
{
    auto &state = stage_barrier_state();
    std::unique_lock<std::mutex> lock(state.mutex);
    auto &slot = state.slots[stage_index(stage)];
    if (!slot.active) return;
    const std::uint64_t generation = slot.generation;
    slot.reached = true;
    state.condition.notify_all();
    state.condition.wait(lock, [&] {
        return slot.generation != generation || slot.released;
    });
}

} // namespace detail::testing
#endif

namespace {

template<class T>
Result<T> failure(ErrorCode code, std::string message, std::string field,
                  std::vector<Diagnostic> diagnostics = {})
{
    return detail::ResultAccess::failure<T>(code, std::move(message), std::move(field),
                                            std::move(diagnostics));
}

template<class T, class U>
Result<T> forward_failure(const Result<U> &source)
{
    const auto &diagnostics = source.diagnostics();
    if (diagnostics.empty())
        return failure<T>(source.error_code().value_or(ErrorCode::internal),
                          "Operation failed without a diagnostic", "/internal");
    std::vector<Diagnostic> additional(diagnostics.begin() + 1, diagnostics.end());
    return failure<T>(source.error_code().value_or(diagnostics.front().code),
                      diagnostics.front().message, diagnostics.front().field,
                      std::move(additional));
}

Result<detail::FrozenSliceInput> freeze_slice_input(
    const std::shared_ptr<detail::SliceEngineState> &engine, SliceRequest request)
{
    try {
        if (!request.output.include_gcode &&
            request.output.preview == PreviewDelivery::none)
            return failure<detail::FrozenSliceInput>(
                ErrorCode::invalid_argument,
                "SliceRequest must request G-code, preview, or both", "/output");
        if (request.output.preview == PreviewDelivery::artifact &&
            !request.output.preview_artifact_path)
            return failure<detail::FrozenSliceInput>(
                ErrorCode::invalid_argument,
                "preview_artifact_path is required for artifact preview output",
                "/output/preview_artifact_path");
        std::optional<detail::TemporarySliceSelection> temporary;
        if (request.temporary_selection) {
            temporary.emplace(detail::TemporarySliceSelection{
                std::move(request.temporary_selection->selection),
                std::move(request.temporary_selection->complete_manual_map)});
        }
        SliceOutputOptions output = std::move(request.output);
        auto resolved = detail::resolve_slice_input(engine->context, std::move(request.project),
                                                    request.plate, std::move(temporary));
        if (resolved.has_value()) resolved.value().output = std::move(output);
        return resolved;
    } catch (const std::bad_alloc &) {
        throw;
    } catch (const std::filesystem::filesystem_error &error) {
        return failure<detail::FrozenSliceInput>(ErrorCode::io, error.what(), "/project");
    } catch (const std::exception &error) {
        return failure<detail::FrozenSliceInput>(ErrorCode::internal, error.what(), "/internal");
    } catch (...) {
        return failure<detail::FrozenSliceInput>(
            ErrorCode::internal, "Unclassified exception escaped slice input resolution",
            "/internal");
    }
}

class ActiveReservation {
public:
    explicit ActiveReservation(std::shared_ptr<detail::SliceEngineState> engine)
        : engine_(std::move(engine)) {}

    ActiveReservation(const ActiveReservation &) = delete;
    ActiveReservation &operator=(const ActiveReservation &) = delete;

    ~ActiveReservation()
    {
        if (!transferred_) {
            std::lock_guard<std::mutex> lock(engine_->mutex);
            engine_->active = false;
        }
    }

    void transfer_to_job() noexcept { transferred_ = true; }

private:
    std::shared_ptr<detail::SliceEngineState> engine_;
    bool transferred_ {false};
};

class EventDispatcher {
public:
    EventDispatcher(std::shared_ptr<detail::SliceJobState> state, SliceCallback callback)
        : state_(std::move(state)), callback_(std::move(callback))
    {
        if (callback_)
            thread_ = std::thread([this] { dispatch(); });
    }

    EventDispatcher(const EventDispatcher &) = delete;
    EventDispatcher &operator=(const EventDispatcher &) = delete;

    ~EventDispatcher() { close(); }

    void emit(SliceEventKind kind, int percent, std::optional<Diagnostic> diagnostic = {})
    {
        if (!callback_) return;
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_ || terminal_enqueued_) return;
        percent = std::max(last_percent_, std::min(100, std::max(0, percent)));
        last_percent_ = percent;
        if (is_terminal(kind)) terminal_enqueued_ = true;
        events_.push_back(SliceEvent{kind, percent, std::move(diagnostic)});
        condition_.notify_one();
    }

    void close()
    {
        if (!callback_) return;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (closed_) return;
            closed_ = true;
        }
        condition_.notify_one();
        if (thread_.joinable()) thread_.join();
    }

private:
    static bool is_terminal(SliceEventKind kind)
    {
        return kind == SliceEventKind::completed || kind == SliceEventKind::failed ||
               kind == SliceEventKind::cancelled;
    }

    void dispatch()
    {
        {
            std::lock_guard<std::mutex> state_lock(state_->mutex);
            state_->callback_thread = std::this_thread::get_id();
        }
        for (;;) {
            SliceEvent event;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                condition_.wait(lock, [&] { return closed_ || !events_.empty(); });
                if (events_.empty()) {
                    if (closed_) break;
                    continue;
                }
                event = std::move(events_.front());
                events_.pop_front();
            }

            bool disabled = false;
            {
                std::lock_guard<std::mutex> state_lock(state_->mutex);
                disabled = state_->callback_failed;
                state_->callback_active = !disabled;
            }
            if (disabled) continue;
            try {
                callback_(event);
            } catch (...) {
                std::lock_guard<std::mutex> state_lock(state_->mutex);
                state_->callback_failed = true;
            }
            {
                std::lock_guard<std::mutex> state_lock(state_->mutex);
                state_->callback_active = false;
            }
            state_->condition.notify_all();
        }
        {
            std::lock_guard<std::mutex> state_lock(state_->mutex);
            state_->callback_active = false;
        }
        state_->condition.notify_all();
    }

    std::shared_ptr<detail::SliceJobState> state_;
    SliceCallback callback_;
    std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<SliceEvent> events_;
    std::thread thread_;
    int last_percent_ {0};
    bool terminal_enqueued_ {false};
    bool closed_ {false};
};

std::vector<Diagnostic> callback_diagnostics(const std::shared_ptr<detail::SliceJobState> &state)
{
    std::lock_guard<std::mutex> lock(state->mutex);
    if (!state->callback_failed) return {};
    return {{ErrorCode::internal, Severity::warning,
             "Slice callback threw an exception and was disabled", "/callback"}};
}

void finish_job(const std::shared_ptr<detail::SliceJobState> &state,
                Result<std::shared_ptr<const SliceResult>> outcome)
{
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->print = nullptr;
        state->outcome.emplace(std::move(outcome));
        state->finished = true;
    }
    {
        std::lock_guard<std::mutex> lock(state->engine->mutex);
        state->engine->active = false;
    }
    state->condition.notify_all();
}

void finish_exception(const std::shared_ptr<detail::SliceJobState> &state,
                      std::exception_ptr exception)
{
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->print = nullptr;
        state->unhandled_exception = std::move(exception);
        state->finished = true;
    }
    {
        std::lock_guard<std::mutex> lock(state->engine->mutex);
        state->engine->active = false;
    }
    state->condition.notify_all();
}

bool cancellation_requested(const std::shared_ptr<detail::SliceJobState> &state)
{
    std::lock_guard<std::mutex> lock(state->mutex);
    return state->cancel_requested;
}

void complete_failure(const std::shared_ptr<detail::SliceJobState> &job,
                      EventDispatcher &events, ErrorCode code,
                      std::string message, std::string field,
                      std::vector<Diagnostic> diagnostics = {})
{
    const Diagnostic terminal_diagnostic{
        code, Severity::error, message, field};
    events.emit(code == ErrorCode::cancelled ? SliceEventKind::cancelled
                                              : SliceEventKind::failed,
                100, terminal_diagnostic);
    events.close();
    auto callback = callback_diagnostics(job);
    diagnostics.insert(diagnostics.end(), std::make_move_iterator(callback.begin()),
                       std::make_move_iterator(callback.end()));
    finish_job(job, failure<std::shared_ptr<const SliceResult>>(
        code, std::move(message), std::move(field), std::move(diagnostics)));
}

std::vector<FilamentUsage> filament_usage(const Slic3r::Print &print,
                                          const Slic3r::DynamicPrintConfig &config,
                                          std::size_t slots)
{
    const auto *diameters = config.option<Slic3r::ConfigOptionFloats>("filament_diameter");
    const auto *densities = config.option<Slic3r::ConfigOptionFloats>("filament_density");
    std::vector<FilamentUsage> result;
    result.reserve(print.print_statistics().filament_stats.size());
    for (const auto &[index, length] : print.print_statistics().filament_stats) {
        if (index >= slots) continue;
        const double diameter = diameters && index < diameters->values.size()
            ? diameters->values[index] : 1.75;
        const double density = densities && index < densities->values.size()
            ? densities->values[index] : 0.0;
        const double volume = length * 3.14159265358979323846 * diameter * diameter / 4.0;
        result.push_back({FilamentSlotId{static_cast<std::uint32_t>(index)}, length,
                          volume, volume * density / 1000.0});
    }
    return result;
}

std::uint64_t layer_count(const Slic3r::Print &print)
{
    std::uint64_t result = 0;
    for (const auto *object : print.objects())
        result = std::max<std::uint64_t>(result, object->total_layer_count());
    return result;
}

FilamentMapMode public_map_mode(Slic3r::FilamentMapMode mode)
{
    if (mode == Slic3r::fmmManual) return FilamentMapMode::manual;
    if (mode == Slic3r::fmmAutoForMatch) return FilamentMapMode::auto_for_match;
    return FilamentMapMode::auto_for_flush;
}

EffectiveFilamentMap final_filament_map(const Slic3r::Print &print,
                                        EffectiveFilamentMap::Source source)
{
    std::vector<ToolId> tools;
    const std::vector<int> core_tools = print.get_filament_maps();
    tools.reserve(core_tools.size());
    for (int tool : core_tools)
        tools.push_back(ToolId{static_cast<std::uint32_t>(tool)});
    return {public_map_mode(print.get_filament_map_mode()), std::move(tools), source};
}

std::array<std::uint8_t, 4> parse_color(std::string value)
{
    if (!value.empty() && value.front() == '#') value.erase(value.begin());
    if (value.size() != 6 && value.size() != 8) return {0x26, 0xa6, 0x9a, 0xff};
    const auto hex = [](char ch) -> int {
        if (ch >= '0' && ch <= '9') return ch - '0';
        if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
        if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
        return -1;
    };
    std::array<std::uint8_t, 4> rgba{0, 0, 0, 0xff};
    for (std::size_t index = 0; index < value.size() / 2 && index < 4; ++index) {
        const int high = hex(value[index * 2]);
        const int low = hex(value[index * 2 + 1]);
        if (high < 0 || low < 0) return {0x26, 0xa6, 0x9a, 0xff};
        rgba[index] = static_cast<std::uint8_t>(high * 16 + low);
    }
    return rgba;
}

PreviewMoveType public_move_type(Slic3r::EMoveType type)
{
    switch (type) {
    case Slic3r::EMoveType::Travel:
    case Slic3r::EMoveType::Wipe:
    case Slic3r::EMoveType::Seam: return PreviewMoveType::travel;
    case Slic3r::EMoveType::Extrude: return PreviewMoveType::extrude;
    case Slic3r::EMoveType::Retract: return PreviewMoveType::retract;
    case Slic3r::EMoveType::Unretract: return PreviewMoveType::unretract;
    case Slic3r::EMoveType::Tool_change: return PreviewMoveType::tool_change;
    case Slic3r::EMoveType::Color_change: return PreviewMoveType::color_change;
    case Slic3r::EMoveType::Custom_GCode: return PreviewMoveType::custom;
    default: return PreviewMoveType::unknown;
    }
}

PreviewExtrusionRole public_extrusion_role(Slic3r::ExtrusionRole role)
{
    switch (role) {
    case Slic3r::erNone: return PreviewExtrusionRole::none;
    case Slic3r::erPerimeter: return PreviewExtrusionRole::perimeter;
    case Slic3r::erExternalPerimeter: return PreviewExtrusionRole::external_perimeter;
    case Slic3r::erOverhangPerimeter: return PreviewExtrusionRole::overhang_perimeter;
    case Slic3r::erInternalInfill: return PreviewExtrusionRole::internal_infill;
    case Slic3r::erSolidInfill: return PreviewExtrusionRole::solid_infill;
    case Slic3r::erTopSolidInfill: return PreviewExtrusionRole::top_solid_infill;
    case Slic3r::erBridgeInfill:
    case Slic3r::erInternalBridgeInfill: return PreviewExtrusionRole::bridge_infill;
    case Slic3r::erSupportMaterial: return PreviewExtrusionRole::support_material;
    case Slic3r::erSupportMaterialInterface:
    case Slic3r::erSupportTransition: return PreviewExtrusionRole::support_interface;
    case Slic3r::erSkirt: return PreviewExtrusionRole::skirt;
    case Slic3r::erBrim: return PreviewExtrusionRole::brim;
    case Slic3r::erWipeTower: return PreviewExtrusionRole::wipe_tower;
    case Slic3r::erCustom: return PreviewExtrusionRole::custom;
    default: return PreviewExtrusionRole::unknown;
    }
}

PreviewColorSource public_color_source(
    Slic3r::GCodeProcessorResult::PreviewColorSource source)
{
    switch (source) {
    case Slic3r::GCodeProcessorResult::PreviewColorSource::Filament:
        return PreviewColorSource::filament;
    case Slic3r::GCodeProcessorResult::PreviewColorSource::ColorChange:
        return PreviewColorSource::color_change;
    case Slic3r::GCodeProcessorResult::PreviewColorSource::Custom:
        return PreviewColorSource::custom;
    default: return PreviewColorSource::unknown;
    }
}

PreviewEventType event_type_for_move(Slic3r::EMoveType type)
{
    switch (type) {
    case Slic3r::EMoveType::Tool_change: return PreviewEventType::tool_change;
    case Slic3r::EMoveType::Color_change: return PreviewEventType::color_change;
    case Slic3r::EMoveType::Pause_Print: return PreviewEventType::pause;
    case Slic3r::EMoveType::Custom_GCode: return PreviewEventType::custom_gcode;
    default: return PreviewEventType::unknown;
    }
}

std::uint64_t estimate_preview_bytes(const SlicePreview &preview)
{
    std::uint64_t bytes = preview.schema_id.size() + preview.coordinate_space.size();
    bytes += preview.layers.size() * sizeof(PreviewLayer);
    bytes += preview.tools.size() * sizeof(PreviewTool);
    bytes += preview.filaments.size() * sizeof(PreviewFilament);
    bytes += preview.colors.size() * sizeof(PreviewColor);
    bytes += preview.instances.size() * sizeof(PreviewInstance);
    bytes += preview.moves.size() * sizeof(PreviewMove);
    bytes += preview.events.size() * sizeof(PreviewEvent);
    for (const auto &object : preview.objects)
        bytes += sizeof(PreviewObject) + object.name.size();
    for (const auto &color : preview.colors) bytes += color.name.size();
    for (const auto &event : preview.events) bytes += event.message.size();
    return bytes;
}

std::string json_escape(const std::string &value)
{
    std::string out;
    out.reserve(value.size());
    for (char ch : value) {
        switch (ch) {
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default: out += ch; break;
        }
    }
    return out;
}

std::string serialize_preview_artifact(const SlicePreview &preview)
{
    std::ostringstream out;
    out << "{\"schema_id\":\"" << json_escape(preview.schema_id) << "\",";
    out << "\"schema_version\":" << preview.schema_version << ",";
    out << "\"coordinate_space\":\"" << json_escape(preview.coordinate_space) << "\",";
    out << "\"layers\":" << preview.layers.size() << ",";
    out << "\"tools\":" << preview.tools.size() << ",";
    out << "\"filaments\":" << preview.filaments.size() << ",";
    out << "\"colors\":" << preview.colors.size() << ",";
    out << "\"objects\":" << preview.objects.size() << ",";
    out << "\"instances\":" << preview.instances.size() << ",";
    out << "\"events\":" << preview.events.size() << ",";
    out << "\"moves\":[";
    for (std::size_t index = 0; index < preview.moves.size(); ++index) {
        const auto &move = preview.moves[index];
        if (index != 0) out << ',';
        out << "{\"id\":" << move.id << ",\"layer\":" << move.layer_id
            << ",\"x\":" << move.end_mm.x << ",\"y\":" << move.end_mm.y
            << ",\"z\":" << move.end_mm.z << ",\"e\":" << move.extrusion_delta_mm
            << ",\"f\":" << move.feedrate_mm_s << "}";
    }
    out << "]}";
    return out.str();
}

Result<std::optional<std::filesystem::path>> publish_preview_artifact(
    const SlicePreview &preview, const std::filesystem::path &destination,
    const std::filesystem::path &temporary_root, std::uint64_t preview_limit,
    std::uint64_t temporary_limit)
{
    const std::string bytes = serialize_preview_artifact(preview);
    if (bytes.size() > preview_limit)
        return failure<std::optional<std::filesystem::path>>(
            ErrorCode::resource_limit_exceeded,
            "Preview byte limit exceeded", "/limits/preview_bytes");
    if (bytes.size() > temporary_limit)
        return failure<std::optional<std::filesystem::path>>(
            ErrorCode::resource_limit_exceeded,
            "Temporary disk byte limit exceeded", "/limits/temporary_disk_bytes");
    std::error_code error;
    if (destination.has_parent_path()) {
        std::filesystem::create_directories(destination.parent_path(), error);
        if (error)
            return failure<std::optional<std::filesystem::path>>(
                ErrorCode::io, "Unable to create preview artifact directory: " +
                                   error.message(),
                "/output/preview_artifact_path");
    }
    const auto temporary = temporary_root / (destination.filename().string() + ".tmp");
    {
        std::ofstream output(temporary, std::ios::binary);
        if (!output)
            return failure<std::optional<std::filesystem::path>>(
                ErrorCode::io, "Unable to write preview artifact",
                "/output/preview_artifact_path");
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }
    std::filesystem::rename(temporary, destination, error);
    if (error) {
        std::filesystem::remove(temporary, error);
        return failure<std::optional<std::filesystem::path>>(
            ErrorCode::io, "Unable to publish preview artifact", "/output/preview_artifact_path");
    }
    return detail::ResultAccess::success(std::optional<std::filesystem::path>{destination});
}

Result<std::shared_ptr<const SlicePreview>> build_preview(
    const Slic3r::GCodeProcessorResult &processor,
    const detail::ProjectData &project,
    const Slic3r::DynamicPrintConfig &config,
    const EffectiveFilamentMap &completed_map,
    const ResourceLimits &limits)
{
    if (processor.moves.size() > limits.preview_moves)
        return failure<std::shared_ptr<const SlicePreview>>(
            ErrorCode::resource_limit_exceeded,
            "Preview move limit exceeded", "/limits/preview_moves");

    auto preview = std::make_shared<SlicePreview>();
    preview->schema_id = "libslicer.preview";
    preview->schema_version = 1;
    preview->coordinate_space = "orca_plate_world_mm";

    for (const Slic3r::ModelObject *object : project.model.objects) {
        preview->objects.push_back({detail::ProjectIdAccess::object(project.identity,
                                      object->id().id), object->name});
        for (const Slic3r::ModelInstance *instance : object->instances) {
            preview->instances.push_back({detail::ProjectIdAccess::instance(
                project.identity, instance->id().id),
                detail::ProjectIdAccess::object(project.identity, object->id().id)});
        }
    }

    const auto *nozzles = config.option<Slic3r::ConfigOptionFloats>("nozzle_diameter");
    const auto *offsets = config.option<Slic3r::ConfigOptionPoints>("extruder_offset");
    std::set<std::uint32_t> emitted_tools;
    for (ToolId tool : completed_map.tools) {
        if (!emitted_tools.insert(tool.value).second) continue;
        const std::size_t core_index = tool.value > 0 ? tool.value - 1 : 0;
        const double nozzle = nozzles && core_index < nozzles->values.size()
            ? nozzles->values[core_index] : 0.0;
        Vec3d offset{0.0, 0.0, 0.0};
        if (offsets && core_index < offsets->values.size())
            offset = {offsets->values[core_index].x(), offsets->values[core_index].y(), 0.0};
        preview->tools.push_back({tool, std::nullopt, nozzle, offset});
    }

    const auto *colors = config.option<Slic3r::ConfigOptionStrings>("filament_colour");
    const auto *diameters = config.option<Slic3r::ConfigOptionFloats>("filament_diameter");
    const auto *densities = config.option<Slic3r::ConfigOptionFloats>("filament_density");
    const auto *costs = config.option<Slic3r::ConfigOptionFloats>("filament_cost");
    for (std::size_t slot = 0; slot < completed_map.tools.size(); ++slot) {
        const auto rgba = colors && slot < colors->values.size()
            ? parse_color(colors->values[slot]) : std::array<std::uint8_t, 4>{0x26, 0xa6, 0x9a, 0xff};
        preview->filaments.push_back({
            FilamentSlotId{static_cast<std::uint32_t>(slot)}, completed_map.tools[slot], rgba,
            diameters && slot < diameters->values.size() ? diameters->values[slot] : 1.75,
            densities && slot < densities->values.size() ? densities->values[slot] : 0.0,
            costs && slot < costs->values.size() ? costs->values[slot] : 0.0});
    }
    for (const auto &[id, fact] : processor.preview_colors) {
        std::array<std::uint8_t, 4> rgba{};
        for (std::size_t index = 0; index < 4; ++index)
            rgba[index] = static_cast<std::uint8_t>(
                std::max(0.0f, std::min(1.0f, fact.color_rgba[index])) * 255.0f);
        std::optional<FilamentSlotId> filament;
        if (fact.filament_id < completed_map.tools.size())
            filament = FilamentSlotId{fact.filament_id};
        preview->colors.push_back({id, rgba, public_color_source(fact.source),
                                   filament, fact.name});
    }

    std::map<std::uint32_t, std::size_t> layer_positions;
    Slic3r::Vec3f previous = Slic3r::Vec3f::Zero();
    double accumulated_time = 0.0;
    std::uint64_t event_id = 0;
    preview->moves.reserve(processor.moves.size());
    for (const auto &core : processor.moves) {
        if (core.internal_only) {
            previous = core.position;
            continue;
        }
        auto layer_it = layer_positions.find(core.layer_id);
        if (layer_it == layer_positions.end()) {
            layer_it = layer_positions.emplace(core.layer_id, preview->layers.size()).first;
            preview->layers.push_back({core.layer_id,
                                       static_cast<std::uint64_t>(preview->moves.size()),
                                       0, core.print_z, core.height, 0.0});
        }
        const std::size_t layer_pos = layer_it->second;
        auto &layer = preview->layers[layer_pos];
        ++layer.move_count;
        layer.duration_s += core.time[static_cast<std::size_t>(Slic3r::ToolpathTimeMode::Normal)];
        if (layer.height_mm == 0.0) layer.height_mm = core.height;

        std::optional<FilamentSlotId> filament;
        std::optional<ToolId> tool;
        const std::size_t slot = static_cast<std::size_t>(core.extruder_id);
        if (slot < completed_map.tools.size()) {
            filament = FilamentSlotId{static_cast<std::uint32_t>(slot)};
            tool = completed_map.tools[slot];
        }
        const auto move_id = static_cast<std::uint64_t>(preview->moves.size());
        const double move_time =
            core.time[static_cast<std::size_t>(Slic3r::ToolpathTimeMode::Normal)];
        accumulated_time += move_time;
        PreviewMove move{
            move_id, core.gcode_id, core.layer_id, std::nullopt, std::nullopt, tool,
            filament, core.cp_color_id == 0 ? std::optional<std::uint32_t>{}
                                            : std::optional<std::uint32_t>{core.cp_color_id},
            public_move_type(core.type), PreviewPathKind::linear,
            public_extrusion_role(core.extrusion_role),
            {previous.x(), previous.y(), previous.z()},
            {core.position.x(), core.position.y(), core.position.z()}, std::nullopt,
            core.delta_extruder, core.feedrate, core.actual_feedrate, core.width,
            core.height, core.mm3_per_mm, core.travel_dist, core.fan_speed,
            core.temperature, core.pressure_advance, core.acceleration, core.jerk,
            move_time, core.layer_duration, core.print_z, std::nullopt};
        preview->moves.push_back(std::move(move));
        const PreviewEventType event_type = event_type_for_move(core.type);
        if (event_type != PreviewEventType::unknown) {
            preview->events.push_back({event_id++, move_id, event_type, tool, filament,
                                       core.print_z, accumulated_time, {}});
        }
        previous = core.position;
    }
    const std::uint64_t bytes = estimate_preview_bytes(*preview);
    if (bytes > limits.preview_bytes)
        return failure<std::shared_ptr<const SlicePreview>>(
            ErrorCode::resource_limit_exceeded,
            "Preview byte limit exceeded", "/limits/preview_bytes");
    return detail::ResultAccess::success(
        std::static_pointer_cast<const SlicePreview>(preview));
}

std::atomic<std::uint64_t> next_job_directory {1};

class JobDirectory {
public:
    explicit JobDirectory(const std::filesystem::path &root)
    {
        std::error_code error;
        for (unsigned attempt = 0; attempt < 100; ++attempt) {
            path_ = root / ("libslicer-sdk-v1-job-" +
                            std::to_string(next_job_directory.fetch_add(1)));
            if (std::filesystem::create_directory(path_, error)) return;
            if (error && error != std::errc::file_exists)
                throw std::filesystem::filesystem_error(
                    "Unable to create slice temporary directory", path_, error);
            error.clear();
        }
        throw std::filesystem::filesystem_error(
            "Unable to allocate a unique slice temporary directory", root,
            std::make_error_code(std::errc::file_exists));
    }

    ~JobDirectory()
    {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    const std::filesystem::path &path() const { return path_; }

private:
    std::filesystem::path path_;
};

struct ExportOutput {
    std::string bytes;
    bool gcode_limit_exceeded {false};
    bool temporary_limit_exceeded {false};
};

ExportOutput export_bounded(Slic3r::Print &print,
                            const std::filesystem::path &final_path,
                            Slic3r::GCodeProcessorResult &processor,
                            std::uint64_t gcode_limit,
                            std::uint64_t temporary_limit)
{
    ExportOutput output;
    struct BudgetScope {
        BudgetScope(std::uint64_t gcode, std::uint64_t temporary)
        {
            Slic3r::begin_bounded_gcode_export(gcode, temporary);
        }
        ~BudgetScope() { Slic3r::end_bounded_gcode_export(); }
    } budget(gcode_limit, temporary_limit);
    try {
        const std::string path = print.export_gcode(final_path.string(), &processor);
        std::ifstream input(path, std::ios::binary);
        if (!input)
            throw std::filesystem::filesystem_error(
                "Unable to read generated G-code", path,
                std::make_error_code(std::errc::io_error));
        output.bytes.assign(std::istreambuf_iterator<char>(input),
                            std::istreambuf_iterator<char>());
    } catch (const Slic3r::GCodeExportLimitExceeded &error) {
        output.gcode_limit_exceeded =
            error.kind() == Slic3r::GCodeExportLimitKind::final_output;
        output.temporary_limit_exceeded =
            error.kind() == Slic3r::GCodeExportLimitKind::temporary_disk;
    }
    return output;
}

void prepare_target_plate(Slic3r::Model &model, const Slic3r::PlateData &plate,
                          const Slic3r::DynamicPrintConfig &config)
{
    model.curr_plate_index = plate.plate_index;

    std::set<std::pair<int, int>> included(plate.objects_and_instances.begin(),
                                            plate.objects_and_instances.end());
    for (std::size_t object_index = 0; object_index < model.objects.size(); ++object_index) {
        Slic3r::ModelObject *object = model.objects[object_index];
        for (std::size_t instance_index = 0; instance_index < object->instances.size();
             ++instance_index) {
            Slic3r::ModelInstance *instance = object->instances[instance_index];
            const std::size_t persistent_id = instance->loaded_id > 0
                ? static_cast<std::size_t>(instance->loaded_id)
                : static_cast<std::size_t>(instance->id().id);
            const bool belongs = included.count({static_cast<int>(object_index),
                                                  static_cast<int>(instance_index)}) != 0;
            const bool excluded = std::find(plate.skipped_objects.begin(),
                                            plate.skipped_objects.end(),
                                            persistent_id) != plate.skipped_objects.end();
            instance->printable = instance->printable && belongs && !excluded;
        }
    }

    const auto *area = config.option<Slic3r::ConfigOptionPoints>("printable_area");
    const auto *height = config.option<Slic3r::ConfigOptionFloat>("printable_height");
    const auto *extruder_areas =
        config.option<Slic3r::ConfigOptionPointsGroups>("extruder_printable_area");
    const auto *extruder_heights =
        config.option<Slic3r::ConfigOptionFloats>("extruder_printable_height");
    if (area && height && extruder_areas && extruder_heights) {
        Slic3r::BuildVolume build_volume(area->values, height->value,
                                         extruder_areas->values,
                                         extruder_heights->values);
        model.update_print_volume_state(build_volume);
    }
}

enum class SlicePhase { preparing, validating, slicing, exporting };

void run_slice(const std::shared_ptr<detail::SliceJobState> &job,
               detail::FrozenSliceInput resolved,
               SliceCallback callback)
{
    EventDispatcher events(job, std::move(callback));
    const auto started = std::chrono::steady_clock::now();
    SlicePhase phase = SlicePhase::preparing;
    std::atomic<bool> report_slicing_progress {false};
    std::mutex diagnostics_mutex;
    std::vector<Diagnostic> result_diagnostics;
#ifdef LIBSLICER_SDK_TESTING
    detail::testing::wait_at_stage(detail::testing::SliceStage::queued);
    if (cancellation_requested(job)) {
        complete_failure(job, events, ErrorCode::cancelled,
                         "Slice job was cancelled", "/job");
        return;
    }
    detail::testing::wait_at_stage(detail::testing::SliceStage::preparing);
    if (cancellation_requested(job)) {
        complete_failure(job, events, ErrorCode::cancelled,
                         "Slice job was cancelled", "/job");
        return;
    }
#endif
    events.emit(SliceEventKind::preparing, 0);

    try {
        const auto &project = detail::ProjectSnapshotAccess::data(resolved.project);
        const auto &config = detail::EffectiveConfigurationAccess::core(
            resolved.configuration);
        const auto context = job->engine->context;

        if (const auto *post = config.option<Slic3r::ConfigOptionStrings>("post_process"))
            if (std::any_of(post->values.begin(), post->values.end(),
                            [](const std::string &value) { return !value.empty(); })) {
                complete_failure(job, events, ErrorCode::invalid_configuration,
                    "post_process scripts are not executed by libslicer v1",
                    "/configuration/post_process");
                return;
            }
        if (cancellation_requested(job))
            throw Slic3r::CanceledException();

        std::lock_guard<std::mutex> runtime(detail::runtime_mutex());
        const std::string old_resources = Slic3r::resources_dir();
        const std::string old_data = Slic3r::data_dir();
        const std::string old_temporary = Slic3r::temporary_dir();
        struct RestorePaths {
            std::string resources, data, temporary;
            ~RestorePaths() {
                Slic3r::set_resources_dir(resources);
                Slic3r::set_data_dir(data);
                Slic3r::set_temporary_dir(temporary);
            }
        } restore{old_resources, old_data, old_temporary};
        Slic3r::set_resources_dir(context->options.resources_dir.string());
        Slic3r::set_data_dir(context->options.data_dir.string());
        Slic3r::set_temporary_dir(context->options.temporary_dir.string());

        Slic3r::Model model(project->model);
        const Slic3r::PlateData *plate = nullptr;
        for (const auto &candidate : project->plates)
            if (static_cast<std::uint64_t>(candidate.plate_index + 1) ==
                resolved.plate.value()) {
                plate = &candidate;
                break;
            }
        if (!plate)
            throw Slic3r::InvalidArgument(
                "Resolved plate is missing from the immutable project snapshot");
        prepare_target_plate(model, *plate, config);

        Slic3r::Print print;
        print.set_plate_index(plate->plate_index);
        print.set_plate_name(plate->plate_name);
        {
            std::lock_guard<std::mutex> lock(job->mutex);
            job->print = &print;
            if (job->cancel_requested) print.cancel();
        }
        print.set_status_callback([&](const Slic3r::PrintBase::SlicingStatus &status) {
            if (!report_slicing_progress.load(std::memory_order_relaxed)) return;
            const int core_percent = std::max(0, std::min(100, status.percent));
            if ((status.flags & (Slic3r::PrintBase::SlicingStatus::UPDATE_PRINT_STEP_WARNINGS |
                                 Slic3r::PrintBase::SlicingStatus::UPDATE_PRINT_OBJECT_STEP_WARNINGS)) != 0 &&
                !status.text.empty()) {
                Diagnostic diagnostic{ErrorCode::invalid_configuration, Severity::warning,
                                      status.text, "/configuration"};
                {
                    std::lock_guard<std::mutex> lock(diagnostics_mutex);
                    result_diagnostics.push_back(diagnostic);
                }
                events.emit(SliceEventKind::warning,
                            15 + core_percent * 65 / 100, std::move(diagnostic));
                return;
            }
            events.emit(SliceEventKind::slicing, 15 + core_percent * 65 / 100);
        });
        print.apply(model, config, false);

#ifdef LIBSLICER_SDK_TESTING
        detail::testing::wait_at_stage(detail::testing::SliceStage::validating);
        if (cancellation_requested(job))
            throw Slic3r::CanceledException();
#endif
        phase = SlicePhase::validating;
        events.emit(SliceEventKind::validating, 10);
        Slic3r::StringObjectException warning;
        const Slic3r::StringObjectException validation = print.validate(&warning);
        if (!warning.string.empty()) {
            Diagnostic diagnostic{ErrorCode::invalid_configuration, Severity::warning,
                                  warning.string, "/configuration"};
            {
                std::lock_guard<std::mutex> lock(diagnostics_mutex);
                result_diagnostics.push_back(diagnostic);
            }
            events.emit(SliceEventKind::warning, 10, std::move(diagnostic));
        }
        if (!validation.string.empty()) {
            complete_failure(job, events, ErrorCode::invalid_configuration,
                             validation.string, "/configuration",
                             std::move(result_diagnostics));
            return;
        }
        if (cancellation_requested(job)) print.cancel();

#ifdef LIBSLICER_SDK_TESTING
        detail::testing::wait_at_stage(detail::testing::SliceStage::slicing);
        if (cancellation_requested(job))
            throw Slic3r::CanceledException();
#endif
        phase = SlicePhase::slicing;
        report_slicing_progress.store(true, std::memory_order_relaxed);
        events.emit(SliceEventKind::slicing, 15);
        print.process();
        report_slicing_progress.store(false, std::memory_order_relaxed);
        if (cancellation_requested(job)) print.cancel();

#ifdef LIBSLICER_SDK_TESTING
        detail::testing::wait_at_stage(detail::testing::SliceStage::exporting);
        if (cancellation_requested(job))
            throw Slic3r::CanceledException();
#endif
        phase = SlicePhase::exporting;
        events.emit(SliceEventKind::exporting, 85);
        JobDirectory job_directory(context->options.temporary_dir);
        Slic3r::GCodeProcessorResult processor;
        ExportOutput output = export_bounded(
            print, job_directory.path() / "output.gcode", processor,
            context->options.limits.gcode_bytes,
            context->options.limits.temporary_disk_bytes);
        if (output.gcode_limit_exceeded) {
            complete_failure(job, events, ErrorCode::resource_limit_exceeded,
                             "G-code byte limit exceeded", "/limits/gcode_bytes",
                             std::move(result_diagnostics));
            return;
        }
        if (output.temporary_limit_exceeded) {
            complete_failure(job, events, ErrorCode::resource_limit_exceeded,
                             "Temporary disk byte limit exceeded",
                             "/limits/temporary_disk_bytes",
                             std::move(result_diagnostics));
            return;
        }
        if (cancellation_requested(job))
            throw Slic3r::CanceledException();

        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started);
        EffectiveFilamentMap completed_map =
            final_filament_map(print, resolved.filament_map.source);
        SliceStatistics statistics{
            elapsed, layer_count(print),
            filament_usage(print, config, completed_map.tools.size())};
        std::shared_ptr<const SlicePreview> preview;
        std::optional<std::filesystem::path> preview_artifact_path;
        if (resolved.output.preview != PreviewDelivery::none) {
            auto converted = build_preview(processor, *project, config, completed_map,
                                           context->options.limits);
            if (!converted.has_value()) {
                complete_failure(job, events,
                    converted.error_code().value_or(ErrorCode::resource_limit_exceeded),
                    converted.diagnostics().front().message,
                    converted.diagnostics().front().field,
                    std::move(result_diagnostics));
                return;
            }
            if (resolved.output.preview == PreviewDelivery::artifact) {
                auto published = publish_preview_artifact(
                    *converted.value(), *resolved.output.preview_artifact_path,
                    job_directory.path(), context->options.limits.preview_bytes,
                    context->options.limits.temporary_disk_bytes);
                if (!published.has_value()) {
                    complete_failure(job, events,
                        published.error_code().value_or(ErrorCode::io),
                        published.diagnostics().front().message,
                        published.diagnostics().front().field,
                        std::move(result_diagnostics));
                    return;
                }
                preview_artifact_path = std::move(published).value();
            } else {
                preview = std::move(converted).value();
            }
        }
        auto result = std::make_shared<const SliceResult>(SliceResult{
            resolved.output.include_gcode
                ? std::optional<std::string>(std::move(output.bytes))
                : std::optional<std::string>{},
            std::move(preview), std::move(preview_artifact_path),
            resolved.configuration, std::move(completed_map), std::move(statistics),
            std::move(result_diagnostics)});
        events.emit(SliceEventKind::completed, 100);
        events.close();
        finish_job(job, detail::ResultAccess::success<std::shared_ptr<const SliceResult>>(
            std::move(result), callback_diagnostics(job)));
    } catch (const Slic3r::CanceledException &) {
        complete_failure(job, events, ErrorCode::cancelled,
                         "Slice job was cancelled", "/job",
                         std::move(result_diagnostics));
    } catch (const std::filesystem::filesystem_error &error) {
        if (cancellation_requested(job)) {
            complete_failure(job, events, ErrorCode::cancelled,
                             "Slice job was cancelled", "/job",
                             std::move(result_diagnostics));
        } else {
            complete_failure(job, events, ErrorCode::io, error.what(),
                             "/temporary_dir", std::move(result_diagnostics));
        }
    } catch (const std::bad_alloc &) {
        finish_exception(job, std::current_exception());
    } catch (const std::exception &error) {
        if (cancellation_requested(job)) {
            complete_failure(job, events, ErrorCode::cancelled,
                             "Slice job was cancelled", "/job",
                             std::move(result_diagnostics));
        } else if (phase == SlicePhase::preparing || phase == SlicePhase::validating) {
            complete_failure(job, events, ErrorCode::invalid_configuration,
                             error.what(), "/configuration",
                             std::move(result_diagnostics));
        } else {
            complete_failure(job, events, ErrorCode::slicing_failed,
                             error.what(), "/slice", std::move(result_diagnostics));
        }
    } catch (...) {
        complete_failure(job, events, ErrorCode::internal,
                         "Unclassified exception escaped the slice adapter", "/internal",
                         std::move(result_diagnostics));
    }
}

} // namespace

SliceJob::SliceJob(std::shared_ptr<detail::SliceJobState> state) : state_(std::move(state)) {}

Result<void> SliceJob::cancel()
{
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->cancel_requested = true;
    if (state_->print) state_->print->cancel();
    return detail::ResultAccess::success();
}

namespace {

Result<std::shared_ptr<const SliceResult>> copy_terminal_outcome_locked(
    const detail::SliceJobState &state)
{
    if (state.unhandled_exception)
        std::rethrow_exception(state.unhandled_exception);
    if (!state.outcome)
        return failure<std::shared_ptr<const SliceResult>>(
            ErrorCode::internal,
            "Slice job finished without a result or diagnostic", "/job");
    if (!state.outcome->has_value() && state.outcome->diagnostics().empty())
        return failure<std::shared_ptr<const SliceResult>>(
            state.outcome->error_code().value_or(ErrorCode::internal),
            "Slice job failed without a diagnostic", "/job");
    return *state.outcome;
}

template<class T>
Result<T> callback_wait_conflict(const char *operation)
{
    return failure<T>(
        ErrorCode::conflict,
        std::string(operation) + " cannot be called from this job's callback",
        "/callback");
}

} // namespace

Result<std::shared_ptr<const SliceResult>> SliceJob::wait()
{
    std::unique_lock<std::mutex> lock(state_->mutex);
    if (state_->callback_thread == std::this_thread::get_id() && state_->callback_active)
        return callback_wait_conflict<std::shared_ptr<const SliceResult>>("wait()");
    state_->condition.wait(lock, [&] { return state_->finished; });
    return copy_terminal_outcome_locked(*state_);
}

Result<std::optional<std::shared_ptr<const SliceResult>>> SliceJob::wait_for(
    std::chrono::milliseconds timeout)
{
    std::unique_lock<std::mutex> lock(state_->mutex);
    if (state_->callback_thread == std::this_thread::get_id() && state_->callback_active)
        return callback_wait_conflict<
            std::optional<std::shared_ptr<const SliceResult>>>("wait_for()");
    if (timeout.count() < 0)
        return failure<std::optional<std::shared_ptr<const SliceResult>>>(
            ErrorCode::invalid_argument,
            "wait_for() timeout must be non-negative", "/timeout");

    if (!state_->condition.wait_for(lock, timeout, [&] { return state_->finished; }))
        return detail::ResultAccess::success(
            std::optional<std::shared_ptr<const SliceResult>>{});

    auto terminal = copy_terminal_outcome_locked(*state_);
    if (!terminal.has_value())
        return forward_failure<std::optional<std::shared_ptr<const SliceResult>>>(terminal);
    return detail::ResultAccess::success(
        std::optional<std::shared_ptr<const SliceResult>>(terminal.value()),
        terminal.diagnostics());
}

SliceEngine::SliceEngine(std::shared_ptr<detail::SliceEngineState> state) : state_(std::move(state)) {}

Result<SliceInspection> SliceEngine::inspect(const SliceRequest &request) const
{
    try {
        std::optional<detail::TemporarySliceSelection> temporary;
        if (request.temporary_selection) {
            temporary.emplace(detail::TemporarySliceSelection{
                request.temporary_selection->selection,
                request.temporary_selection->complete_manual_map});
        }
        auto frozen = detail::resolve_slice_input(
            state_->context, request.project, request.plate, std::move(temporary),
            detail::SliceInputMode::inspect);
        if (!frozen.has_value()) return forward_failure<SliceInspection>(frozen);
        auto diagnostics = frozen.diagnostics();
        auto input = std::move(frozen).value();
        return detail::ResultAccess::success(
            SliceInspection{std::move(input.configuration), std::move(input.filament_map)},
            std::move(diagnostics));
    } catch (const std::bad_alloc &) {
        throw;
    } catch (const std::filesystem::filesystem_error &error) {
        return failure<SliceInspection>(ErrorCode::io, error.what(), "/project");
    } catch (const std::exception &error) {
        return failure<SliceInspection>(ErrorCode::internal, error.what(), "/internal");
    } catch (...) {
        return failure<SliceInspection>(
            ErrorCode::internal, "Unclassified exception escaped slice inspection",
            "/internal");
    }
}

Result<SliceJob> SliceEngine::submit(SliceRequest request, SliceCallback callback)
{
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (state_->active)
            return failure<SliceJob>(ErrorCode::busy,
                                     "Slice engine already has an active job", "/engine");
        state_->active = true;
    }
    ActiveReservation reservation(state_);

    auto frozen = freeze_slice_input(state_, std::move(request));
    if (!frozen.has_value()) return forward_failure<SliceJob>(frozen);
    auto diagnostics = frozen.diagnostics();
    auto input = std::move(frozen).value();

    auto job = std::make_shared<detail::SliceJobState>();
    job->engine = state_;
    try {
        std::thread([job, input = std::move(input), callback = std::move(callback)]() mutable {
            try {
                run_slice(job, std::move(input), std::move(callback));
            } catch (const std::bad_alloc &) {
                finish_exception(job, std::current_exception());
            } catch (const std::exception &error) {
                finish_job(job, failure<std::shared_ptr<const SliceResult>>(
                    ErrorCode::internal, error.what(), "/internal"));
            } catch (...) {
                finish_job(job, failure<std::shared_ptr<const SliceResult>>(
                    ErrorCode::internal, "Unclassified exception escaped the slice worker",
                    "/internal"));
            }
        }).detach();
        reservation.transfer_to_job();
    } catch (const std::bad_alloc &) {
        throw;
    } catch (const std::exception &error) {
        return failure<SliceJob>(ErrorCode::internal, error.what(), "/engine");
    }
    return detail::ResultAccess::success(SliceJob(std::move(job)),
                                         std::move(diagnostics));
}

} // namespace libslicer::v1
