#include <libslicer/v1/Context.hpp>
#include <libslicer/v1/Project.hpp>

#include "OrcaConfigAdapter.hpp"
#include "PresetInternal.hpp"
#include "RuntimeCoordinator.hpp"
#include "libslic3r/PresetBundle.hpp"

#include <nlohmann/json.hpp>
#include <openssl/sha.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <locale>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <psapi.h>
#include <windows.h>
#elif defined(__APPLE__)
#include <sys/resource.h>
#include <sys/sysctl.h>
#include <sys/utsname.h>
#include <unistd.h>
#else
#include <sys/resource.h>
#include <sys/utsname.h>
#include <unistd.h>
#endif

using namespace libslicer::v1;

namespace {

using Clock = std::chrono::steady_clock;
using Json = nlohmann::json;

struct Arguments {
    std::filesystem::path resources;
    std::filesystem::path data_root;
    std::filesystem::path fixture;
    std::filesystem::path result;
};

std::optional<Arguments> parse_arguments(int argc, char **argv)
{
    Arguments arguments;
    for (int index = 1; index < argc; index += 2) {
        if (index + 1 >= argc)
            return std::nullopt;
        const std::string option = argv[index];
        const std::filesystem::path value = argv[index + 1];
        if (option == "--resources")
            arguments.resources = value;
        else if (option == "--data-root")
            arguments.data_root = value;
        else if (option == "--fixture")
            arguments.fixture = value;
        else if (option == "--result")
            arguments.result = value;
        else
            return std::nullopt;
    }
    if (arguments.resources.empty() || arguments.data_root.empty() ||
        arguments.fixture.empty() || arguments.result.empty())
        return std::nullopt;
    return arguments;
}

std::int64_t milliseconds(Clock::duration duration)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        duration).count();
}

std::uint64_t nanoseconds_to_milliseconds(std::uint64_t nanoseconds)
{
    return nanoseconds / 1'000'000;
}

std::string hex_digest(const unsigned char *digest)
{
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (std::size_t index = 0; index < SHA256_DIGEST_LENGTH; ++index)
        output << std::setw(2) << static_cast<unsigned>(digest[index]);
    return output.str();
}

std::string file_sha256(const std::filesystem::path &path)
{
    SHA256_CTX context;
    SHA256_Init(&context);
    std::ifstream input(path, std::ios::binary);
    if (!input)
        throw std::runtime_error("Unable to read file for SHA-256: " +
                                 path.string());
    std::vector<char> buffer(1024 * 1024);
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize count = input.gcount();
        if (count > 0)
            SHA256_Update(&context, buffer.data(),
                          static_cast<std::size_t>(count));
    }
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256_Final(digest, &context);
    return hex_digest(digest);
}

struct TreeMetadata {
    std::string sha256;
    std::uint64_t bytes {0};
    std::size_t vendor_count {0};
};

TreeMetadata resource_tree_metadata(const std::filesystem::path &root)
{
    std::vector<std::filesystem::path> files;
    for (const auto &entry :
         std::filesystem::recursive_directory_iterator(root)) {
        if (entry.is_regular_file())
            files.push_back(entry.path());
    }
    std::sort(files.begin(), files.end(),
              [&root](const auto &lhs, const auto &rhs) {
                  return std::filesystem::relative(lhs, root).generic_string() <
                         std::filesystem::relative(rhs, root).generic_string();
              });

    SHA256_CTX context;
    SHA256_Init(&context);
    TreeMetadata metadata;
    std::vector<char> buffer(1024 * 1024);
    for (const auto &path : files) {
        const std::string relative =
            std::filesystem::relative(path, root).generic_string();
        SHA256_Update(&context, relative.data(), relative.size());
        const char separator = '\0';
        SHA256_Update(&context, &separator, 1);
        metadata.bytes += std::filesystem::file_size(path);
        if (path.parent_path() == root / "profiles" &&
            path.extension() == ".json")
            ++metadata.vendor_count;

        std::ifstream input(path, std::ios::binary);
        if (!input)
            throw std::runtime_error(
                "Unable to read resource for SHA-256: " + path.string());
        while (input) {
            input.read(buffer.data(),
                       static_cast<std::streamsize>(buffer.size()));
            const std::streamsize count = input.gcount();
            if (count > 0)
                SHA256_Update(&context, buffer.data(),
                              static_cast<std::size_t>(count));
        }
    }
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256_Final(digest, &context);
    metadata.sha256 = hex_digest(digest);
    return metadata;
}

