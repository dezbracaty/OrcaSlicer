#pragma once

#include "Config.hpp"
#include "Export.hpp"
#include "Toolpath.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace libslicer {

class CalibrationTask;
enum class CalibMode;
struct CalibrationRequest;
struct CalibrationPrepareResult;
struct CalibrationOutputOptions;

LIBSLICER_API const char* version() noexcept;

struct PrintableAreaPoint
{
    double x{0.0};
    double y{0.0};
};

struct MachineVariantOption
{
    std::string id;
    std::string name;
    double nozzle_diameter{0.0};
    double printable_width{0.0};
    double printable_depth{0.0};
    double printable_height{0.0};
    std::size_t physical_tool_count{1};
    bool variable_filament_slots{false};
    bool filament_slots_bound_to_physical_tools{false};
    std::size_t max_filament_slots{1};
    std::vector<int> toolhead_filament_capacity;
    std::vector<PrintableAreaPoint> printable_area;
    std::string printer_preset_id;
};

struct MachineModelOption
{
    std::string id;
    std::string vendor_id;
    std::string name;
    std::string family;
    std::string cover_image_path;
    std::string bed_model_path;
    std::string bed_texture_path;
    std::vector<MachineVariantOption> variants;
};

struct BuildPlateOption
{
    std::string value;
    std::string name;
    std::string image_path;
};

struct PresetOption
{
    std::string id;
    std::string name;
    bool is_default{false};
};

struct ConfigSelection
{
    std::string machine_model_id;
    std::string machine_variant_id;
    std::string process_preset_id;
    std::vector<std::string> filament_preset_ids;
    // Optional physical tool IDs, one per material. Never inferred from material type.
    std::vector<unsigned> filament_physical_tools;
    std::string printer_preset_id;
};

struct ResolvedSelection
{
    std::string machine_model_id;
    std::string machine_variant_id;
    std::string printer_preset_id;
    std::string process_preset_id;
    std::vector<std::string> filament_preset_ids;
    std::vector<unsigned> filament_physical_tools;
};

struct LibraryOptions
{
    // Empty in production: Library locates the resources installed with it.
    // Tests and SDK embedding may provide an explicit libslicer resource root.
    std::string resource_directory;
    std::vector<std::string> vendors;
    // Optional writable root for Orca user presets, separate from slice work files.
    std::string user_preset_directory;
};

struct ConfigCreateResult
{
    bool success{false};
    ResolvedSelection selection;
    std::vector<PresetOption> compatible_processes;
    std::vector<PresetOption> compatible_filaments;
    std::vector<ConfigDiagnostic> diagnostics;
    std::unique_ptr<Config> config;

    explicit operator bool() const noexcept { return success; }
};

struct Rgba8
{
    std::uint8_t red{0};
    std::uint8_t green{0};
    std::uint8_t blue{0};
    std::uint8_t alpha{255};
};

struct FilamentSlotInfo
{
    std::size_t index{0};
    std::string preset_id;
    std::string preset_name;
    std::string vendor;
    std::string material_type;
    Rgba8 color;
    double diameter_mm{1.75};
    std::size_t physical_tool_index{0};
    std::string physical_tool_name;
    std::string physical_tool_role;
    std::string physical_tool_side;
    double nozzle_diameter_mm{0.0};
    std::vector<PresetOption> compatible_presets;
    bool preset_modified{false};
};

struct ActiveConfigView
{
    std::uint64_t revision{0};
    ResolvedSelection selection;
    std::vector<PresetOption> compatible_processes;
    std::vector<PresetOption> compatible_filaments;
    std::vector<SettingItem> settings;
    std::vector<FilamentSlotInfo> filament_slots;
    std::vector<std::string> modified_process_keys;
    bool printer_preset_modified{false};
    bool valid() const noexcept { return revision != 0; }
};

struct ConfigActivationResult
{
    bool success{false};
    ActiveConfigView view;
    std::vector<ConfigDiagnostic> diagnostics;

    explicit operator bool() const noexcept { return success; }
};

