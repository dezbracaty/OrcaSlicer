#include <filesystem>
#include <iostream>
#include <type_traits>

#include <libslicer_worker/WorkerClient.hpp>
#include <libslicer_worker/WorkerEvent.hpp>
#include <libslicer_worker/WorkerProtocol.hpp>

int main()
{
    static_assert(!std::is_copy_constructible_v<libslicer::worker::WorkerClient>);
    static_assert(!std::is_copy_assignable_v<libslicer::worker::WorkerClient>);
    static_assert(std::is_move_constructible_v<libslicer::worker::WorkerClient>);
    static_assert(std::is_move_assignable_v<libslicer::worker::WorkerClient>);

    libslicer::worker::WorkerEvent event;
    event.type = libslicer::worker::WorkerEventType::Progress;
    event.job_id = "find-package-smoke";
    event.percent = 50;

    const std::string line = libslicer::worker::event_to_json_line(event);
    if (line.find("\"type\":\"progress\"") == std::string::npos) {
        std::cerr << "worker protocol did not serialize a progress event\n";
        return 1;
    }

    if (!std::filesystem::exists(LIBSLICER_WORKER_EXE)) {
        std::cerr << "worker executable does not exist: " << LIBSLICER_WORKER_EXE << '\n';
        return 2;
    }

    libslicer::worker::WorkerClient client([](const libslicer::worker::WorkerEvent&) {});
    if (client.running()) {
        std::cerr << "new worker client should not be running\n";
        return 3;
    }

    std::cout << "worker=" << LIBSLICER_WORKER_EXE << '\n';
    return 0;
}
