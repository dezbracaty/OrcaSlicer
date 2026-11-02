#pragma once

#include <libslicer/Library.hpp>

namespace libslicer_test {

// Test geometry is loaded through the public snapshot protocol before slicing.
// Keeping fixture geometry here lets negative cases mutate their input without
// introducing another production slicing entry point.
struct SliceInput : libslicer::SliceRequest
{
    libslicer::ConfigSnapshot config;
    std::vector<libslicer::SliceObjectInput> objects;
    bool center_on_build_plate{true};
};

inline libslicer::SliceResult load_and_slice(const libslicer::Library& library,
                                             const SliceInput& input,
                                             const libslicer::SliceCallbacks& callbacks = {})
{
    libslicer::ModelLoadRequest load;
    load.objects               = input.objects;
    load.config                = input.config;
    load.center_on_build_plate = input.center_on_build_plate;
    const auto loaded          = library.load_model(load, callbacks);
    if (!loaded.success) {
        libslicer::SliceResult result;
        result.cancelled   = loaded.cancelled;
        result.diagnostics = loaded.diagnostics;
        return result;
    }
    libslicer::SliceRequest request = input;
    request.model                   = loaded.model;
    return library.slice(request, callbacks);
}

} // namespace libslicer_test
