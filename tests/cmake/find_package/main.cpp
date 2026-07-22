#include <libslicer/v1/Config.hpp>
#include <libslicer/v1/ConfigSchema.hpp>
#include <libslicer/v1/Context.hpp>
#include <libslicer/v1/Preset.hpp>
#include <libslicer/v1/Project.hpp>
#include <libslicer/v1/Result.hpp>
#include <libslicer/v1/Slice.hpp>
#include <libslicer/v1/Version.hpp>

#include <iostream>

int main()
{
    using namespace libslicer::v1;

    static_assert(sdk_version_major == 1, "consumer expects libslicer SDK v1");

    const OptionId option("sparse_infill_density");
    ConfigPatch patch;
    patch.set(option, ConfigValue::percent(15.0));
    if (patch.find(option) != ConfigValue::percent(15.0))
        return 1;

    ConfigPatch copy = patch;
    copy.set(option, ConfigValue::percent(20.0));
    if (patch.find(option) != ConfigValue::percent(15.0))
        return 2;
    if (copy.find(option) != ConfigValue::percent(20.0))
        return 3;

    // Ensure the installed static archive carries the private Orca link
    // closure, rather than only supporting header-only/config-only consumers.
    auto context = SdkContext::create(ContextOptions{});
    if (context.has_value() || context.error_code() != ErrorCode::invalid_argument)
        return 4;

    std::cout << "found libslicer::sdk_v1 " << sdk_version_major << '.'
              << sdk_version_minor << '.' << sdk_version_patch << '\n';
    return 0;
}
