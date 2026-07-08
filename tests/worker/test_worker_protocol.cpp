#include "libslicer_worker/WorkerProtocol.hpp"

#include <catch2/catch_all.hpp>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <array>
#include <filesystem>
#include <string>

using namespace libslicer::worker;

#ifndef _WIN32
TEST_CASE("Worker protocol sends JSON events with ancillary file descriptors", "[worker][protocol]")
{
    int sockets[2] { -1, -1 };
    REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);

    const std::filesystem::path temp_path =
        std::filesystem::temp_directory_path() / "libslicer-worker-protocol-fd-test.txt";
    int payload_fd = ::open(temp_path.string().c_str(), O_CREAT | O_TRUNC | O_RDWR, 0600);
    REQUIRE(payload_fd >= 0);
    CHECK(::unlink(temp_path.string().c_str()) == 0);

    constexpr const char payload[] = "fd-event";
    REQUIRE(::write(payload_fd, payload, sizeof(payload) - 1) == static_cast<ssize_t>(sizeof(payload) - 1));
    REQUIRE(::lseek(payload_fd, 0, SEEK_SET) == 0);

    WorkerEvent sent;
    sent.type = WorkerEventType::Artifact;
    sent.job_id = "protocol-fd-job";
    sent.kind = "preview";
    sent.transport = "shared_memory_fd";
    sent.size = sizeof(payload) - 1;
    sent.complete = true;
    sent.file_descriptor = payload_fd;
    REQUIRE(send_worker_event(sockets[0], sent));
    ::close(payload_fd);

    WorkerProtocolReader reader;
    std::optional<WorkerEvent> received = receive_worker_event(sockets[1], reader);
    REQUIRE(received.has_value());
    CHECK(received->type == WorkerEventType::Artifact);
    CHECK(received->job_id == "protocol-fd-job");
    CHECK(received->kind == "preview");
    CHECK(received->transport == "shared_memory_fd");
    CHECK(received->size == sizeof(payload) - 1);
    CHECK(received->complete);
    REQUIRE(received->file_descriptor >= 0);

    std::array<char, sizeof(payload)> buffer {};
    REQUIRE(::read(received->file_descriptor, buffer.data(), sizeof(payload) - 1) == static_cast<ssize_t>(sizeof(payload) - 1));
    CHECK(std::string(buffer.data(), sizeof(payload) - 1) == "fd-event");

    ::close(received->file_descriptor);
    ::close(sockets[0]);
    ::close(sockets[1]);
}
#endif