std::uint64_t peak_rss_bytes()
{
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS counters {};
    if (!GetProcessMemoryInfo(GetCurrentProcess(), &counters,
                              sizeof(counters)))
        return 0;
    return static_cast<std::uint64_t>(counters.PeakWorkingSetSize);
#else
    rusage usage {};
    if (getrusage(RUSAGE_SELF, &usage) != 0)
        return 0;
#ifdef __APPLE__
    return static_cast<std::uint64_t>(usage.ru_maxrss);
#else
    return static_cast<std::uint64_t>(usage.ru_maxrss) * 1024;
#endif
#endif
}

std::string os_description()
{
#ifdef _WIN32
    return "Windows";
#else
    utsname value {};
    if (uname(&value) != 0)
        return "unknown";
    return std::string(value.sysname) + " " + value.release + " " +
           value.machine;
#endif
}

std::string machine_model()
{
#ifdef __APPLE__
    std::size_t size = 0;
    if (sysctlbyname("hw.model", nullptr, &size, nullptr, 0) != 0 ||
        size == 0)
        return "unknown";
    std::string value(size, '\0');
    if (sysctlbyname("hw.model", value.data(), &size, nullptr, 0) != 0)
        return "unknown";
    if (!value.empty() && value.back() == '\0')
        value.pop_back();
    return value;
#else
    return "unknown";
#endif
}

std::uint64_t physical_memory_bytes()
{
#ifdef __APPLE__
    std::uint64_t value = 0;
    std::size_t size = sizeof(value);
    return sysctlbyname("hw.memsize", &value, &size, nullptr, 0) == 0
        ? value : 0;
#elif defined(_WIN32)
    MEMORYSTATUSEX status {};
    status.dwLength = sizeof(status);
    return GlobalMemoryStatusEx(&status) ? status.ullTotalPhys : 0;
#else
    const long pages = sysconf(_SC_PHYS_PAGES);
    const long page_size = sysconf(_SC_PAGESIZE);
    return pages > 0 && page_size > 0
        ? static_cast<std::uint64_t>(pages) *
              static_cast<std::uint64_t>(page_size)
        : 0;
#endif
}

Json diagnostics_json(const std::vector<Diagnostic> &diagnostics)
{
    Json result = Json::array();
    for (const Diagnostic &diagnostic : diagnostics) {
        result.push_back({
            {"code", static_cast<int>(diagnostic.code)},
            {"severity", static_cast<int>(diagnostic.severity)},
            {"message", diagnostic.message},
            {"field", diagnostic.field}});
    }
    return result;
}

struct LoaderSegments {
    std::mutex mutex;
    std::map<std::string, Clock::time_point> vendor_starts;
    std::optional<Clock::time_point> orca_start;
    Clock::duration orca {};
    Clock::duration vendor_parse_cpu_wall_sum {};
    Clock::duration merge {};
    std::size_t orca_load_count {0};
    std::size_t vendor_load_count {0};
    std::size_t merge_count {0};
};

} // namespace

