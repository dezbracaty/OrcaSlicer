#pragma once

#include "Config.hpp"
#include "Export.hpp"
#include "Toolpath.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace libslicer {

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
};

struct ResolvedSelection
{
    std::string machine_model_id;
    std::string machine_variant_id;
    std::string printer_preset_id;
    std::string process_preset_id;
    std::vector<std::string> filament_preset_ids;
};

struct LibraryOptions
{
    // Empty in production: Library locates the resources installed with it.
    // Tests and SDK embedding may provide an explicit libslicer resource root.
    std::string resource_directory;
    std::vector<std::string> vendors;
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
    SupportBlocker
};

struct SliceVolumeInput
{
    std::vector<SliceVertex> vertices;
    std::vector<SliceTriangle> triangles;
    int default_filament_slot{1};
    SliceFacetLabels facet_labels;
    SliceVolumeRole role{SliceVolumeRole::ModelPart};
};

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
};

struct SliceCallbacks
{
    std::function<void(float progress, std::string_view stage)> progress;
    std::function<bool()> is_cancelled;
};

struct SliceResult
{
    bool success{false};
    bool cancelled{false};
    OutputArtifact output;
    OutputArtifact gcode_3mf;
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

struct ProjectImportMesh
{
    std::string id;
    std::string name;
    std::string filament_id;
    std::vector<ProjectImportVertex> vertices;
    std::vector<ProjectImportTriangle> triangles;
    ProjectImportFacetLabels facet_labels;
};

struct ProjectImportInstance
{
    std::string mesh_id;
    std::string name;
    // Row-major affine matrix. Coordinates and translations use millimetres.
    std::array<double, 16> transform{
        1.0, 0.0, 0.0, 0.0,
        0.0, 1.0, 0.0, 0.0,
        0.0, 0.0, 1.0, 0.0,
        0.0, 0.0, 0.0, 1.0};
    bool printable{true};
};

struct ProjectImportConfigEntry
{
    std::string key;
    std::string value;
};

enum class ProjectImportConfigGroup
{
    Project,
    Printer,
    Process
};

struct ProjectImportGroupedConfigEntry
{
    ProjectImportConfigGroup group{ProjectImportConfigGroup::Project};
    ProjectImportConfigEntry entry;
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
    std::vector<ProjectImportConfigEntry> settings;
};

struct ProjectImportRequest
{
    std::string path;
};

struct ProjectImportResult
{
    bool success{false};
    bool cancelled{false};
    std::vector<ProjectImportMesh> meshes;
    std::vector<ProjectImportInstance> instances;
    std::vector<ProjectImportFilament> filaments;
    std::vector<ProjectImportGroupedConfigEntry> config;
    std::vector<SliceDiagnostic> diagnostics;

    explicit operator bool() const noexcept { return success; }
};

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
    const std::vector<BuildPlateOption>& build_plate_options() const noexcept;
    ConfigCreateResult create_config(const ConfigSelection& selection) const;
    SliceResult slice(const SliceRequest& request, const SliceCallbacks& callbacks = {}) const;
    GCodePreviewResult load_gcode_preview(const GCodePreviewRequest& request,
                                          const SliceCallbacks& callbacks = {}) const;
    ProjectImportResult import_project(const ProjectImportRequest& request,
                                       const SliceCallbacks& callbacks = {}) const;

private:
    Library();

    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace libslicer
