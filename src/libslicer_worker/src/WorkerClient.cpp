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

#include <atomic>
#include <chrono>
#include <cstring>
#include <exception>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <utility>

namespace libslicer::worker {

struct WorkerClient::Impl {
    explicit Impl(EventCallback callback) : on_event(std::move(callback)) {}

    void set_last_error(std::string error)
    {
        std::lock_guard<std::mutex> lock(error_mutex);
        last_error = std::move(error);
    }

    std::string get_last_error() const
    {
        std::lock_guard<std::mutex> lock(error_mutex);
        return last_error;
    }

    EventCallback on_event;
    WorkerOptions options;
    std::string last_error;
    std::thread read_thread;
    std::mutex write_mutex;
    mutable std::mutex error_mutex;
    std::atomic_bool connected { false };
#ifndef _WIN32
    pid_t pid { -1 };
    int fd { -1 };
#endif
};

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

WorkerClient::WorkerClient(WorkerClient&& other) noexcept
    : m_impl(std::exchange(other.m_impl, nullptr))
{
}

WorkerClient& WorkerClient::operator=(WorkerClient&& other) noexcept
{
    if (this != &other) {
        kill();
        delete m_impl;
        m_impl = std::exchange(other.m_impl, nullptr);
    }
    return *this;
}

bool WorkerClient::start(const WorkerOptions& options)
{
    if (m_impl == nullptr)
        return false;
    m_impl->options = options;
#ifdef _WIN32
    m_impl->set_last_error("WorkerClient process start is not implemented on Windows yet");
    return false;
#else
    if (m_impl->pid > 0) {
        m_impl->set_last_error("worker process is already running");
        return false;
    }
    const pid_t pid = ::fork();
    if (pid < 0) {
        m_impl->set_last_error("fork failed");
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
    if (m_impl == nullptr)
        return false;
#ifdef _WIN32
    m_impl->set_last_error("WorkerClient socket connect is not implemented on Windows yet");
    return false;
#else
    const std::string socket_path = m_impl->options.socket_path.string();
    if (socket_path.empty()) {
        m_impl->set_last_error("socket path is empty");
        return false;
    }
    if (socket_path.size() >= sizeof(sockaddr_un::sun_path)) {
        m_impl->set_last_error("socket path is too long: " + socket_path);
        return false;
    }
    if (m_impl->fd >= 0) {
        m_impl->set_last_error("worker socket is already connected");
        return false;
    }

    const auto deadline = std::chrono::steady_clock::now() + m_impl->options.startup_timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) {
            m_impl->set_last_error("socket failed");
            return false;
        }
        disable_sigpipe(fd);

        sockaddr_un addr {};
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path) - 1);
        if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
            m_impl->fd = fd;
            m_impl->connected.store(true);
            Impl* impl = m_impl;
            m_impl->read_thread = std::thread([impl]() {
                std::string buffer;
                char chunk[4096];
                while (impl->connected.load()) {
                    const ssize_t count = ::recv(impl->fd, chunk, sizeof(chunk), 0);
                    if (count <= 0)
                        break;
                    buffer.append(chunk, static_cast<size_t>(count));
                    size_t pos = std::string::npos;
                    while ((pos = buffer.find('\n')) != std::string::npos) {
                        std::string line = buffer.substr(0, pos);
                        buffer.erase(0, pos + 1);
                        if (auto event = event_from_json_line(line); event && impl->on_event) {
                            try {
                                impl->on_event(*event);
                            } catch (const std::exception& e) {
                                impl->set_last_error(std::string("worker event callback failed: ") + e.what());
                            } catch (...) {
                                impl->set_last_error("worker event callback failed");
                            }
                        }
                    }
                }
            });
            if (!send_all(m_impl->fd, make_message({ { "type", "hello" }, { "protocol", WORKER_PROTOCOL_VERSION } }))) {
                m_impl->set_last_error("failed to send worker hello");
                kill();
                return false;
            }
            return true;
        }
        ::close(fd);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    m_impl->set_last_error("Timed out connecting to worker socket");
    return false;
#endif
}

bool WorkerClient::submit(const SliceJob& job)
{
    if (m_impl == nullptr)
        return false;
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
    if (m_impl == nullptr)
        return false;
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
    if (m_impl == nullptr)
        return false;
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
    if (m_impl == nullptr)
        return;
#ifndef _WIN32
    if (m_impl->fd >= 0) {
        m_impl->connected.store(false);
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
    if (m_impl == nullptr)
        return false;
#ifdef _WIN32
    return false;
#else
    return m_impl->pid > 0;
#endif
}

std::string WorkerClient::last_error() const
{
    if (m_impl == nullptr)
        return {};
    return m_impl->get_last_error();
}

} // namespace libslicer::worker