int main(int argc, char **argv)
{
    const auto parsed = parse_arguments(argc, argv);
    if (!parsed) {
        std::cerr << "Invalid cold gate arguments\n";
        return 2;
    }
    const Arguments arguments = *parsed;
    Json result;
    bool passed = false;
    std::vector<Diagnostic> diagnostics;

    try {
        std::error_code cleanup;
        std::filesystem::remove_all(arguments.data_root, cleanup);
        if (cleanup ||
            !std::filesystem::create_directories(arguments.data_root))
            throw std::runtime_error("Unable to create cold gate data root");
        if (!std::filesystem::is_regular_file(arguments.fixture))
            throw std::runtime_error("Cold gate fixture is missing");

        const TreeMetadata resources =
            resource_tree_metadata(arguments.resources);
        const std::uint64_t fixture_bytes =
            std::filesystem::file_size(arguments.fixture);
        const std::string fixture_hash =
            file_sha256(arguments.fixture);

        LoaderSegments loader;
        Slic3r::set_system_preset_load_test_observer(
            [&loader](Slic3r::SystemPresetLoadTestEvent event,
                      const std::string &vendor) {
                const auto now = Clock::now();
                std::lock_guard<std::mutex> lock(loader.mutex);
                switch (event) {
                case Slic3r::SystemPresetLoadTestEvent::orca_load_started:
                    loader.orca_start = now;
                    ++loader.orca_load_count;
                    break;
                case Slic3r::SystemPresetLoadTestEvent::orca_load_finished:
                    if (loader.orca_start)
                        loader.orca += now - *loader.orca_start;
                    loader.orca_start.reset();
                    break;
                case Slic3r::SystemPresetLoadTestEvent::vendor_load_started:
                    loader.vendor_starts[vendor] = now;
                    ++loader.vendor_load_count;
                    break;
                case Slic3r::SystemPresetLoadTestEvent::vendor_load_finished: {
                    const auto found = loader.vendor_starts.find(vendor);
                    if (found != loader.vendor_starts.end()) {
                        loader.vendor_parse_cpu_wall_sum += now - found->second;
                        loader.vendor_starts.erase(found);
                    }
                    break;
                }
                case Slic3r::SystemPresetLoadTestEvent::vendor_merge_started:
                    loader.vendor_starts["merge:" + vendor] = now;
                    ++loader.merge_count;
                    break;
                case Slic3r::SystemPresetLoadTestEvent::vendor_merge_finished: {
                    const auto found =
                        loader.vendor_starts.find("merge:" + vendor);
                    if (found != loader.vendor_starts.end()) {
                        loader.merge += now - found->second;
                        loader.vendor_starts.erase(found);
                    }
                    break;
                }
                }
            });

        using namespace libslicer::v1::detail;
        if (!reset_runtime_coordinator_for_testing())
            throw std::runtime_error(
                "Runtime coordinator was not clean at process start");
        reset_record_scheduler_test_control();
        set_record_scheduler_test_control({4, {}});
        reset_config_adapter_test_stats();

        ContextOptions options{
            arguments.resources,
            arguments.data_root / "data",
            arguments.data_root / "temporary",
            {},
            {256ull * 1024 * 1024,
             1024ull * 1024 * 1024,
             10'000'000,
             1024ull * 1024 * 1024,
             1024ull * 1024 * 1024,
             20'000'000,
             1024ull * 1024 * 1024}};

        const auto cold_started = Clock::now();
        auto cold = SdkContext::create(options);
        const auto cold_finished = Clock::now();
        if (!cold.has_value())
            diagnostics = cold.diagnostics();

        std::optional<SdkContext> context;
        if (cold.has_value())
            context.emplace(std::move(cold).value());

        Clock::duration reuse_duration {};
        if (context) {
            const auto reuse_started = Clock::now();
            auto reused = SdkContext::create(options);
            reuse_duration = Clock::now() - reuse_started;
            if (!reused.has_value())
                diagnostics = reused.diagnostics();
        }

        Clock::duration project_duration {};
        bool project_loaded = false;
        if (context && diagnostics.empty()) {
            const auto project_started = Clock::now();
            auto project = Project::load(*context, arguments.fixture);
            project_duration = Clock::now() - project_started;
            project_loaded = project.has_value();
            if (!project.has_value())
                diagnostics = project.diagnostics();
        }

        Slic3r::set_system_preset_load_test_observer({});
        const RuntimeCoordinatorTestStats runtime =
            runtime_coordinator_test_stats();
        const RecordSchedulerTestStats scheduler =
            record_scheduler_test_stats();
        const ConfigAdapterTestStats adapter =
            config_adapter_test_stats();
        const std::int64_t cold_ms =
            milliseconds(cold_finished - cold_started);
        const std::int64_t reuse_ms = milliseconds(reuse_duration);
        const std::int64_t project_ms = milliseconds(project_duration);
        const std::uint64_t rss = peak_rss_bytes();

        passed = context.has_value() && diagnostics.empty() &&
                 project_loaded &&
                 cold_ms <= 15000 &&
                 reuse_ms <= 100 &&
                 project_ms <= 10000 &&
                 runtime.loader_invocations == 1 &&
                 rss <= 2'200'000'000ull;

        result = {
            {"cold_ms", cold_ms},
            {"reuse_ms", reuse_ms},
            {"project_load_ms", project_ms},
            {"loader_count", runtime.loader_invocations},
            {"peak_rss_bytes", rss},
            {"pass", passed},
            {"diagnostics", diagnostics_json(diagnostics)},
            {"segments", {
                {"schema_ms",
                 nanoseconds_to_milliseconds(adapter.schema_build_ns)},
                {"system_loader_ms",
                 nanoseconds_to_milliseconds(scheduler.system_loader_ns)},
                {"orca_load_ms", milliseconds(loader.orca)},
                {"parallel_vendor_parse_sum_ms",
                 milliseconds(loader.vendor_parse_cpu_wall_sum)},
                {"deterministic_merge_ms", milliseconds(loader.merge)},
                {"record_freeze_ms",
                 nanoseconds_to_milliseconds(scheduler.freeze_ns)},
                {"default_conversion_ms",
                 nanoseconds_to_milliseconds(scheduler.default_batch_ns)},
                {"effective_conversion_ms",
                 nanoseconds_to_milliseconds(scheduler.effective_batch_ns)},
                {"candidate_build_ms",
                 nanoseconds_to_milliseconds(scheduler.candidate_batch_ns)},
                {"record_commit_ms",
                 nanoseconds_to_milliseconds(scheduler.commit_ns)},
                {"user_store_ms",
                 nanoseconds_to_milliseconds(scheduler.user_store_ns)},
                {"record_worker_batches",
                 scheduler.dependency_batches_joined}}},
            {"machine", {
                {"model", machine_model()},
                {"cpu_threads", std::thread::hardware_concurrency()},
                {"memory_bytes", physical_memory_bytes()},
                {"os", os_description()}}},
            {"build", {
#ifdef NDEBUG
                {"type", "Release"},
#else
                {"type", "Debug"},
#endif
                {"compiler", std::string(__VERSION__)},
                {"tbb_record_worker_limit", 4}}},
            {"resources", {
                {"sha256", resources.sha256},
                {"bytes", resources.bytes},
                {"vendor_count", resources.vendor_count}}},
            {"fixture", {
                {"sha256", fixture_hash},
                {"bytes", fixture_bytes}}},
            {"environment", {
                {"locale", std::locale("").name()},
                {"timezone", "process-default"},
                {"os_file_cache_policy", "uncontrolled-os-cache"}}}
        };
    } catch (const std::exception &error) {
        diagnostics.push_back({ErrorCode::internal, Severity::error,
                               error.what(), "/cold_gate"});
        result = {
            {"pass", false},
            {"diagnostics", diagnostics_json(diagnostics)}};
    }

    try {
        std::filesystem::create_directories(arguments.result.parent_path());
        std::ofstream output(arguments.result,
                             std::ios::binary | std::ios::trunc);
        if (!output)
            return 3;
        output << result.dump(2) << '\n';
        output.flush();
        if (!output)
            return 3;
    } catch (...) {
        return 3;
    }
    return passed ? 0 : 1;
}
