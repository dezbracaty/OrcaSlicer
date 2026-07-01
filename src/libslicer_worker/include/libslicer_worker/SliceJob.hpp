#pragma once

#include <atomic>
#include <filesystem>
#include <string>

namespace libslicer::worker {

struct SliceJob {
    std::string job_id;
    std::filesystem::path request_path;
};

class CancellationToken {
public:
    void cancel() { m_cancelled.store(true); }
    void reset() { m_cancelled.store(false); }
    bool cancelled() const { return m_cancelled.load(); }

private:
    std::atomic_bool m_cancelled { false };
};

} // namespace libslicer::worker