enum class PrinterSwitchPreset { Printer, Process, Filament };
enum class PrinterSwitchAction { Transfer, Discard, Cancel };

struct PrinterSwitchChange
{
    std::string key;
    std::string before;
    std::string after;
    bool transferable{false};
};

struct PrinterSwitchReview
{
    struct Preset {
        PrinterSwitchPreset type;
        std::string name;
        bool transfer_allowed;
        bool cancel_switch;
        bool save_as;
        std::vector<PrinterSwitchChange> changes;
    };
    std::uint64_t revision{0};
    std::vector<Preset> presets;
    std::vector<ConfigDiagnostic> diagnostics;
};

struct PrinterSwitchDecision
{
    PrinterSwitchPreset type;
    PrinterSwitchAction action;
};

struct PrinterSwitchSaveResult
{
    ActiveConfigView view;
    std::vector<ConfigDiagnostic> diagnostics;
    std::string overwrite_warning;
    explicit operator bool() const noexcept { return view.valid(); }
};

struct SliceVertex
{
    float x{0.0f};
    float y{0.0f};
    float z{0.0f};
};

struct SliceTriangle
{
    std::uint32_t vertex_a{0};
    std::uint32_t vertex_b{0};
    std::uint32_t vertex_c{0};
};

struct SliceFacetLabelRoot
{
    std::uint32_t triangle_index{0};
    std::uint32_t bitstream_start_index{0};
};

struct SliceFacetLabels
{
    std::vector<SliceFacetLabelRoot> roots;
    std::vector<std::uint8_t> bitstream;

    bool empty() const noexcept { return roots.empty() && bitstream.empty(); }
    bool valid() const noexcept { return roots.empty() == bitstream.empty(); }
};

enum class SliceVolumeRole
{
    ModelPart,
    SupportEnforcer,
    SupportBlocker,
    NegativeVolume,
    ParameterModifier
};

struct SliceVolumeInput
{
    std::vector<SliceVertex> vertices;
    std::vector<SliceTriangle> triangles;
    int default_filament_slot{1};
    SliceFacetLabels facet_labels;
    SliceVolumeRole role{SliceVolumeRole::ModelPart};
    std::string project_semantics;
    std::string surface_annotations;
    bool inherit_object_filament{false};
    // Native volume-local to object-local transform. Keep facet topology local,
    // including mirrored and nonuniformly scaled parts.
    std::array<double, 16> local_transform{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
};

// Geometry-only plane cutting contract. The mesh is expressed in model-local
// coordinates and mesh_to_plane maps those coordinates into a space where the
// cutting plane is z == 0. Results are returned in the original model-local
// coordinates and include triangulated cut caps when the input is closed.
struct PlaneCutMesh
{
    std::vector<SliceVertex> vertices;
    std::vector<SliceTriangle> triangles;

    bool empty() const noexcept { return vertices.empty() || triangles.empty(); }
};

struct PlaneCutRequest
{
    PlaneCutMesh mesh;
    std::array<double, 16> mesh_to_plane{
        1.0, 0.0, 0.0, 0.0,
        0.0, 1.0, 0.0, 0.0,
        0.0, 0.0, 1.0, 0.0,
        0.0, 0.0, 0.0, 1.0};
};

struct PlaneCutResult
{
    bool success{false};
    PlaneCutMesh upper;
    PlaneCutMesh lower;
    std::string error;

