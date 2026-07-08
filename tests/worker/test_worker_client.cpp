#include "libslicer_worker/WorkerClient.hpp"
#include "libslicer_worker/WorkerProtocol.hpp"

#include <catch2/catch_all.hpp>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <future>
#include <string>
#include <thread>

using namespace libslicer::worker;

#ifndef _WIN32
namespace {

bool recv_line(int fd, std::string& line)
{
    line.clear();
    for (;;) {
        char ch = 0;
        const ssize_t count = ::recv(fd, &ch, 1, 0);
        if (count <= 0)
            return false;
        if (ch == '\n')
            return true;
        line.push_back(ch);
    }
}

bool descriptor_is_closed(int fd)
{
    errno = 0;
    return ::fcntl(fd, F_GETFD) < 0 && errno == EBADF;
}

} // namespace

TEST_CASE("WorkerClient closes delivered event file descriptors after callback", "[worker][client]")
{
    const std::filesystem::path socket_path =
        "/tmp/libslicer-worker-client-fd-" + std::to_string(::getpid()) + ".sock";
    ::unlink(socket_path.string().c_str());

    std::promise<void> listening;
    std::promise<int> delivered_fd;
    std::promise<void> server_done;

    std::thread server([&]() {
        const int server_fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        REQUIRE(server_fd >= 0);

        sockaddr_un addr {};
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, socket_path.string().c_str(), sizeof(addr.sun_path) - 1);
        REQUIRE(::bind(server_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
        REQUIRE(::listen(server_fd, 1) == 0);
        listening.set_value();

        const int client_fd = ::accept(server_fd, nullptr, nullptr);
        REQUIRE(client_fd >= 0);

        std::string hello;
        REQUIRE(recv_line(client_fd, hello));
        REQUIRE(event_from_json_line(hello).has_value());

        const std::filesystem::path temp_path =
            std::filesystem::temp_directory_path() /
            ("libslicer-worker-client-fd-payload-" + std::to_string(::getpid()) + ".bin");
        int payload_fd = ::open(temp_path.string().c_str(), O_CREAT | O_TRUNC | O_RDWR, 0600);
        REQUIRE(payload_fd >= 0);
        CHECK(::unlink(temp_path.string().c_str()) == 0);

        constexpr const char payload[] = "worker-client-fd";
        REQUIRE(::write(payload_fd, payload, sizeof(payload) - 1) == static_cast<ssize_t>(sizeof(payload) - 1));

        WorkerEvent event;
        event.type = WorkerEventType::Artifact;
        event.job_id = "worker-client-fd-job";
        event.kind = "preview";
        event.transport = "shared_memory_fd";
        event.size = sizeof(payload) - 1;
        event.complete = true;
        event.file_descriptor = payload_fd;
        REQUIRE(send_worker_event(client_fd, event));

        ::close(payload_fd);
        ::close(client_fd);
        ::close(server_fd);
        ::unlink(socket_path.string().c_str());
        server_done.set_value();
    });

    REQUIRE(listening.get_future().wait_for(std::chrono::seconds(5)) == std::future_status::ready);

    WorkerOptions options;
    options.socket_path = socket_path;
    options.startup_timeout = std::chrono::seconds(5);

    WorkerClient client([&](const WorkerEvent& event) {
        if (event.type == WorkerEventType::Artifact &&
            event.kind == "preview" &&
            event.transport == "shared_memory_fd") {
            delivered_fd.set_value(event.file_descriptor);
        }
    });
    INFO(client.last_error());
    REQUIRE(client.connect(options));

    auto delivered = delivered_fd.get_future();
    REQUIRE(delivered.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    const int fd = delivered.get();
    REQUIRE(fd >= 0);

    bool closed = false;
    for (int attempt = 0; attempt < 100; ++attempt) {
        if (descriptor_is_closed(fd)) {
            closed = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(closed);

    client.kill();
    REQUIRE(server_done.get_future().wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    server.join();
}
#endif
