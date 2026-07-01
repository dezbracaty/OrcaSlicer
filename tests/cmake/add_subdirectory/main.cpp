#include <iostream>
#include <string>

#include <libslic3r/PrintConfig.hpp>
#include <libslic3r/Utils.hpp>
#include <libslicer_worker/WorkerClient.hpp>
#include <libslicer_worker/WorkerEvent.hpp>
#include <libslicer_worker/WorkerProtocol.hpp>

int main()
{
    Slic3r::set_resources_dir(TEST_LIBSLICER_RESOURCES_DIR);
    Slic3r::set_data_dir(".");

    const auto &defs = Slic3r::print_config_def;
    libslicer::worker::WorkerEvent event;
    event.type = libslicer::worker::WorkerEventType::Progress;
    event.job_id = "add-subdirectory-smoke";
    event.percent = 1;
    const std::string line = libslicer::worker::event_to_json_line(event);
    if (line.find("\"type\":\"progress\"") == std::string::npos)
        return 2;

    libslicer::worker::WorkerClient client([](const libslicer::worker::WorkerEvent&) {});
    if (client.running())
        return 3;

    std::cout << "options=" << defs.options.size() << '\n';
    return defs.options.empty() ? 1 : 0;
}