    explicit operator bool() const noexcept { return success; }
};

LIBSLICER_API PlaneCutResult cut_mesh_with_plane(
    const PlaneCutRequest& request);

enum class FiberMaskShape { Box, Cylinder, Sphere };

// A centered primitive in its local frame, attached to one SliceObjectInput.
// Only in-memory volume inputs support masks. File inputs with masks are rejected.
// Regions use native modifier precedence: later masks override earlier masks
// where they intersect; unspecified settings inherit the underlying region.
struct SliceFiberMaskInput
{
    // Nonempty and unique within the owning object, including disabled masks.
    std::string id;
    FiberMaskShape shape{FiberMaskShape::Box};
    // Box: XYZ lengths. Cylinder: matching XY diameters and Z height.
    // Sphere: matching XYZ diameters. All sizes are positive finite millimeters.
    std::array<double, 3> dimensions_mm{1.0, 1.0, 1.0};
    // Row-major rigid affine transform from mask-local to object coordinates.
    // Scale belongs in dimensions_mm; the object's parent transform may scale
    // or mirror both the model and masks together.
    std::array<double, 16> mask_to_object{
        1.0, 0.0, 0.0, 0.0,
        0.0, 1.0, 0.0, 0.0,
        0.0, 0.0, 1.0, 0.0,
        0.0, 0.0, 0.0, 1.0};
    // Disabled masks still validate ID, shape, dimensions and local pose, but
    // do not validate settings or produce a native modifier.
    bool enabled{true};
    // Partial serialized patch restricted to ConfigScope::FiberMask parameters.
    std::vector<std::pair<std::string, std::string>> settings;
};

// Dimensions and transformed coordinates are limited to 1,000,000 mm.
// Curved masks use the configured resolution and reject tessellation finer
// than 2*pi/1024 radians or meshes exceeding 1,000,000 triangles.
// Effective mask regions currently reject infill_combination, interface_shells
// and infill_only_where_needed; invalid inputs return slice diagnostics.

struct SliceObjectInput
{
    // File input remains available for compatibility. New document slicing
    // uses immutable in-memory volumes so transforms and facet annotations are
    // not discarded through an STL intermediate. Exactly one of model_path or
    // volumes must be populated.
    std::string model_path;
    std::vector<std::string> support_enforcer_paths;
    std::string name;
    std::array<double, 16> transform{
        1.0, 0.0, 0.0, 0.0,
        0.0, 1.0, 0.0, 0.0,
        0.0, 0.0, 1.0, 0.0,
        0.0, 0.0, 0.0, 1.0};
    std::vector<SliceVolumeInput> volumes;
    std::vector<SliceFiberMaskInput> fiber_masks;
    std::string project_semantics;
    int default_filament_slot{0};
};

enum class OutputArtifactOwnership
{
    // Created because no output path was requested. The consumer must retain
    // it while needed and remove it when the last preview releases it.
    LibraryTemporary,
    // Written to a path explicitly supplied by the caller. The library never
    // removes this path.
    CallerOwned
};

struct OutputArtifact
{
    std::string path;
    OutputArtifactOwnership ownership{OutputArtifactOwnership::CallerOwned};
};

struct SliceRequest
{
    // Project/default material and plate semantics are immutable request inputs.
    std::string project_semantics;
    int default_filament_slot{1};
    // Each support mesh is associated with exactly one printable object.
    std::vector<SliceObjectInput> objects;
    ConfigSnapshot config;
    std::string output_gcode_path;
    // A sliced G-code 3MF is a packaged print job containing the generated
    // G-code and its slicing metadata. Supplying a path also enables it.
    bool generate_gcode_3mf{false};
    std::string output_gcode_3mf_path;
    bool center_on_build_plate{true};
    bool generate_preview{true};
    // Development audit mode for the Belt support algorithm. It records and
    // renders every algorithm stage while still exporting the real G-code.
    // When an explicit audit path is supplied, the G-code is retained beside
    // it so the intermediate and final artifacts remain one audit unit.
    bool belt_support_debug_only{false};
    std::string belt_support_debug_output_path;
};

struct SliceSummary
{
    double estimated_time_seconds{0.0};
    double filament_used_mm{0.0};
    double filament_weight_g{0.0};
    std::size_t layer_count{0};
    std::size_t logical_motion_count{0};
    std::size_t render_segment_count{0};
};

struct SliceDiagnostic
{
    std::string code;
    std::string message;
    bool warning{false};
    std::string option_key;
};

// Views are valid only for the synchronous callback invocation. Layer counts
// describe the current object's infill work, not a time estimate or all objects.
struct SliceProgress
{
    float progress{0.0f};
    std::string_view stage;
    std::size_t completed_layers{0};
    std::size_t total_layers{0};
    std::string_view detail;
};

struct SliceCallbacks
{
    std::function<void(float progress, std::string_view stage)> progress;
    std::function<bool()> is_cancelled;
    // Optional richer notification; when set it replaces the legacy progress
    // callback. Source strings remain untranslated for the consumer's locale.
    std::function<void(const SliceProgress&)> progress_details;
};

struct SliceResult
{
    bool success{false};
    bool cancelled{false};
    OutputArtifact output;
    OutputArtifact gcode_3mf;
    OutputArtifact belt_support_debug;
    SliceSummary summary;
    ToolpathPreviewPtr preview;
    std::vector<SliceDiagnostic> diagnostics;

