#include "libslicer_worker/WorkerProtocol.hpp"
#include "libslicer_worker/WorkerServer.hpp"

#include <iostream>
#include <string>
#include <vector>

namespace {

void usage()
{
    std::cerr
        << "Usage:\n"
        << "  orcaslicer-worker slice --job <request.json> [--progress jsonl]\n"
        << "  orcaslicer-worker serve --socket <path>\n";
}

std::string arg_value(const std::vector<std::string>& args, const std::string& key)
{
    for (size_t i = 0; i + 1 < args.size(); ++i)
        if (args[i] == key)
            return args[i + 1];
    return {};
}

} // namespace

int main(int argc, char** argv)
{
    std::vector<std::string> args(argv + 1, argv + argc);
    if (args.empty()) {
        usage();
        return 2;
    }

    const std::string command = args[0];
    if (command == "slice") {
        const std::string job_path = arg_value(args, "--job");
        if (job_path.empty()) {
            usage();
            return 2;
        }

        libslicer::worker::CancellationToken cancellation;
        return libslicer::worker::run_slice_job_from_request(
            job_path,
            [](const libslicer::worker::WorkerEvent& event) {
                std::cout << libslicer::worker::event_to_json_line(event);
                std::cout.flush();
            },
            cancellation);
    }

    if (command == "serve") {
        const std::string socket_path = arg_value(args, "--socket");
        if (socket_path.empty()) {
            usage();
            return 2;
        }

        libslicer::worker::ServerOptions options;
        options.socket_path = socket_path;
        return libslicer::worker::run_worker_server(options);
    }

    usage();
    return 2;
}
