#include "libslicer_worker/WorkerProtocol.hpp"
#include "libslicer_worker/WorkerServer.hpp"

#include <nlohmann/json.hpp>

#ifndef _WIN32
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

#include <atomic>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

namespace libslicer::worker {
namespace {

#ifndef _WIN32
int socket_send_flags()
{
#ifdef MSG_NOSIGNAL
    return MSG_NOSIGNAL;
#else
    return 0;
#endif
}

void disable_sigpipe(int fd)
{
#ifdef SO_NOSIGPIPE
    int value = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &value, sizeof(value));
#else
    (void)fd;
#endif
}

bool send_all(int fd, const std::string& message)
{
    const char* data = message.data();
    size_t left = message.size();
    while (left > 0) {
        const ssize_t written = ::send(fd, data, left, socket_send_flags());
        if (written <= 0)
            return false;
        data += written;
        left -= static_cast<size_t>(written);
    }
    return true;
}

bool recv_line(int fd, std::string& buffer, std::string& line)
{
    for (;;) {
        const size_t pos = buffer.find('\n');
        if (pos != std::string::npos) {
            line = buffer.substr(0, pos);
            buffer.erase(0, pos + 1);
            return true;
        }

        char chunk[4096];
        const ssize_t count = ::recv(fd, chunk, sizeof(chunk), 0);
        if (count <= 0)
            return false;
        buffer.append(chunk, static_cast<size_t>(count));
    }
}

void send_event(int fd, const WorkerEvent& event)
{
    send_all(fd, event_to_json_line(event));
}
#endif

} // namespace

int run_worker_server(const ServerOptions& options)
{
#ifdef _WIN32
    std::cerr << "Socket worker mode is not implemented on Windows yet.\n";
    return 2;
#else
    const std::string socket_path = options.socket_path.string();
    if (socket_path.empty()) {
        std::cerr << "--socket is required\n";
        return 2;
    }
    if (socket_path.size() >= sizeof(sockaddr_un::sun_path)) {
        std::cerr << "Socket path is too long: " << socket_path << '\n';
        return 2;
    }

    ::unlink(socket_path.c_str());
    const int server_fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd < 0) {
        std::cerr << "Failed to create socket\n";
        return 70;
    }
    disable_sigpipe(server_fd);

    sockaddr_un addr {};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path) - 1);
    if (::bind(server_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        std::cerr << "Failed to bind socket: " << socket_path << '\n';
        ::close(server_fd);
        return 70;
    }
    if (::listen(server_fd, 1) != 0) {
        std::cerr << "Failed to listen on socket: " << socket_path << '\n';
        ::close(server_fd);
        return 70;
    }

    std::cout << event_to_json_line({ WorkerEventType::Listening, "", -1, "", "", "", "", options.socket_path });
    std::cout.flush();

    bool stop = false;
    while (!stop) {
        const int client_fd = ::accept(server_fd, nullptr, nullptr);
        if (client_fd < 0)
            continue;
        disable_sigpipe(client_fd);

        std::atomic_bool job_active { false };
        CancellationToken cancellation;
        std::thread job_thread;
        std::mutex send_mutex;
        std::string recv_buffer;
        std::string active_job_id;
        bool handshake_complete = false;

        auto emit_to_client = [&](const WorkerEvent& event) {
            std::lock_guard<std::mutex> lock(send_mutex);
            send_event(client_fd, event);
        };

        std::string line;
        while (recv_line(client_fd, recv_buffer, line)) {
            nlohmann::json message;
            try {
                message = nlohmann::json::parse(line);
            } catch (...) {
                emit_to_client({ WorkerEventType::Error, "", -1, "", "", "bad_protocol", "Invalid JSON message" });
                continue;
            }

            const std::string type = message.value("type", "");
            if (type == "hello") {
                const int protocol = message.value("protocol", -1);
                if (protocol != WORKER_PROTOCOL_VERSION) {
                    emit_to_client({ WorkerEventType::Error, "", -1, "", "", "unsupported_protocol_version",
                                     "Unsupported worker protocol version: " + std::to_string(protocol) });
                    continue;
                }
                handshake_complete = true;
                WorkerEvent hello;
                hello.type = WorkerEventType::Hello;
                hello.message = "orcaslicer-worker";
                emit_to_client(hello);
            } else if (type == "start_job") {
                if (!handshake_complete) {
                    emit_to_client({ WorkerEventType::Error, "", -1, "", "", "bad_protocol", "hello is required before start_job" });
                    continue;
                }
                if (job_thread.joinable() && !job_active.load())
                    job_thread.join();
                if (job_active.load()) {
                    emit_to_client({ WorkerEventType::Error, "", -1, "", "", "job_active", "A job is already active" });
                    continue;
                }
                const std::string request_path = message.value("request_path", "");
                active_job_id = message.value("job_id", "");
                if (request_path.empty()) {
                    emit_to_client({ WorkerEventType::Error, active_job_id, -1, "", "", "invalid_request", "request_path is required" });
                    continue;
                }

                cancellation.reset();
                job_active.store(true);
                emit_to_client({ WorkerEventType::Accepted, active_job_id });
                job_thread = std::thread([&, request_path]() {
                    run_slice_job_from_request(request_path, emit_to_client, cancellation);
                    job_active.store(false);
                });
            } else if (type == "cancel") {
                cancellation.cancel();
                emit_to_client({ WorkerEventType::CancelAccepted, message.value("job_id", active_job_id) });
            } else if (type == "stop") {
                const bool force = message.value("force", false);
                if (job_thread.joinable() && !job_active.load())
                    job_thread.join();
                if (job_active.load() && !force) {
                    emit_to_client({ WorkerEventType::Error, active_job_id, -1, "", "", "job_active", "Job is active" });
                    continue;
                }
                cancellation.cancel();
                emit_to_client({ WorkerEventType::Stopping });
                stop = true;
                break;
            } else {
                emit_to_client({ WorkerEventType::Error, "", -1, "", "", "bad_protocol", "Unknown message type: " + type });
            }
        }

        if (job_thread.joinable())
            job_thread.join();
        ::close(client_fd);
    }

    ::close(server_fd);
    ::unlink(socket_path.c_str());
    return 0;
#endif
}

} // namespace libslicer::worker
