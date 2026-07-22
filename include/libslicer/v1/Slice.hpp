#pragma once

#include "Project.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace libslicer::v1 {

namespace detail { struct SliceJobState; struct SliceEngineState; }

struct TemporarySliceSelection {
    PresetSelection     selection;
    FilamentMapOverride complete_manual_map;
};

struct SliceRequest {
    ProjectSnapshot                       project;
    PlateId                               plate;
    std::optional<TemporarySliceSelection> temporary_selection;
};

struct SliceInspection {
    EffectiveConfiguration effective_configuration;
    EffectiveFilamentMap   effective_filament_map;
};

enum class SliceEventKind {
    preparing,
    validating,
    slicing,
    exporting,
    warning,
    completed,
    failed,
    cancelled
};

struct SliceEvent {
    SliceEventKind kind;
    int percent;
    std::optional<Diagnostic> diagnostic;
};

using SliceCallback = std::function<void(const SliceEvent &)>;

struct FilamentUsage {
    FilamentSlotId slot;
    double length_mm;
    double volume_mm3;
    double mass_g;
};

struct SliceStatistics {
    std::chrono::milliseconds elapsed;
    std::uint64_t layer_count;
    std::vector<FilamentUsage> filament_usage;
};

struct SliceResult {
    std::string gcode_bytes;
    EffectiveConfiguration effective_configuration;
    EffectiveFilamentMap effective_filament_map;
    SliceStatistics statistics;
    std::vector<Diagnostic> diagnostics;
};

class SliceJob {
public:
    Result<void> cancel();
    Result<std::shared_ptr<const SliceResult>> wait();

private:
    explicit SliceJob(std::shared_ptr<detail::SliceJobState> state);
    std::shared_ptr<detail::SliceJobState> state_;
    friend class SliceEngine;
};

class SliceEngine {
public:
    Result<SliceInspection> inspect(const SliceRequest &request) const;
    Result<SliceJob> submit(SliceRequest request, SliceCallback callback = {});

private:
    explicit SliceEngine(std::shared_ptr<detail::SliceEngineState> state);
    std::shared_ptr<detail::SliceEngineState> state_;
    friend class SdkContext;
};

} // namespace libslicer::v1