    explicit operator bool() const noexcept { return success; }
};

struct GCodePreviewRequest
{
    std::string gcode_path;
};

struct GCodePreviewResult
{
    bool success{false};
    bool cancelled{false};
    ToolpathPreviewPtr preview;
    std::vector<SliceDiagnostic> diagnostics;

    explicit operator bool() const noexcept { return success; }
};

struct ProjectImportVertex
{
    float x{0.0f};
    float y{0.0f};
    float z{0.0f};
};

struct ProjectImportTriangle
{
    std::uint32_t vertex_a{0};
    std::uint32_t vertex_b{0};
    std::uint32_t vertex_c{0};
};

// Compact split-tree annotation used by Orca/Bambu facet painting. Label 0
// means the volume default; positive labels are one-based filament indices.
struct ProjectImportFacetLabelRoot
{
    std::uint32_t triangle_index{0};
    std::uint32_t bitstream_start_index{0};
};

struct ProjectImportFacetLabels
{
    std::vector<ProjectImportFacetLabelRoot> roots;
    std::vector<std::uint8_t> bitstream;

    bool empty() const noexcept { return roots.empty() || bitstream.empty(); }
};

enum class ProjectImportPartRole
{
    Model,
    SupportEnforcer,
    SupportBlocker
};

struct ProjectImportPart
{
    std::string id;
    std::string name;
    ProjectImportPartRole role{ProjectImportPartRole::Model};
    // Row-major affine matrix. Coordinates and translations use millimetres.
    std::array<double, 16> local_transform{
        1.0, 0.0, 0.0, 0.0,
        0.0, 1.0, 0.0, 0.0,
        0.0, 0.0, 1.0, 0.0,
        0.0, 0.0, 0.0, 1.0};
    std::string filament_id;
    std::vector<ProjectImportVertex> vertices;
    std::vector<ProjectImportTriangle> triangles;
    ProjectImportFacetLabels facet_labels;
};

struct ProjectImportInstance
{
    std::string name;
    // Row-major affine matrix. Coordinates and translations use millimetres.
    std::array<double, 16> transform{
        1.0, 0.0, 0.0, 0.0,
        0.0, 1.0, 0.0, 0.0,
        0.0, 0.0, 1.0, 0.0,
        0.0, 0.0, 0.0, 1.0};
    bool printable{true};
};

struct ProjectImportObject
{
    std::string id;
    std::string name;
    std::vector<ProjectImportPart> parts;
    std::vector<ProjectImportInstance> instances;
};

struct ProjectImportColor
{
    float red{0.8f};
    float green{0.8f};
    float blue{0.8f};
    float alpha{1.0f};
};

struct ProjectImportFilament
{
    std::string id;
    std::string preset_id;
    std::string name;
    std::string vendor;
    std::string material_type;
    ProjectImportColor color;
};

struct ProjectImportRequest
{
    std::string path;
};

struct ProjectImportResult
{
    bool success{false};
    bool cancelled{false};
    std::vector<ProjectImportObject> objects;
    std::vector<ProjectImportFilament> filaments;
    std::vector<SliceDiagnostic> diagnostics;

