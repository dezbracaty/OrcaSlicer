#include <libslicer/Export.hpp>
#include <functional>
#include <stdexcept>
#include <tbb/global_control.h>
#include <tbb/task_arena.h>

// This hook belongs only to the test DLL assembled from production API objects.
// A control in the caller cannot constrain the DLL's private static TBB runtime.
namespace libslicer::testing {
LIBSLICER_API void with_serial_runtime(const std::function<void()>& run)
{
    tbb::global_control threads(tbb::global_control::max_allowed_parallelism, 1);
    tbb::task_arena arena(1);
    arena.execute([&] {
        if (tbb::global_control::active_value(tbb::global_control::max_allowed_parallelism) != 1 ||
            tbb::this_task_arena::max_concurrency() != 1)
            throw std::runtime_error("Calibration reference SDK runtime is not serial");
        run();
    });
}
}
