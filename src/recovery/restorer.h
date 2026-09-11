#pragma once
// Background file/directory recovery job management.

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../include/types.h"
#include "../include/eta.h"

namespace recovery {

struct ScanTask;

struct RecoverJob {
    std::string id;
    std::string task_id;
    std::string output_dir;

    std::atomic<int>      progress{0};
    std::atomic<uint64_t> total_files{0};
    std::atomic<uint64_t> recovered_files{0};
    std::atomic<uint64_t> failed_files{0};
    std::atomic<uint64_t> total_bytes{0};
    std::atomic<uint64_t> recovered_bytes{0};

    // Estimated seconds remaining (updated by the worker's progress emits, read
    // by the HTTP layer). -1 = no estimate yet.
    std::atomic<int64_t> eta_seconds{-1};
    EtaEstimator eta;  // worker-thread-only progress-rate state

    // Control flags (checked by the worker between files and between clusters
    // of a large file, so pause/stop take effect promptly without corrupting
    // the file currently being written).
    std::atomic<bool> pause_requested{false};
    std::atomic<bool> stop_requested{false};

    std::mutex  mtx;
    std::string status;  // "running" | "paused" | "completed" | "failed" | "stopped"
    std::string error;

    // Progress detail for the file currently being written (guarded by mtx).
    std::string current_file;   // root-relative path
    uint64_t    current_bytes = 0; // bytes written so far
    uint64_t    current_size  = 0; // total bytes of the file
};

class Restorer {
public:
    Restorer() = default;
    ~Restorer();

    // Schedule a recovery of `paths` (root-relative, e.g. "/Docs/a.pdf") from a
    // completed scan task into `output_dir`. Returns the new job id.
    std::string start_recover(std::shared_ptr<ScanTask> task,
                              const std::vector<std::string>& paths,
                              const std::string& output_dir);

    std::shared_ptr<RecoverJob> get_job(const std::string& id);

    // Snapshot of all jobs (live + finished), for /api/active listing.
    std::vector<std::shared_ptr<RecoverJob>> list_jobs();

    // Control an in-flight job. Return false if the job is unknown or already
    // finished. `stop` also clears a pending pause so the worker can exit.
    bool pause(const std::string& id);
    bool resume(const std::string& id);
    bool stop(const std::string& id);

    // Register a callback invoked (from the worker thread) with a JSON progress
    // snapshot. The HTTP/WS layer forwards these to connected clients. The
    // callback must be cheap and thread-safe (it is).
    void set_event_callback(std::function<void(const std::string&)> cb);

    // Request stop on every in-flight job so workers exit promptly during a
    // graceful shutdown (mirrors Scanner::shutdown).
    void shutdown() {
        std::vector<std::shared_ptr<RecoverJob>> all;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (const auto& kv : jobs_) all.push_back(kv.second);
        }
        for (auto& j : all) {
            j->stop_requested.store(true);
            j->pause_requested.store(false);
        }
    }

private:
    void recover_worker(std::shared_ptr<RecoverJob> job,
                        std::shared_ptr<ScanTask> task,
                        std::vector<std::string> paths);

    std::mutex mutex_;
    std::map<std::string, std::shared_ptr<RecoverJob>> jobs_;
    std::atomic<uint64_t> counter_{0};
    std::vector<std::thread> threads_;
    std::function<void(const std::string&)> event_cb_;
};

} // namespace recovery