    explicit operator bool() const noexcept { return success; }
};

using ProjectSettings = std::vector<std::pair<std::string, std::string>>;
enum class ProjectReadMode { Geometry, Project };
enum class ProjectContainerKind { Model, Project, GCodeJob };
enum class ProjectPartRole { Model, NegativeVolume, ParameterModifier, SupportEnforcer, SupportBlocker };
enum class ProjectDecisionKind {
    LegacyInfillRotation, ConvertMeters, ConvertInches,
    MultipartObject, ScaleOversizedInstance
};
enum class ProjectDecisionAnswer { Yes, No, Cancel };
struct ProjectDecision {
    ProjectDecisionKind kind;
    std::size_t object_index{std::size_t(-1)}, instance_index{std::size_t(-1)};
    std::string message;
};
// Called synchronously on the read thread. Do not reenter this Library or its
// owning executor while waiting for a GUI answer. The caller owns cancellation
// and wakeup of any such wait; external cancellation is checked before/after it.
using ProjectDecisionCallback = std::function<ProjectDecisionAnswer(const ProjectDecision&)>;
// Snapshot before this operation. Project mode performs the native logical
// reset first; if reset changes the bed, its geometry/dimensions are rebuilt
// from the reset printer using this snapshot's axes length.
struct ProjectBedContext {
    double minimum_x{0}, minimum_y{0}, maximum_x{0}, maximum_y{0};
    bool current_plate_empty{true};
    // Native Bed3D position; Project reset selects plate zero.
    PrintableAreaPoint shape_position_mm{0,0};
    // Transformed per-instance convex hulls, in native world millimetres.
    std::vector<std::vector<PrintableAreaPoint>> occupied_hulls;
    // Captured native PartPlateList dimensions, if the caller has an existing
    // GUI bed. Otherwise derived with Bed3D's axes/bounds and int conversion.
    std::optional<std::array<int,3>> native_plate_size;
    std::optional<float> axes_length_mm;
};
struct ProjectDiagnostic {
    std::string severity, code, context, message;
};
struct ProjectMesh {
    std::vector<ProjectImportVertex> vertices;
    std::vector<ProjectImportTriangle> triangles;
};
struct ProjectLayerRange {
    double minimum_z{0}, maximum_z{0};
    ProjectSettings settings;
};
struct ProjectBrimPoint { float x{0}, y{0}, z{0}, head_front_radius{0}; };
struct ProjectCustomGCode {
    double print_z{0}; int type{0}; int filament_slot{0};
    std::string color, extra;
};
struct ProjectPart {
    std::string id, name;
    ProjectPartRole role{ProjectPartRole::Model};
    std::array<double,16> local_transform{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
    int default_filament_slot{0};
    bool explicit_filament_binding{false};
    std::shared_ptr<const ProjectMesh> mesh;
    ProjectSettings settings;
    SliceFacetLabels material_facets, support_facets, seam_facets, fuzzy_skin_facets;
};
struct ProjectInstance {
    std::string id, name, plate_id;
    std::array<double,16> transform{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
    bool printable{true};
};
struct ProjectObject {
    std::string id, name;
    int default_filament_slot{0};
    ProjectSettings settings;
    std::vector<ProjectLayerRange> layer_ranges;
    std::vector<double> layer_height_profile;
    std::vector<ProjectBrimPoint> brim_points;
    std::vector<ProjectPart> parts;
    std::vector<ProjectInstance> instances;
};
struct ProjectPlate {
    std::string id, name;
    int native_plate_index{0};
    bool locked{false};
    ProjectSettings settings;
    int custom_gcode_mode{0};
    std::vector<ProjectCustomGCode> custom_gcodes;
    std::array<double,3> origin_mm{0,0,0};
    std::array<int,3> native_size_mm{0,0,0};
    // Original no-model job validity is independent of a drawable preview.
    // Model projects keep G-code bytes as attachments with this flag false.
    bool slice_result_valid{false};
    std::string gcode;
    ToolpathPreviewPtr preview;
};
struct ProjectMachine { MachineModelOption model; MachineVariantOption variant; bool project_local{false}; };
struct ProjectScene {
    ProjectContainerKind kind{ProjectContainerKind::Model};
    ProjectReadMode mode{ProjectReadMode::Geometry};
    std::string producer, version;
    std::vector<ProjectObject> objects;
    std::vector<ProjectPlate> plates;
    std::vector<FilamentSlotInfo> source_filaments;
    std::size_t active_plate_index{0};
    bool preview_only{false};
};
class LIBSLICER_API ConfigurationState final {
public:
    ConfigurationState();
    bool valid() const noexcept;
    const ActiveConfigView& view() const;
    const ProjectMachine& machine() const;
    ConfigSnapshot snapshot() const;
private:
    class Impl;
    std::shared_ptr<Impl> impl_;
    std::uint64_t captured_revision_{0};
    std::size_t filament_color_cursor_{0};
    std::weak_ptr<const void> capture_origin_;
    friend class Library;
};
struct ProjectReadRequest {
    std::string path;
    ProjectReadMode mode{ProjectReadMode::Geometry};
    // Transitional source compatibility. Unknown settings never need consent;
    // the legacy true value supplies Yes only if no typed callback is supplied.
    bool accept_legacy_infill_rotation_fix{false};
    bool accept_unknown_settings{false};
    std::optional<ConfigurationState> configuration;
    std::optional<ProjectBedContext> bed;
    std::optional<std::size_t> filament_color_cursor;
};
struct ProjectReadResult {
    bool success{false}, cancelled{false};
    std::shared_ptr<const ProjectScene> scene;
    ConfigurationState configuration;
    std::vector<ProjectDiagnostic> diagnostics;
    std::vector<std::string> required_decisions;
    std::vector<std::string> ignored_settings;
    // Document lifecycle follows the entry, configuration follows load_files.
    bool replace_document{false}, load_config{false}, configuration_changed{false};
    std::uint64_t configuration_revision{0};
    std::size_t filament_color_cursor{0};
    explicit operator bool() const noexcept { return success; }
};
struct ConfigurationInstallResult {
    bool success{false};
    std::uint64_t revision{0};
    ConfigurationState previous;
    std::vector<ConfigDiagnostic> diagnostics;
    explicit operator bool() const noexcept { return success; }
};
struct PrinterSwitchResult {
    ConfigurationState configuration;
    std::vector<ConfigDiagnostic> diagnostics;
    explicit operator bool() const noexcept { return configuration.valid(); }
};
LIBSLICER_API std::string encode_project_object_semantics(const ProjectObject&);
LIBSLICER_API std::string encode_project_part_semantics(const ProjectPart&);
LIBSLICER_API std::string encode_project_part_annotations(const ProjectPart&);
LIBSLICER_API std::string encode_project_plate_semantics(const ProjectPlate&);
LIBSLICER_API SettingsResult edit_project_override(const ConfigSnapshot& base,
    std::string_view plate, std::string& target, std::optional<std::string_view> parent_object,
    std::string_view key, std::optional<std::string_view> value);
// Apply Orca's object-list cleanup for support slots beyond the new palette.
LIBSLICER_API std::string synchronize_project_model_materials(
    std::string_view semantics, std::string_view scope, std::size_t filament_count);
// Call only when the machine/material domain changes, before publishing the
// new configuration to consumers. Preserves unrelated semantics and applicable
// manual bindings; stale bindings inherit target_config. Does not repair slice
// requests or user edits. Malformed semantics/invalid snapshots throw.
LIBSLICER_API std::string synchronize_project_plate_materials(
    std::string_view semantics, const ConfigSnapshot& target_config,
    std::vector<ConfigDiagnostic>* diagnostics = nullptr, bool update_print_sequence = false,
    bool reset_single_tool_mapping = false);
class LIBSLICER_API Library final
{
public:
    static std::unique_ptr<Library> open(const LibraryOptions& options = {},
                                         std::vector<ConfigDiagnostic>* diagnostics = nullptr);

