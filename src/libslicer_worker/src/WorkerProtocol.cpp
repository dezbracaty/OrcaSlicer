#include "libslicer_worker/WorkerProtocol.hpp"

#include <nlohmann/json.hpp>

#include <sstream>

namespace libslicer::worker {
namespace {

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

} // namespace

std::string event_to_json_line(const WorkerEvent& event)
{
    nlohmann::json json;
    json["type"] = type_to_string(event.type);
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
        return event;
    } catch (...) {
        return std::nullopt;
    }
}

} // namespace libslicer::worker
