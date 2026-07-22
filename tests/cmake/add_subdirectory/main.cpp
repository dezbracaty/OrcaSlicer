#include <libslicer/v1/Config.hpp>
#include <libslicer/v1/ConfigSchema.hpp>
#include <libslicer/v1/Context.hpp>
#include <libslicer/v1/Preset.hpp>
#include <libslicer/v1/Project.hpp>
#include <libslicer/v1/Result.hpp>
#include <libslicer/v1/Slice.hpp>
#include <libslicer/v1/Version.hpp>

#include <iostream>
#include <stdexcept>

int main()
{
    using namespace libslicer::v1;

    static_assert(sdk_version_major == 1, "consumer expects libslicer SDK v1");

    ConfigPatch patch;
    patch.set(OptionId("outer_wall_speed"), ConfigValue::decimal(50.0));
    patch.set(OptionId("wall_loops"), ConfigValue::integer(3));
    patch.set(OptionId("outer_wall_speed"), ConfigValue::decimal(60.0));

    if (patch.entries().size() != 2)
        return 1;
    if (patch.find(OptionId("outer_wall_speed")) != ConfigValue::decimal(60.0))
        return 2;

    const ConfigValueShape item_shape = ConfigValueShape::scalar(ConfigValueType::integer);
    auto list = ConfigValue::list(item_shape,
                                  {ConfigValue::integer(0), ConfigValue::null(item_shape)});
    if (!list.has_value() || !list.value().as_list().has_value())
        return 3;

    // Pull a non-header-only SDK translation unit into the final link.  Invalid
    // zero limits keep the smoke deterministic without requiring test assets.
    auto context = SdkContext::create(ContextOptions{});
    if (context.has_value() || context.error_code() != ErrorCode::invalid_argument)
        return 4;

    std::cout << "libslicer SDK " << sdk_version_major << '.' << sdk_version_minor << '.'
              << sdk_version_patch << '\n';
    return 0;
}
