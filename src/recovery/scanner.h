#pragma once
// Background NTFS scan task management (volume + raw physical-disk modes).

#include <atomic>
#include <cstdint>
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

// A surviving $MFT region located on a physical disk.
struct RawMftCandidate {
    uint64_t offset = 0;        // absolute byte offset of $MFT record 0
    uint64_t record_count = 0;  // contiguous records found
};

struct ScanTask {
    std::string id;
    std::string drive;   // e.g. "D:" or "\\\\.\\PhysicalDrive1"
    std::string mode;    // "quick" | "raw"
    int  disk_number = -1;      // >=0 for raw physical-disk scans
    bool raw = false;

    // Physical-disk identity captured at scan time, used to verify — by serial,
    // falling back to (model + size) — that the SAME disk is still attached
    // before recovery. A reused \\.\PhysicalDriveN number must never cause files
    // to be recovered onto/from a *different* disk that now occupies that slot.
    std::string serial;         // physical disk serial ("" if unknown)
    std::string model;          // physical disk model / ProductId ("" if unknown)
    uint64_t disk_size = 0;     // physical disk byte size (0 if unknown)

    uint64_t mft_offset = 0;       // chosen $MFT record-0 byte offset (raw mode)
    uint64_t mft_offset_hint = 0;  // caller-supplied hint (0 = auto-detect)

    std::atomic<int>      progress{0};
    std::atomic<uint64_t> total_files{0};
    std::atomic<uint64_t> deleted_files{0};
    std::atomic<uint64_t> directories{0};

    // Estimated seconds remaining (updated by the worker's progress emits, read
    // by the HTTP layer). -1 = no estimate yet.
    std::atomic<int64_t> eta_seconds{-1};
    EtaEstimator eta;  // worker-thread-only progress-rate state

    // Byte-based ETA source. The scan progress percentage is phase-based and too
    // coarse (the whole-disk $MFT search advances one point per ~1/8 of the
    // disk), which starves the estimator's rate. When total_bytes > 0 the worker
    // feeds a continuous (scanned_bytes / total_bytes) percentage instead.
    std::atomic<uint64_t> scanned_bytes{0};
    std::atomic<uint64_t> total_bytes{0}; // 0 = fall back to the progress %

    std::atomic<bool> pause_requested{false};
    std::atomic<bool> stop_requested{false};

    std::mutex  mtx;
    std::string status;   // "running" | "paused" | "completed" | "failed" | "stopped"
    std::string error;
    std::string current_path; // name of the file/dir currently being parsed (guarded by mtx)

    // Populated on completion (guarded by mtx).
    FileNodePtr root;
    std::vector<FileNodePtr> nodes;
    std::map<uint64_t, FileNodePtr> by_id;

    // Raw-scan metadata (guarded by mtx).
    std::vector<RawMftCandidate> candidates;
    uint64_t cluster_size = 0;      // detected cluster size in bytes (raw mode)
    uint64_t volume_start = 0;      // detected volume start byte offset (raw mode)
    uint64_t volume_size = 0;       // detected volume byte size (0 = unknown)
    uint64_t mft_records_total = 0; // total $MFT records (from record 0 $DATA)

    // Filesystem family this volume was parsed as (NTFS / FAT12 / FAT16 /
    // FAT32 / exFAT). FAT volumes set this; NTFS volumes default to NTFS.
    FsType fs_type = FsType::NTFS;

    // True when this task was reconstructed from a saved scan file on startup
    // (no re-scan needed); its id is deterministic ("disk<N>" or "disk<N>_v<K>").
    bool persisted = false;

    // Deterministic persistence file stem ("scan_disk1_v0"); when empty, the
    // legacy name "scan_disk<disk_number>" is used. Multi-volume scans set this.
    std::string save_name;

    // True for the coordinator task that merely discovers volumes and spawns
    // per-volume scans; it has no result tree and must not be listed in
    // /api/scans or persisted.
    bool meta = false;

    // Whole-disk scan mode. true = deep: always run the full $MFT search so
    // lost/reformatted NTFS volumes hiding under a FAT-only MBR are found.
    // false = quick: skip the $MFT search once the disk is (super)FAT/exFAT-only.
    bool deep = false;

    std::string status_snapshot() const {
        // status is only mutated under mtx; read atomically enough for UI.
        return status;
    }
};

