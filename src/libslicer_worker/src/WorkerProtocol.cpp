#include "libslicer_worker/WorkerProtocol.hpp"

#include <nlohmann/json.hpp>

#ifndef _WIN32
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <array>
#include <cstring>
#include <sstream>

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
#endif

std::string type_to_string(WorkerEventType type)
{
    switch (type) {
    case WorkerEventType::Hello: return "hello";
    case WorkerEventType::Listening: return "listening";
    case WorkerEventType::Accepted: return "accepted";
    case WorkerEventType::Progress: return "progress";
    case WorkerEventType::Artifact: return "artifact";
    case WorkerEventType::Warning: return "warning";
    case WorkerEventType::Error: return "error";
    case WorkerEventType::Result: return "result";
    case WorkerEventType::CancelAccepted: return "cancel_accepted";
    case WorkerEventType::Stopping: return "stopping";
    }
    return "progress";
}

WorkerEventType type_from_string(const std::string& type)
{
    if (type == "hello") return WorkerEventType::Hello;
    if (type == "listening") return WorkerEventType::Listening;
    if (type == "accepted") return WorkerEventType::Accepted;
    if (type == "progress") return WorkerEventType::Progress;
    if (type == "artifact") return WorkerEventType::Artifact;
    if (type == "warning") return WorkerEventType::Warning;
    if (type == "error") return WorkerEventType::Error;
    if (type == "result") return WorkerEventType::Result;
    if (type == "cancel_accepted") return WorkerEventType::CancelAccepted;
    if (type == "stopping") return WorkerEventType::Stopping;
    return WorkerEventType::Error;
}

std::optional<WorkerEvent> pop_buffered_event(WorkerProtocolReader& reader)
{
    const size_t pos = reader.buffer.find('\n');
    if (pos == std::string::npos)
        return std::nullopt;

    std::string line = reader.buffer.substr(0, pos);
    reader.buffer.erase(0, pos + 1);

    std::optional<WorkerEvent> event = event_from_json_line(line);
    if (!event.has_value())
        return std::nullopt;
    if (!reader.pending_file_descriptors.empty()) {
        event->file_descriptor = reader.pending_file_descriptors.front();
        reader.pending_file_descriptors.erase(reader.pending_file_descriptors.begin());
    }
    return event;
}

} // namespace

std::string event_to_json_line(const WorkerEvent& event)
{
    nlohmann::json json;
    json["type"] = type_to_string(event.type);
    if (event.type == WorkerEventType::Hello) {
        json["protocol"] = WORKER_PROTOCOL_VERSION;
        json["server"] = event.message.empty() ? "orcaslicer-worker" : event.message;
        json["version"] = "0.1.0";
        json["capabilities"] = nlohmann::json::array({ "slice", "cancel", "artifacts" });
    }
    if (!event.job_id.empty()) json["job_id"] = event.job_id;
    if (event.percent >= 0) json["percent"] = event.percent;
    if (!event.stage.empty()) json["stage"] = event.stage;
    if (!event.kind.empty()) json["kind"] = event.kind;
    if (!event.code.empty()) json["code"] = event.code;
    if (!event.message.empty()) json["message"] = event.message;
    if (!event.path.empty()) json["path"] = event.path.string();
    if (event.type == WorkerEventType::Result) json["success"] = event.success;
    if (event.type == WorkerEventType::Error) json["recoverable"] = event.recoverable;
    if (event.elapsed_ms >= 0) json["elapsed_ms"] = event.elapsed_ms;
    if (!event.phase.empty()) json["phase"] = event.phase;
    if (!event.transport.empty()) json["transport"] = event.transport;
    if (!event.shm_name.empty()) json["shm_name"] = event.shm_name;
    if (event.size > 0) json["size"] = event.size;
    if (!event.schema.empty()) json["schema"] = event.schema;
    if (!event.format.empty()) json["format"] = event.format;
    if (!event.section.empty()) json["section"] = event.section;
    if (event.offset >= 0) json["offset"] = event.offset;
    if (event.count >= 0) json["count"] = event.count;
    if (event.record_size >= 0) json["record_size"] = event.record_size;
    if (event.type == WorkerEventType::Artifact) json["complete"] = event.complete;

    std::ostringstream out;
    out << json.dump() << '\n';
    return out.str();
}