    Library(const Library&) = delete;
    Library(Library&&) noexcept;
    Library& operator=(const Library&) = delete;
    Library& operator=(Library&&) noexcept;
    ~Library();

    const std::string& resource_directory() const noexcept;
    const std::vector<MachineModelOption>& machine_models() const noexcept;
    std::vector<MachineModelOption> project_machine_models() const;
    const std::vector<BuildPlateOption>& build_plate_options() const noexcept;
    std::vector<PresetOption> printer_presets(std::string_view model, std::string_view variant) const;
    ConfigCreateResult create_config(const ConfigSelection& selection) const;
    ConfigActivationResult activate_config(
        const ConfigSelection& selection,
        const std::vector<std::pair<std::string, std::string>>& patch = {});
    PrinterSwitchReview review_printer_switch(const ConfigSelection& target, bool after_save = false) const;
    PrinterSwitchSaveResult save_printer_switch_preset(
        std::uint64_t expected_revision, PrinterSwitchPreset type, std::string_view save_name,
        bool validate_only = false, std::string_view source_preset_id = {});
    PrinterSwitchResult prepare_printer_switch(
        const ConfigSelection& target, std::uint64_t expected_revision,
        const std::vector<PrinterSwitchDecision>& decisions,
        const std::vector<std::pair<std::string, std::string>>& remembered_project_settings = {});
    std::optional<ActiveConfigView> active_config() const;
    std::optional<ConfigSnapshot> active_config_snapshot() const;
    SettingsResult apply_active_config_patch(
        const std::vector<std::pair<std::string, std::string>>& patch);
    SettingsResult set_active_config_value(std::string_view key, std::string_view value);
    SettingsResult reset_active_config_value(std::string_view key);
    // Discard edits to the selected native preset. Filament requires an explicit
    // zero-based slot; all references to that preset are restored together.
    // Project palette/mapping and edits to other presets are preserved.
    ConfigActivationResult restore_active_preset(SettingGroup group,
        std::optional<std::size_t> filament_slot = std::nullopt);
    ConfigActivationResult set_active_filament_preset(
        std::size_t slot_index, std::string_view preset_id);
    ConfigActivationResult resize_active_filament_slots(std::size_t slot_count);
    ConfigActivationResult add_active_filament(std::size_t physical_tool, std::string_view preset_id = {});
    SettingsResult set_active_filament_color(std::size_t slot_index, Rgba8 color);
    std::vector<ConfigDiagnostic> validate_active_config() const;
    SliceResult slice(const SliceRequest& request, const SliceCallbacks& callbacks = {}) const;
    // Manual calibration mirrors the original calib_dlg -> Plater entries.
    // Include Calibration.hpp for request, task and output definitions.
    std::vector<CalibMode> calibration_modes() const;
    CalibrationPrepareResult prepare_calibration(const CalibrationRequest& request,
        const SliceCallbacks& callbacks = {}) const;
    SliceResult slice_calibration(const CalibrationTask& task, std::size_t plate_index,
        const CalibrationOutputOptions& output, const SliceCallbacks& callbacks = {}) const;
    GCodePreviewResult load_gcode_preview(const GCodePreviewRequest& request,
                                          const SliceCallbacks& callbacks = {}) const;
    ProjectImportResult import_project(const ProjectImportRequest& request,
                                       const SliceCallbacks& callbacks = {}) const;
    ProjectReadResult read_project(const ProjectReadRequest&, const SliceCallbacks& = {}) const;
    ProjectReadResult read_project(const ProjectReadRequest&, const SliceCallbacks&,
        const ProjectDecisionCallback&) const;
    // Explicit mapping returns geometry in target_config's material domain.
    // It never returns the source read's configuration-install candidate.
    ProjectReadResult remap_project_materials(const ProjectReadResult&,
        const std::vector<unsigned>& source_to_target_one_based,
        const ConfigSnapshot& target_config, const SliceCallbacks& = {}) const;
    std::optional<ProjectMachine> active_project_machine() const;
    std::uint64_t configuration_revision() const noexcept;
    ConfigurationState capture_configuration() const;
    ConfigurationInstallResult install_configuration(const ConfigurationState&, std::uint64_t expected_revision);

private:
    Library();
    SliceResult slice_impl(const SliceRequest& request, const SliceCallbacks& callbacks,
        const CalibrationTask* calibration, std::size_t plate_index) const;

    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace libslicer
