#include "libslicer_worker/WorkerClient.hpp"
#include "libslicer_worker/WorkerProtocol.hpp"

#include <nlohmann/json.hpp>

#ifndef _WIN32
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <chrono>
#include <cstring>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

namespace libslicer::worker {

struct WorkerClient::Impl {
    explicit Impl(EventCallback callback) : on_event(std::move(callback)) {}

    EventCallback on_event;
    WorkerOptions options;
    std::string last_error;
    std::thread read_thread;
    std::mutex write_mutex;
    bool connected { false };
#ifndef _WIN32
    pid_t pid { -1 };
    int fd { -1 };
#endif
};

namespace {

#ifndef _WIN32
bool send_all(int fd, const std::string& message)
{
    const char* data = message.data();
    size_t left = message.size();
    while (left > 0) {
        const ssize_t written = ::send(fd, data, left, 0);
        if (written <= 0)
            return false;
        data += written;
        left -= static_cast<size_t>(written);
    }
    return true;
}

std::string make_message(nlohmann::json json)
{
    return json.dump() + "\n";
}
#endif

} // namespace

WorkerClient::WorkerClient(EventCallback on_event)
    : m_impl(new Impl(std::move(on_event)))
{
}

WorkerClient::~WorkerClient()
{
    kill();
    delete m_impl;
}

bool WorkerClient::start(const WorkerOptions& options)
{
    m_impl->options = options;
#ifdef _WIN32
    m_impl->last_error = "WorkerClient process start is not implemented on Windows yet";
    return false;
#else
    const pid_t pid = ::fork();
    if (pid < 0) {
        m_impl->last_error = "fork failed";
        return false;
    }
    if (pid == 0) {
        if (!options.working_dir.empty())
            ::chdir(options.working_dir.string().c_str());
        ::execl(options.executable_path.string().c_str(),
                options.executable_path.filename().string().c_str(),
                "serve",
                "--socket",
                options.socket_path.string().c_str(),
                static_cast<char*>(nullptr));
        _exit(127);
    }
    m_impl->pid = pid;
    return true;
#endif
}

bool WorkerClient::connect()
{
#ifdef _WIN32
    m_impl->last_error = "WorkerClient socket connect is not implemented on Windows yet";
    return false;
#else
    const auto deadline = std::chrono::steady_clock::now() + m_impl->options.startup_timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) {
            m_impl->last_error = "socket failed";
            return false;
        }

        sockaddr_un addr {};
        addr.sun_family = AF_UNIX;
        const std::string socket_path = m_impl->options.socket_path.string();
        std::strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path) - 1);
        if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
            m_impl->fd = fd;
            m_impl->connected = true;
            m_impl->read_thread = std::thread([this]() {
                std::string buffer;
                char chunk[4096];
                while (m_impl->connected) {
                    const ssize_t count = ::recv(m_impl->fd, chunk, sizeof(chunk), 0);
                    if (count <= 0)
                        break;
                    buffer.append(chunk, static_cast<size_t>(count));
                    size_t pos = std::string::npos;
                    while ((pos = buffer.find('\n')) != std::string::npos) {
                        std::string line = buffer.substr(0, pos);
                        buffer.erase(0, pos + 1);
                        if (auto event = event_from_json_line(line); event && m_impl->on_event)
                            m_impl->on_event(*event);
                    }
                }
            });
            return send_all(m_impl->fd, make_message({ { "type", "hello" }, { "protocol", 1 } }));
        }
        ::close(fd);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    m_impl->last_error = "Timed out connecting to worker socket";
    return false;
#endif
}

bool WorkerClient::submit(const SliceJob& job)
{
#ifdef _WIN32
    return false;
#else
    if (m_impl->fd < 0)
        return false;
    std::lock_guard<std::mutex> lock(m_impl->write_mutex);
    return send_all(m_impl->fd, make_message({
        { "type", "start_job" },
        { "job_id", job.job_id },
        { "request_path", job.request_path.string() }
    }));
#endif
}

bool WorkerClient::cancel(const std::string& job_id)
{
#ifdef _WIN32
    return false;
#else
    if (m_impl->fd < 0)
        return false;
    std::lock_guard<std::mutex> lock(m_impl->write_mutex);
    return send_all(m_impl->fd, make_message({ { "type", "cancel" }, { "job_id", job_id } }));
#endif
}

bool WorkerClient::stop()
{
#ifdef _WIN32
    return false;
#else
    if (m_impl->fd < 0)
        return false;
    std::lock_guard<std::mutex> lock(m_impl->write_mutex);
    return send_all(m_impl->fd, make_message({ { "type", "stop" } }));
#endif
}

void WorkerClient::kill()
{
#ifndef _WIN32
    if (m_impl->fd >= 0) {
        m_impl->connected = false;
        ::shutdown(m_impl->fd, SHUT_RDWR);
        ::close(m_impl->fd);
        m_impl->fd = -1;
    }
    if (m_impl->read_thread.joinable())
        m_impl->read_thread.join();
    if (m_impl->pid > 0) {
        ::kill(m_impl->pid, SIGTERM);
        ::waitpid(m_impl->pid, nullptr, 0);
        m_impl->pid = -1;
    }
#endif
}

bool WorkerClient::running() const
{
#ifdef _WIN32
    return false;
#else
    return m_impl->pid > 0;
#endif
}

std::string WorkerClient::last_error() const
{
    return m_impl->last_error;
}

} // namespace libslicer::worker