std::optional<WorkerEvent> event_from_json_line(const std::string& line)
{
    try {
        const nlohmann::json json = nlohmann::json::parse(line);
        WorkerEvent event;
        event.type = type_from_string(json.value("type", "error"));
        event.job_id = json.value("job_id", "");
        event.percent = json.value("percent", -1);
        event.stage = json.value("stage", "");
        event.kind = json.value("kind", "");
        event.code = json.value("code", "");
        event.message = json.value("message", "");
        if (json.contains("path"))
            event.path = json.at("path").get<std::string>();
        event.success = json.value("success", false);
        event.recoverable = json.value("recoverable", false);
        event.elapsed_ms = json.value("elapsed_ms", -1LL);
        event.phase = json.value("phase", "");
        event.transport = json.value("transport", "");
        event.shm_name = json.value("shm_name", "");
        event.size = json.value("size", 0ULL);
        event.schema = json.value("schema", "");
        event.format = json.value("format", "");
        event.section = json.value("section", "");
        event.offset = json.value("offset", -1LL);
        event.count = json.value("count", -1LL);
        event.record_size = json.value("record_size", -1LL);
        event.complete = json.value("complete", false);
        return event;
    } catch (...) {
        return std::nullopt;
    }
}

bool send_worker_event(int socket_fd, const WorkerEvent& event)
{
    const std::string line = event_to_json_line(event);
#ifdef _WIN32
    (void)socket_fd;
    return event.file_descriptor < 0 && !line.empty();
#else
    if (event.file_descriptor < 0)
        return send_all(socket_fd, line);

    iovec iov {};
    iov.iov_base = const_cast<char*>(line.data());
    iov.iov_len = line.size();

    std::array<char, CMSG_SPACE(sizeof(int))> control {};
    msghdr message {};
    message.msg_iov = &iov;
    message.msg_iovlen = 1;
    message.msg_control = control.data();
    message.msg_controllen = control.size();

    cmsghdr* cmsg = CMSG_FIRSTHDR(&message);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    std::memcpy(CMSG_DATA(cmsg), &event.file_descriptor, sizeof(int));
    message.msg_controllen = CMSG_SPACE(sizeof(int));

    const ssize_t sent = ::sendmsg(socket_fd, &message, socket_send_flags());
    return sent == static_cast<ssize_t>(line.size());
#endif
}

std::optional<WorkerEvent> receive_worker_event(int socket_fd, WorkerProtocolReader& reader)
{
    if (std::optional<WorkerEvent> buffered = pop_buffered_event(reader))
        return buffered;

#ifdef _WIN32
    (void)socket_fd;
    return std::nullopt;
#else
    for (;;) {
        std::array<char, 4096> chunk {};
        std::array<char, CMSG_SPACE(sizeof(int) * 4)> control {};
        iovec iov {};
        iov.iov_base = chunk.data();
        iov.iov_len = chunk.size();

        msghdr message {};
        message.msg_iov = &iov;
        message.msg_iovlen = 1;
        message.msg_control = control.data();
        message.msg_controllen = control.size();

        const ssize_t count = ::recvmsg(socket_fd, &message, 0);
        if (count <= 0)
            return std::nullopt;

        for (cmsghdr* cmsg = CMSG_FIRSTHDR(&message); cmsg != nullptr; cmsg = CMSG_NXTHDR(&message, cmsg)) {
            if (cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS)
                continue;
            const auto data_size = static_cast<std::size_t>(cmsg->cmsg_len - CMSG_LEN(0));
            const int* fds = reinterpret_cast<const int*>(CMSG_DATA(cmsg));
            const std::size_t fd_count = data_size / sizeof(int);
            for (std::size_t i = 0; i < fd_count; ++i)
                reader.pending_file_descriptors.push_back(fds[i]);
        }

        reader.buffer.append(chunk.data(), static_cast<size_t>(count));
        if (std::optional<WorkerEvent> event = pop_buffered_event(reader))
            return event;
    }
#endif
}

} // namespace libslicer::worker
