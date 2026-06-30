#include <iostream>

#include <libslic3r/PrintConfig.hpp>
#include <libslic3r/Utils.hpp>

int main()
{
    Slic3r::set_resources_dir(TEST_LIBSLICER_RESOURCES_DIR);
    Slic3r::set_data_dir(".");

    const auto &defs = Slic3r::print_config_def;
    std::cout << "options=" << defs.options.size() << '\n';
    return defs.options.empty() ? 1 : 0;
}