// One scan "session": a single invocation of "scan the whole disk", keyed by
// the physical disk's serial number so re-scanning the same disk replaces the
// previous record (only the last scan per serial is kept). `partitions` are the
// deterministic per-volume task ids ("disk<N>_v<K>").
struct ScanSession {
    std::string key;          // dedup key: serial, or "disk<N>" when serial unknown
    std::string serial;       // physical disk serial ("" if unknown)
    std::string model;        // physical disk model / ProductId ("" if unknown)
    uint64_t size = 0;        // physical disk byte size (0 if unknown)
    int  disk_number = -1;
    int64_t scanned_at = 0;   // unix seconds
    std::vector<std::string> partitions; // task ids "disk<N>_v<K>"
};

class Scanner {
public:
    Scanner() = default;
    ~Scanner();

    // Kick off a background volume scan; returns the new task id.
    std::string start_scan(const std::string& drive, const std::string& mode);

    // Raw physical-disk scan: locate a surviving $MFT and rebuild the tree
    // from its metadata only (no file data is read or written).
    // `disk_number` indexes \\.\PhysicalDriveN. `mft_offset_hint`, when
    // non-zero, targets a specific $MFT record-0 byte offset instead of
    // auto-detecting one. `save_name` overrides the persistence filename stem.
    std::string start_raw_scan(int disk_number, uint64_t mft_offset_hint = 0,
                               const std::string& save_name = "");

    // Scan EVERY surviving NTFS volume on a physical disk. Discovers all
    // self-consistent $MFT record-0s across the whole disk, then spawns one
    // scan task per volume (each persisted separately as "scan_<token>_v<K>").
    // `deep` controls whether the whole-disk $MFT search always runs (true) or
    // is skipped once the disk is (super)FAT/exFAT-only (false, the fast path).
    // Returns the coordinator task id; poll it for aggregate progress.
    std::string start_raw_scan_all(int disk_number, bool deep = false);

    std::shared_ptr<ScanTask> get_task(const std::string& id);

    // Load previously-saved raw-scan results from `data_dir` (files named
    // "scan_disk<N>.json") so a completed scan can be browsed and recovered
    // from across restarts without re-scanning. Also remembers `data_dir` for
    // saving future raw scans.
    void load_saved(const std::string& data_dir);

    // Snapshot of all known tasks (live + loaded), for /api/scans listing.
    std::vector<std::shared_ptr<ScanTask>> list_tasks();

    // Read-only diagnostic: scan `[start, end)` (byte offsets) of `disk_number`
    // for every surviving $MFT record-0 and report full per-candidate detail
    // (offset, contiguous records, total records, cluster size, volume start).
    // `end == 0` means "whole disk". Returns a JSON array string.
    std::string debug_find_mft(int disk_number, uint64_t start, uint64_t end);

    // Register a callback invoked (from worker threads) with a JSON scan-progress
    // snapshot. Same contract as Restorer::set_event_callback: it must be cheap
    // and thread-safe.
    void set_event_callback(std::function<void(const std::string&)> cb);

    // Pause / resume / stop an in-flight scan. Return false if the task is
    // unknown or already finished (mirrors Restorer).
    bool pause(const std::string& id);
    bool resume(const std::string& id);
    bool stop(const std::string& id);

    // Request stop on every live task so worker threads exit promptly and the
    // destructor's join returns quickly (graceful shutdown).
    void shutdown();

    // Scan-session model (one record per scanned physical disk, deduped by
    // serial, persisted to data/scans.json).
    std::vector<ScanSession> list_sessions();
    bool delete_scan(const std::string& key);  // drop session + its files/tasks

private:
    void scan_worker(std::shared_ptr<ScanTask> task);
    void raw_scan_worker(std::shared_ptr<ScanTask> task);
    void raw_scan_all_worker(std::shared_ptr<ScanTask> meta);

    // Parse a FAT12/16/32 or exFAT volume whose start is `task->volume_start`;
    // `task->fs_type` selects the family. Fills the result tree + counters and
    // persists it like the NTFS raw_scan_worker.
    void fat_scan_worker(std::shared_ptr<ScanTask> task);

    // Build + broadcast a scan progress snapshot (throttling is the caller's job).
    void emit_scan_progress(const std::shared_ptr<ScanTask>& task, const char* type);

    void record_session(const std::string& serial, const std::string& model,
                        uint64_t size, int disk,
                        const std::vector<std::string>& partitions);
    void load_sessions();
    void save_sessions();

    std::mutex mutex_;
    std::map<std::string, std::shared_ptr<ScanTask>> tasks_;
    std::atomic<uint64_t> counter_{0};
    std::mutex threads_mutex_;   // guards threads_ (a coordinator thread also appends)
    std::vector<std::thread> threads_;
    std::string data_dir_; // where completed raw scans are persisted
    std::function<void(const std::string&)> event_cb_;

    std::mutex sessions_mutex_;
    std::vector<ScanSession> sessions_;  // newest first
};

} // namespace recovery
