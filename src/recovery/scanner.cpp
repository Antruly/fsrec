#include "scanner.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>

#include <windows.h>
#include <winioctl.h>

#include <nlohmann/json.hpp>

#include "../include/util.h"
#include "../ntfs/boot_parser.h"
#include "../ntfs/disk_reader.h"
#include "../ntfs/mft_parser.h"
#include "../ntfs/tree_builder.h"

namespace fs = std::filesystem;

namespace recovery {

namespace {

using json = nlohmann::json;

// ---------------------------------------------------------------------------
// Persistence: serialize/deserialize the full result tree (including the data
// descriptors captured during scan) so recovery never needs to re-read the MFT.
// ---------------------------------------------------------------------------
std::string hex_encode(const std::vector<uint8_t>& b) {
    static const char* digits = "0123456789abcdef";
    std::string s;
    s.reserve(b.size() * 2);
    for (uint8_t x : b) {
        s.push_back(digits[x >> 4]);
        s.push_back(digits[x & 0x0F]);
    }
    return s;
}

std::vector<uint8_t> hex_decode(const std::string& s) {
    std::vector<uint8_t> b;
    b.reserve(s.size() / 2);
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i + 1 < s.size(); i += 2) {
        int hi = nib(s[i]), lo = nib(s[i + 1]);
        if (hi < 0 || lo < 0) break;
        b.push_back(static_cast<uint8_t>((hi << 4) | lo));
    }
    return b;
}

json serialize_node(const FileNodePtr& node) {
    json j;
    j["name"] = utf16_to_utf8(node->name);
    j["id"] = node->mft_id;
    j["parent"] = node->parent_mft_id;
    j["dir"] = node->is_directory;
    j["deleted"] = node->is_deleted;
    j["size"] = node->size;
    j["mtime"] = node->modified_time;
    j["has_data"] = node->has_data;
    j["resident"] = node->resident;
    if (node->resident) j["rdata"] = hex_encode(node->resident_data);
    if (!node->data_runs.empty()) {
        json runs = json::array();
        for (const auto& r : node->data_runs) {
            json rj;
            rj["lcn"] = r.lcn;
            rj["len"] = r.length;
            rj["sparse"] = r.sparse;
            runs.push_back(rj);
        }
        j["runs"] = runs;
    }
    j["recoverable"] = node->recoverable;
    if (node->is_directory) {
        j["children"] = json::array();
        for (const auto& c : node->children) {
            j["children"].push_back(serialize_node(c));
        }
    }
    return j;
}

FileNodePtr deserialize_node(const json& j) {
    auto node = std::make_shared<FileNode>();
    node->name = utf8_to_utf16(j.value("name", ""));
    node->mft_id = j.value("id", static_cast<uint64_t>(0));
    node->parent_mft_id = j.value("parent", static_cast<uint64_t>(0));
    node->is_directory = j.value("dir", false);
    node->is_deleted = j.value("deleted", false);
    node->size = j.value("size", static_cast<uint64_t>(0));
    node->modified_time = j.value("mtime", static_cast<int64_t>(0));
    node->has_data = j.value("has_data", false);
    node->resident = j.value("resident", false);
    if (node->resident && j.contains("rdata")) {
        node->resident_data = hex_decode(j["rdata"].get<std::string>());
    }
    if (j.contains("runs") && j["runs"].is_array()) {
        for (const auto& rj : j["runs"]) {
            DataRun r;
            r.lcn = rj.value("lcn", static_cast<int64_t>(0));
            r.length = rj.value("len", static_cast<uint64_t>(0));
            r.sparse = rj.value("sparse", false);
            node->data_runs.push_back(r);
        }
    }
    node->recoverable = j.value("recoverable", false);
    if (node->is_directory && j.contains("children") && j["children"].is_array()) {
        for (const auto& cj : j["children"]) {
            node->children.push_back(deserialize_node(cj));
        }
    }
    return node;
}

// Write a completed raw scan to data_dir/scan_disk<N>.json so it survives a
// restart. Returns false (without throwing) on any I/O error.
bool save_scan_file(const std::shared_ptr<ScanTask>& task, const std::string& data_dir) {
    if (task->disk_number < 0 || data_dir.empty()) return false;
    try {
        FileNodePtr root;
        uint64_t cluster_size = 0, volume_start = 0, mft_offset = 0, mft_records_total = 0;
        uint64_t total = 0, deleted = 0, dirs = 0;
        {
            std::lock_guard<std::mutex> lock(task->mtx);
            root = task->root;
            cluster_size = task->cluster_size;
            volume_start = task->volume_start;
            mft_offset = task->mft_offset;
            mft_records_total = task->mft_records_total;
            total = task->total_files.load();
            deleted = task->deleted_files.load();
            dirs = task->directories.load();
        }
        if (!root) return false;

        json j;
        j["version"] = 1;
        j["disk_number"] = task->disk_number;
        j["mft_offset"] = mft_offset;
        j["cluster_size"] = cluster_size;
        j["volume_start"] = volume_start;
        j["mft_records_total"] = mft_records_total;
        j["total_files"] = total;
        j["deleted_files"] = deleted;
        j["directories"] = dirs;
        j["root"] = serialize_node(root);

        fs::path dir = fs::u8path(data_dir);
        std::error_code ec;
        fs::create_directories(dir, ec);
        std::string stem = task->save_name.empty()
                               ? ("scan_disk" + std::to_string(task->disk_number))
                               : task->save_name;
        fs::path file = dir / (stem + ".json");
        // Write to a temp file then rename, so a crash mid-write never leaves a
        // truncated file that would fail to load next time.
        fs::path tmp = dir / (stem + ".json.tmp");
        {
            std::ofstream ofs(tmp, std::ios::binary | std::ios::trunc);
            if (!ofs.is_open()) return false;
            ofs << j.dump();
            ofs.close();
            if (!ofs.good()) return false;
        }
        fs::remove(file, ec);
        fs::rename(tmp, file, ec);
        return !ec;
    } catch (...) {
        return false;
    }
}



// Read a physical disk's serial number (STORAGE_DEVICE_DESCRIPTOR /
// SerialNumberOffset). Returns "" when unavailable (e.g. no access or no
// vendor-supplied serial), in which case callers key by "disk<N>" instead.
std::string read_disk_serial(int disk_number) {
    char path[64];
    snprintf(path, sizeof(path), "\\\\.\\PhysicalDrive%d", disk_number);
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return "";

    std::string serial;
    std::vector<uint8_t> buf(sizeof(STORAGE_DEVICE_DESCRIPTOR) + 512, 0);
    STORAGE_PROPERTY_QUERY spq{};
    spq.PropertyId = StorageDeviceProperty;
    spq.QueryType = PropertyStandardQuery;
    DWORD bytes = 0;
    if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &spq, sizeof(spq),
                        buf.data(), static_cast<DWORD>(buf.size()), &bytes, nullptr)) {
        auto* desc = reinterpret_cast<PSTORAGE_DEVICE_DESCRIPTOR>(buf.data());
        if (desc->SerialNumberOffset != 0) {
            const char* p = reinterpret_cast<const char*>(buf.data() + desc->SerialNumberOffset);
            serial.assign(p, strnlen(p, 512));
        }
    }
    CloseHandle(h);
    return serial;
}

// Sentinel returned by an offset provider to stop record iteration.
constexpr uint64_t kNoOffset = ~0ULL;

std::string make_id(const char* prefix, uint64_t n) {
    auto now = std::chrono::system_clock::now().time_since_epoch().count();
    char buf[64];
    snprintf(buf, sizeof(buf), "%s-%llu-%lld", prefix,
             static_cast<unsigned long long>(n),
             static_cast<long long>(now % 1000000));
    return std::string(buf);
}

inline uint16_t le16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}
inline uint32_t le32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

// True if `p` looks like the start of an MFT record: "FILE" magic + a sane
// Update Sequence Array offset (0x30) and count.
bool is_mft_record_signature(const uint8_t* p) {
    if (std::memcmp(p, "FILE", 4) != 0) return false;
    uint16_t usa_off = le16(p + 4);
    uint16_t usa_cnt = le16(p + 6);
    return usa_off == 0x30 && usa_cnt >= 1 && usa_cnt <= 0x20;
}

// ---------------------------------------------------------------------------
// Maps an MFT record number to its physical byte offset by walking the $MFT
// file's own data runs (handles a non-contiguous $MFT).
// ---------------------------------------------------------------------------
struct MftRecordLocator {
    uint64_t volume_start = 0;
    uint64_t cluster_size = 4096;
    uint64_t record_size = 1024;
    std::vector<DataRun> runs;

    bool offset(uint64_t record, uint64_t* out) const {
        uint64_t file_byte = record * record_size;
        uint64_t run_start = 0;
        for (const auto& r : runs) {
            uint64_t run_bytes = r.length * cluster_size;
            if (file_byte < run_start + run_bytes) {
                if (r.sparse) { *out = kNoOffset; return true; }
                uint64_t within = file_byte - run_start;
                uint64_t lcn = static_cast<uint64_t>(r.lcn) + within / cluster_size;
                *out = volume_start + lcn * cluster_size + (within % cluster_size);
                return true;
            }
            run_start += run_bytes;
        }
        return false; // beyond the $MFT file
    }
};

// Determine the NTFS cluster size that makes the $MFT data runs
// self-consistent. Returns 0 if none is found.
uint64_t detect_cluster_size(DiskReader& reader, uint64_t mft_off,
                             const std::vector<DataRun>& runs,
                             uint64_t total_records) {
    if (runs.empty() || runs[0].sparse || runs[0].lcn < 0 || total_records < 2)
        return 0;

    const uint64_t candidates[] = {512, 1024, 2048, 4096, 8192, 16384, 32768, 65536};
    std::vector<uint8_t> buf(1024);

    for (uint64_t cs : candidates) {
        uint64_t first_lcn = static_cast<uint64_t>(runs[0].lcn);
        if (first_lcn * cs > mft_off) continue; // would make volume start negative
        uint64_t volume_start = mft_off - first_lcn * cs;
        if ((volume_start & 511) != 0) continue; // not sector-aligned

        MftRecordLocator loc;
        loc.volume_start = volume_start;
        loc.cluster_size = cs;
        loc.runs = runs;

        // Validate records spread across the whole $MFT. A contiguous MFT makes
        // every cluster size look correct on the first few records, but only the
        // right size maps records that live in a later extent.
        bool ok = true;
        const uint64_t checks[] = {1, 2, 3,
                                   total_records / 4,
                                   total_records / 2,
                                   total_records * 3 / 4};
        for (uint64_t i : checks) {
            if (i == 0 || i >= total_records) continue;
            uint64_t off = 0;
            if (!loc.offset(i, &off) || off == kNoOffset) { ok = false; break; }
            size_t got = 0;
            if (!reader.read(off, buf.data(), 1024, &got) || got < 512) { ok = false; break; }
            if (std::memcmp(buf.data(), "FILE", 4) != 0) { ok = false; break; }
            uint32_t id = le32(buf.data() + 0x2C);
            if (id != 0 && id != i) { ok = false; break; }
        }
        if (ok) return cs;
    }
    return 0;
}

// Parse record 0 to estimate the total number of records from the $MFT file's
// own $DATA size. Falls back to `fallback` records when unavailable.
uint64_t estimate_total_records(MftParser& mft, uint32_t bytes_per_sector,
                                uint32_t record_size, uint64_t fallback) {
    std::vector<uint8_t> rec0;
    if (mft.read_record(0, rec0)) {
        MftParser::apply_usa_fixup(rec0, bytes_per_sector);
        FileNode n0;
        if (mft.parse_record(rec0, n0) && n0.size >= record_size) {
            uint64_t t = n0.size / record_size;
            if (t > 0) return t;
        }
    }
    return fallback;
}

// Iterate MFT records [0, total) and fill `nodes` plus the task counters.
// `offset_fn(record)` returns the physical byte offset, or kNoOffset to stop.
void parse_records_at(const std::shared_ptr<ScanTask>& task, DiskReader& reader,
                      uint64_t total_records, uint32_t bytes_per_sector,
                      const std::function<uint64_t(uint64_t)>& offset_fn,
                      std::vector<FileNodePtr>& nodes,
                      const std::function<void()>& on_progress = {}) {
    MftParser mft(reader);
    std::vector<uint8_t> buf;
    uint64_t consecutive_invalid = 0;
    std::string last_name; // UTF-8 name of the last record successfully parsed

    for (uint64_t i = 0; i < total_records; i++) {
        uint64_t off = offset_fn(i);
        if (off == kNoOffset) break;

        buf.assign(1024, 0);
        size_t got = 0;
        if (!reader.read(off, buf.data(), 1024, &got) || got < 512) {
            if (++consecutive_invalid > 4096) break;
            continue;
        }
        if (std::memcmp(buf.data(), "FILE", 4) != 0) {
            if (++consecutive_invalid > 4096) break;
            continue;
        }
        consecutive_invalid = 0;

        MftParser::apply_usa_fixup(buf, bytes_per_sector);

        FileNode node;
        if (mft.parse_record(buf, node)) {
            if (node.mft_id == 0 && i != 0) node.mft_id = i;
            last_name = utf16_to_utf8(node.name);
            auto p = std::make_shared<FileNode>(std::move(node));
            nodes.push_back(p);
            task->total_files.fetch_add(1, std::memory_order_relaxed);
            if (p->is_directory) task->directories.fetch_add(1, std::memory_order_relaxed);
            if (p->is_deleted) task->deleted_files.fetch_add(1, std::memory_order_relaxed);
        }

        if ((i & 0x3FF) == 0) { // update progress every 1024 records
            task->progress = static_cast<int>((i * 100) / total_records);
            {
                std::lock_guard<std::mutex> lock(task->mtx);
                task->current_path = last_name;
            }
            if (on_progress) on_progress(); // worker emits / honors pause & stop
            if (task->stop_requested.load()) break;
        }
    }
}

// Scan a physical disk (bytes [0, scan_end)) for surviving $MFT record-0
// regions. Returns them sorted by record count, largest first.
std::vector<RawMftCandidate> scan_mft_candidates(
    DiskReader& reader, uint64_t scan_end,
    const std::atomic<bool>* stop = nullptr,
    const std::function<void(uint64_t)>& on_progress = {}) {
    constexpr size_t kChunk = 4 * 1024 * 1024; // 4 MiB reads
    constexpr size_t kOverlap = 4096;          // > one record, for boundaries

    std::vector<uint64_t> record0_offsets;
    std::vector<uint8_t> buf(kChunk);
    MftParser mft(reader);

    uint64_t off = 0;
    while (off < scan_end) {
        if (stop && stop->load()) break;
        size_t want = static_cast<size_t>(std::min<uint64_t>(kChunk, scan_end - off));
        size_t got = 0;
        if (!reader.read(off, buf.data(), want, &got) || got < 8) break;
        size_t n = got;

        for (size_t i = 0; i + 8 <= n; i += 512) {
            if (!is_mft_record_signature(buf.data() + i)) continue;

            // Require the following record to also look like an MFT record so
            // random "FILE" strings inside file data are filtered out.
            if (i + 1024 + 8 <= n) {
                if (!is_mft_record_signature(buf.data() + i + 1024)) continue;
            } else {
                uint8_t next[8];
                size_t ng = 0;
                if (!reader.read(off + i + 1024, next, 8, &ng) || ng < 8) continue;
                if (!is_mft_record_signature(next)) continue;
            }

            // Parse and confirm it is $MFT (record 0).
            uint64_t abs = off + i;
            std::vector<uint8_t> rec(1024);
            size_t rg = 0;
            if (!reader.read(abs, rec.data(), 1024, &rg) || rg < 512) continue;
            MftParser::apply_usa_fixup(rec, 512);
            FileNode node;
            if (mft.parse_record(rec, node) && node.name == u"$MFT") {
                record0_offsets.push_back(abs);
            }
        }

        if (got < want) break; // EOF reached
        off += (n > kOverlap) ? (n - kOverlap) : n;
        if (on_progress) on_progress(off);
    }

    std::sort(record0_offsets.begin(), record0_offsets.end());
    record0_offsets.erase(std::unique(record0_offsets.begin(), record0_offsets.end()),
                          record0_offsets.end());

    std::vector<RawMftCandidate> out;
    std::vector<uint8_t> rec(1024);
    for (uint64_t base : record0_offsets) {
        if (stop && stop->load()) break;
        uint64_t count = 0;
        uint64_t consecutive_invalid = 0;
        for (uint64_t i = 0; ; i++) {
            if (stop && stop->load()) break;
            uint64_t rec_off = base + i * 1024;
            size_t rg = 0;
            if (!reader.read(rec_off, rec.data(), 1024, &rg) || rg < 8) {
                if (++consecutive_invalid > 64) break;
                continue;
            }
            if (!is_mft_record_signature(rec.data())) {
                if (++consecutive_invalid > 64) break;
                continue;
            }
            consecutive_invalid = 0;
            count++;
            if (i > 20'000'000ULL) break; // safety cap
        }
        if (count >= 4) {
            RawMftCandidate c;
            c.offset = base;
            c.record_count = count;
            out.push_back(c);
        }
    }

    std::sort(out.begin(), out.end(),
              [](const RawMftCandidate& a, const RawMftCandidate& b) {
                  return a.record_count > b.record_count;
              });
    return out;
}

} // namespace

Scanner::~Scanner() {
    std::vector<std::thread> to_join;
    {
        std::lock_guard<std::mutex> lock(threads_mutex_);
        to_join.swap(threads_);
    }
    for (auto& t : to_join) {
        if (t.joinable()) t.join();
    }
}

std::string Scanner::start_scan(const std::string& drive, const std::string& mode) {
    auto task = std::make_shared<ScanTask>();
    task->id = make_id("scan", counter_.fetch_add(1));
    task->drive = drive;
    task->mode = mode.empty() ? "quick" : mode;
    task->status = "running";
    task->progress = 0;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        tasks_[task->id] = task;
    }

    {
        std::lock_guard<std::mutex> lock(threads_mutex_);
        threads_.emplace_back(&Scanner::scan_worker, this, task);
    }
    return task->id;
}

std::string Scanner::start_raw_scan(int disk_number, uint64_t mft_offset_hint,
                                    const std::string& save_name) {
    auto task = std::make_shared<ScanTask>();
    task->id = make_id("rawscan", counter_.fetch_add(1));
    char path[64];
    snprintf(path, sizeof(path), "\\\\.\\PhysicalDrive%d", disk_number);
    task->drive = path;
    task->mode = "raw";
    task->disk_number = disk_number;
    task->raw = true;
    task->mft_offset_hint = mft_offset_hint;
    task->save_name = save_name;
    task->status = "running";
    task->progress = 0;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        tasks_[task->id] = task;
    }

    {
        std::lock_guard<std::mutex> lock(threads_mutex_);
        threads_.emplace_back(&Scanner::raw_scan_worker, this, task);
    }
    return task->id;
}

std::string Scanner::start_raw_scan_all(int disk_number) {
    auto meta = std::make_shared<ScanTask>();
    meta->id = make_id("scanall", counter_.fetch_add(1));
    char path[64];
    snprintf(path, sizeof(path), "\\\\.\\PhysicalDrive%d", disk_number);
    meta->drive = path;
    meta->mode = "raw";
    meta->disk_number = disk_number;
    meta->raw = true;
    meta->meta = true; // no result tree; aggregates progress of per-volume tasks
    meta->status = "running";
    meta->progress = 0;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        tasks_[meta->id] = meta;
    }

    {
        std::lock_guard<std::mutex> lock(threads_mutex_);
        threads_.emplace_back(&Scanner::raw_scan_all_worker, this, meta);
    }
    return meta->id;
}

std::shared_ptr<ScanTask> Scanner::get_task(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = tasks_.find(id);
    if (it == tasks_.end()) return nullptr;
    return it->second;
}

void Scanner::load_saved(const std::string& data_dir) {
    data_dir_ = data_dir;
    if (data_dir_.empty()) return;

    // Restore the scan-session index (data/scans.json) in addition to the
    // per-partition result trees loaded below.
    load_sessions();

    std::error_code ec;
    fs::path dir = fs::u8path(data_dir_);
    if (!fs::is_directory(dir, ec)) return;

    // Load every "scan_disk<N>.json" and "scan_disk<N>_v<K>.json" file. The
    // filename stem doubles as the deterministic task id ("disk<N>" /
    // "disk<N>_v<K>"), so a saved scan is addressable across restarts.
    std::vector<fs::path> files;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        const auto& p = it->path();
        std::string stem = p.stem().string(); // "scan_disk1" or "scan_disk1_v0"
        if (p.extension() != ".json") continue;
        if (stem.rfind("scan_disk", 0) != 0) continue;
        files.push_back(p);
    }
    std::sort(files.begin(), files.end());

    for (const auto& file : files) {
        std::string stem = file.stem().string();
        std::string id = stem; // "scan_disk1_v0" → "disk1_v0"
        id.erase(0, strlen("scan_")); // → "disk1_v0" / "disk1"

        try {
            std::ifstream ifs(file, std::ios::binary);
            json j;
            ifs >> j;
            if (!j.is_object() || !j.contains("root")) continue;

            int disk = j.value("disk_number", -1);
            if (disk < 0) continue;

            auto task = std::make_shared<ScanTask>();
            task->id = id;
            task->save_name = stem;
            char path[64];
            snprintf(path, sizeof(path), "\\\\.\\PhysicalDrive%d", disk);
            task->drive = path;
            task->mode = "raw";
            task->raw = true;
            task->disk_number = disk;
            task->status = "completed";
            task->progress = 100;
            task->persisted = true;

            task->mft_offset = j.value("mft_offset", static_cast<uint64_t>(0));
            task->cluster_size = j.value("cluster_size", static_cast<uint64_t>(0));
            task->volume_start = j.value("volume_start", static_cast<uint64_t>(0));
            task->mft_records_total = j.value("mft_records_total", static_cast<uint64_t>(0));
            task->total_files.store(j.value("total_files", static_cast<uint64_t>(0)));
            task->deleted_files.store(j.value("deleted_files", static_cast<uint64_t>(0)));
            task->directories.store(j.value("directories", static_cast<uint64_t>(0)));

            task->root = deserialize_node(j["root"]);

            {
                std::lock_guard<std::mutex> lock(mutex_);
                tasks_[task->id] = task;
            }
        } catch (...) {
            // Corrupt or incompatible file: skip it rather than crash startup.
            continue;
        }
    }
}

std::vector<std::shared_ptr<ScanTask>> Scanner::list_tasks() {
    std::vector<std::shared_ptr<ScanTask>> out;
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& kv : tasks_) out.push_back(kv.second);
    return out;
}

// ---------------------------------------------------------------------------
// Scan-session index (data/scans.json): one record per scanned physical disk,
// keyed by serial so re-scanning the same disk replaces the previous record.
// ---------------------------------------------------------------------------

std::vector<ScanSession> Scanner::list_sessions() {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    return sessions_;
}

void Scanner::record_session(const std::string& serial, int disk,
                             const std::vector<std::string>& partitions) {
    std::string key = serial.empty() ? ("disk" + std::to_string(disk)) : serial;

    ScanSession s;
    s.key = key;
    s.serial = serial;
    s.disk_number = disk;
    s.scanned_at = static_cast<int64_t>(
        std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()));
    s.partitions = partitions;

    {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        // Dedup by key: drop any prior session for this serial, then prepend so
        // the newest scan appears first.
        sessions_.erase(std::remove_if(sessions_.begin(), sessions_.end(),
                                       [&](const ScanSession& x) { return x.key == key; }),
                        sessions_.end());
        sessions_.insert(sessions_.begin(), std::move(s));
    }
    save_sessions();
}

bool Scanner::delete_scan(const std::string& key) {
    ScanSession found;
    bool have = false;
    {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        for (auto it = sessions_.begin(); it != sessions_.end(); ++it) {
            if (it->key == key) {
                found = *it;
                sessions_.erase(it);
                have = true;
                break;
            }
        }
    }
    if (!have) return false;

    // Drop the per-partition result files and in-memory tasks.
    for (const auto& id : found.partitions) {
        if (!data_dir_.empty()) {
            std::error_code ec;
            fs::remove(fs::u8path(data_dir_) / ("scan_" + id + ".json"), ec);
        }
        std::lock_guard<std::mutex> lock(mutex_);
        tasks_.erase(id);
    }

    save_sessions();
    return true;
}

void Scanner::save_sessions() {
    if (data_dir_.empty()) return;
    try {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        json arr = json::array();
        for (const auto& s : sessions_) {
            json j;
            j["key"] = s.key;
            j["serial"] = s.serial;
            j["disk_number"] = s.disk_number;
            j["scanned_at"] = s.scanned_at;
            j["partitions"] = s.partitions;
            arr.push_back(std::move(j));
        }

        fs::path dir = fs::u8path(data_dir_);
        std::error_code ec;
        fs::create_directories(dir, ec);
        fs::path file = dir / "scans.json";
        fs::path tmp = dir / "scans.json.tmp";
        {
            std::ofstream ofs(tmp, std::ios::binary | std::ios::trunc);
            if (!ofs.is_open()) return;
            ofs << arr.dump();
            ofs.close();
            if (!ofs.good()) return;
        }
        fs::remove(file, ec);
        fs::rename(tmp, file, ec);
    } catch (...) {
        // Non-fatal: the session index is advisory; per-partition trees remain.
    }
}

void Scanner::load_sessions() {
    if (data_dir_.empty()) return;
    std::error_code ec;
    fs::path file = fs::u8path(data_dir_) / "scans.json";
    if (!fs::exists(file, ec)) return;

    try {
        std::ifstream ifs(file, std::ios::binary);
        json arr;
        ifs >> arr;
        if (!arr.is_array()) return;

        std::lock_guard<std::mutex> lock(sessions_mutex_);
        sessions_.clear();
        for (const auto& j : arr) {
            ScanSession s;
            s.key = j.value("key", "");
            s.serial = j.value("serial", "");
            s.disk_number = j.value("disk_number", -1);
            s.scanned_at = j.value("scanned_at", static_cast<int64_t>(0));
            if (j.contains("partitions") && j["partitions"].is_array())
                for (const auto& p : j["partitions"])
                    s.partitions.push_back(p.get<std::string>());
            if (!s.key.empty()) sessions_.push_back(std::move(s));
        }
    } catch (...) {
        // Corrupt index: start empty rather than crash startup.
    }
}

std::string Scanner::debug_find_mft(int disk_number, uint64_t start, uint64_t end) {
    json out = json::array();
    DiskReader reader;
    std::string err;
    if (!reader.open_physical(disk_number, &err)) {
        json e; e["error"] = "open physical disk: " + err;
        return e.dump();
    }

    uint64_t phys = reader.physical_size();
    if (end == 0 || end > phys) end = phys;
    if (start >= end) start = 0;

    constexpr size_t kChunk = 8 * 1024 * 1024; // 8 MiB reads
    constexpr size_t kOverlap = 4096;          // > one record, for boundaries
    std::vector<uint8_t> buf(kChunk);
    MftParser mft(reader);
    std::vector<uint64_t> record0_offsets;

    uint64_t off = start;
    while (off < end) {
        size_t want = static_cast<size_t>(std::min<uint64_t>(kChunk, end - off));
        size_t got = 0;
        if (!reader.read(off, buf.data(), want, &got) || got < 8) break;
        size_t n = got;

        for (size_t i = 0; i + 8 <= n; i += 512) {
            if (std::memcmp(buf.data() + i, "FILE", 4) != 0) continue;
            // Require the following record to also look like an MFT record.
            if (i + 1024 + 8 <= n) {
                if (!is_mft_record_signature(buf.data() + i + 1024)) continue;
            } else {
                uint8_t next[8];
                size_t ng = 0;
                if (!reader.read(off + i + 1024, next, 8, &ng) || ng < 8) continue;
                if (!is_mft_record_signature(next)) continue;
            }
            uint64_t abs = off + i;
            std::vector<uint8_t> rec(1024);
            size_t rg = 0;
            if (!reader.read(abs, rec.data(), 1024, &rg) || rg < 512) continue;
            MftParser::apply_usa_fixup(rec, 512);
            FileNode node;
            if (mft.parse_record(rec, node) && node.name == u"$MFT") {
                record0_offsets.push_back(abs);
            }
        }

        if (got < want) break;
        off += (n > kOverlap) ? (n - kOverlap) : n;
    }

    std::sort(record0_offsets.begin(), record0_offsets.end());
    record0_offsets.erase(std::unique(record0_offsets.begin(), record0_offsets.end()),
                          record0_offsets.end());

    std::vector<uint8_t> rec(1024);
    for (uint64_t base : record0_offsets) {
        // Contiguous record count.
        uint64_t count = 0;
        for (uint64_t k = 0; ; k++) {
            uint8_t hdr[8];
            size_t hg = 0;
            if (!reader.read(base + k * 1024, hdr, 8, &hg) || hg < 8) break;
            if (!is_mft_record_signature(hdr)) break;
            count++;
            if (k > 20'000'000ULL) break;
        }

        std::fill(rec.begin(), rec.end(), 0);
        size_t rg = 0;
        if (!reader.read(base, rec.data(), 1024, &rg) || rg < 512) continue;
        MftParser::apply_usa_fixup(rec, 512);
        FileNode node;
        if (!mft.parse_record(rec, node) || node.name != u"$MFT") continue;

        uint64_t tr = (node.size >= 1024) ? (node.size / 1024) : 0;
        uint64_t cs = 0, vs = 0;
        if (tr >= 2 && !node.data_runs.empty())
            cs = detect_cluster_size(reader, base, node.data_runs, tr);
        if (cs != 0)
            vs = base - static_cast<uint64_t>(node.data_runs[0].lcn) * cs;

        json j;
        j["offset"] = base;
        j["contiguous_records"] = count;
        j["mft_size"] = node.size;
        j["total_records"] = tr;
        j["cluster_size"] = cs;
        j["volume_start"] = vs;
        j["num_runs"] = node.data_runs.size();
        j["first_lcn"] = node.data_runs.empty() ? 0 : node.data_runs[0].lcn;
        out.push_back(j);
    }

    return out.dump();
}

void Scanner::set_event_callback(std::function<void(const std::string&)> cb) {
    event_cb_ = std::move(cb);
}

bool Scanner::pause(const std::string& id) {
    auto task = get_task(id);
    if (!task) return false;
    {
        std::lock_guard<std::mutex> lock(task->mtx);
        if (task->status != "running" && task->status != "paused") return false;
    }
    task->pause_requested.store(true);
    return true;
}

bool Scanner::resume(const std::string& id) {
    auto task = get_task(id);
    if (!task) return false;
    task->pause_requested.store(false);
    return true;
}

bool Scanner::stop(const std::string& id) {
    auto task = get_task(id);
    if (!task) return false;
    task->stop_requested.store(true);
    task->pause_requested.store(false); // wake a paused worker so it can exit
    return true;
}

void Scanner::shutdown() {
    std::vector<std::shared_ptr<ScanTask>> all;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& kv : tasks_) all.push_back(kv.second);
    }
    for (auto& t : all) {
        t->stop_requested.store(true);
        t->pause_requested.store(false); // wake paused workers so they can exit
    }
}

// Build + broadcast a scan progress snapshot. `type` is "progress" during the
// scan, or "done"/"stopped"/"error"/"paused"/"resumed" at transitions. Throttling
// is the caller's job; this only serializes and forwards. Called from workers.
void Scanner::emit_scan_progress(const std::shared_ptr<ScanTask>& task,
                                 const char* type) {
    if (!event_cb_) return;
    json j;
    j["type"] = type;
    j["task_id"] = task->id;
    j["progress"] = task->progress.load();
    j["total_files"] = task->total_files.load();
    j["deleted_files"] = task->deleted_files.load();
    j["directories"] = task->directories.load();
    {
        std::lock_guard<std::mutex> lock(task->mtx);
        j["status"] = task->status;
        j["error"] = task->error;
        j["current_path"] = task->current_path;
    }
    event_cb_(j.dump());
}

void Scanner::scan_worker(std::shared_ptr<ScanTask> task) {
    auto last_emit = std::chrono::steady_clock::now() - std::chrono::seconds(1);
    auto emit = [&](bool force, const char* type = "progress") {
        auto now = std::chrono::steady_clock::now();
        if (!force && now - last_emit < std::chrono::milliseconds(50)) return;
        last_emit = now;
        emit_scan_progress(task, type);
    };

    auto finish = [&](const std::string& status, const std::string& err) {
        {
            std::lock_guard<std::mutex> lock(task->mtx);
            task->status = status;
            task->error = err;
            task->current_path.clear();
        }
        if (status == "completed") task->progress = 100;
        const char* t = (status == "completed") ? "done"
                        : (status == "stopped") ? "stopped"
                        : "error";
        emit(true, t);
    };

    DiskReader reader;
    std::string err;
    if (!reader.open(task->drive, &err)) {
        finish("failed", "open volume: " + err);
        return;
    }
    if (!BootParser::parse(reader, &err)) {
        finish("failed", "parse boot sector: " + err);
        return;
    }

    MftParser mft(reader);
    const NTFSBootInfo& boot = reader.boot();
    uint64_t total_records = estimate_total_records(mft, boot.bytes_per_sector,
                                                    boot.mft_record_size, 0);
    if (total_records == 0) total_records = 1 << 20; // fallback cap
    const uint64_t kMaxRecords = 50'000'000ULL;
    if (total_records > kMaxRecords) total_records = kMaxRecords;

    std::vector<FileNodePtr> nodes;
    nodes.reserve(static_cast<size_t>(total_records > 1'000'000 ? 1'000'000 : total_records));

    auto on_tick = [&]() {
        if (task->pause_requested.load()) {
            { std::lock_guard<std::mutex> lock(task->mtx); task->status = "paused"; }
            emit(true, "paused");
            while (task->pause_requested.load() && !task->stop_requested.load())
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (!task->stop_requested.load()) {
                { std::lock_guard<std::mutex> lock(task->mtx); task->status = "running"; }
                emit(true, "resumed");
            }
        } else {
            emit(false);
        }
    };

    parse_records_at(task, reader, total_records, boot.bytes_per_sector,
                     [&](uint64_t i) {
                         return boot.mft_lcn * boot.cluster_size() + i * boot.mft_record_size;
                     },
                     nodes, on_tick);
    if (task->stop_requested.load()) { finish("stopped", ""); return; }

    std::map<uint64_t, FileNodePtr> by_id;
    FileNodePtr root = TreeBuilder::build(nodes, by_id);

    {
        std::lock_guard<std::mutex> lock(task->mtx);
        task->root = root;
        task->nodes = std::move(nodes);
        task->by_id = std::move(by_id);
    }

    if (!root) {
        finish("failed", "root directory (MFT 5) not found");
        return;
    }

    finish("completed", "");
}

void Scanner::raw_scan_worker(std::shared_ptr<ScanTask> task) {
    auto last_emit = std::chrono::steady_clock::now() - std::chrono::seconds(1);
    auto emit = [&](bool force, const char* type = "progress") {
        auto now = std::chrono::steady_clock::now();
        if (!force && now - last_emit < std::chrono::milliseconds(50)) return;
        last_emit = now;
        emit_scan_progress(task, type);
    };

    auto finish = [&](const std::string& status, const std::string& err) {
        {
            std::lock_guard<std::mutex> lock(task->mtx);
            task->status = status;
            task->error = err;
            task->current_path.clear();
        }
        if (status == "completed") task->progress = 100;
        const char* t = (status == "completed") ? "done"
                        : (status == "stopped") ? "stopped"
                        : "error";
        emit(true, t);
    };

    DiskReader reader;
    std::string err;
    if (!reader.open_physical(task->disk_number, &err)) {
        finish("failed", "open physical disk: " + err);
        return;
    }

    // Default scan window: the first 16 GiB, where $MFT lives for any volume
    // whose partition begins in the first few GiB ($MFT itself typically sits
    // ~3 GiB into its volume). The full disk is scanned if smaller.
    constexpr uint64_t kDefaultScan = 16ULL * 1024 * 1024 * 1024;
    uint64_t scan_end = reader.physical_size();
    if (scan_end == 0 || scan_end > kDefaultScan) scan_end = kDefaultScan;

    std::vector<RawMftCandidate> cands;
    uint64_t mft_off = task->mft_offset_hint;
    if (mft_off == 0) {
        cands = scan_mft_candidates(reader, scan_end, &task->stop_requested);
        if (cands.empty()) {
            finish("failed", "no surviving $MFT found in the first 16 GiB");
            return;
        }
        mft_off = cands.front().offset;
    } else {
        // Validate the caller-supplied hint by reading and parsing its record.
        std::vector<uint8_t> rec(1024);
        size_t rg = 0;
        MftParser hint_parser(reader);
        bool ok = reader.read(mft_off, rec.data(), 1024, &rg) && rg >= 512;
        if (ok) {
            MftParser::apply_usa_fixup(rec, 512);
            FileNode n0;
            ok = hint_parser.parse_record(rec, n0) && n0.name == u"$MFT";
        }
        if (!ok) {
            finish("failed", "mft_offset hint is not a valid $MFT record 0");
            return;
        }
        RawMftCandidate c;
        c.offset = mft_off;
        cands.push_back(c);
    }

    // Select a candidate whose record-0 $DATA runs are self-consistent, i.e. its
    // extents map back to its own location. A stale record-0 copy can have more
    // contiguous records yet be inconsistent with its own data runs (its implied
    // volume start is negative), and following those runs would read the wrong
    // region. Prefer the first consistent candidate; otherwise fall back to the
    // largest one read contiguously.
    MftParser parser(reader);
    std::vector<uint8_t> rec0(1024);

    uint64_t total_records = 0;
    uint64_t chosen_off = 0;
    MftRecordLocator loc;
    bool use_runs = false;
    uint64_t cluster_size = 0;
    uint64_t volume_start = 0;

    for (const auto& cand : cands) {
        FileNode n0;
        size_t rg = 0;
        std::fill(rec0.begin(), rec0.end(), 0);
        if (!reader.read(cand.offset, rec0.data(), 1024, &rg) || rg < 512) continue;
        MftParser::apply_usa_fixup(rec0, 512);
        if (!parser.parse_record(rec0, n0) || n0.name != u"$MFT") continue;

        uint64_t tr = (n0.size >= 1024) ? (n0.size / 1024) : 0;

        uint64_t cs = 0;
        if (tr >= 2 && !n0.data_runs.empty()) {
            cs = detect_cluster_size(reader, cand.offset, n0.data_runs, tr);
        }
        if (cs != 0) {
            chosen_off = cand.offset;
            total_records = tr;
            cluster_size = cs;
            volume_start = cand.offset - static_cast<uint64_t>(n0.data_runs[0].lcn) * cs;
            loc.volume_start = volume_start;
            loc.cluster_size = cs;
            loc.runs = n0.data_runs;
            use_runs = true;
            break;
        }
        // Remember the first (largest) candidate as a contiguous fallback.
        if (chosen_off == 0) {
            chosen_off = cand.offset;
            total_records = tr;
        }
    }

    if (total_records == 0) {
        total_records = cands.empty() ? (1 << 20) : cands.front().record_count;
    }
    if (total_records == 0) total_records = 1 << 20;
    if (chosen_off == 0 && !cands.empty()) chosen_off = cands.front().offset;

    const uint64_t kMaxRecords = 50'000'000ULL;
    if (total_records > kMaxRecords) total_records = kMaxRecords;

    {
        std::lock_guard<std::mutex> lock(task->mtx);
        task->mft_offset = chosen_off;
        task->cluster_size = cluster_size;
        task->volume_start = volume_start;
        task->mft_records_total = total_records;
        task->candidates = std::move(cands);
    }

    std::vector<FileNodePtr> nodes;
    nodes.reserve(static_cast<size_t>(total_records > 1'000'000 ? 1'000'000 : total_records));

    auto on_tick = [&]() {
        if (task->pause_requested.load()) {
            { std::lock_guard<std::mutex> lock(task->mtx); task->status = "paused"; }
            emit(true, "paused");
            while (task->pause_requested.load() && !task->stop_requested.load())
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (!task->stop_requested.load()) {
                { std::lock_guard<std::mutex> lock(task->mtx); task->status = "running"; }
                emit(true, "resumed");
            }
        } else {
            emit(false);
        }
    };

    // Only metadata (names/parents/sizes) is parsed — no file data is read.
    if (use_runs) {
        MftRecordLocator loc_copy = loc;
        parse_records_at(task, reader, total_records, 512,
                         [loc_copy](uint64_t i) {
                             uint64_t off = 0;
                             if (!loc_copy.offset(i, &off)) return kNoOffset;
                             return off;
                         },
                         nodes, on_tick);
    } else {
        parse_records_at(task, reader, total_records, 512,
                         [chosen_off](uint64_t i) { return chosen_off + i * 1024; },
                         nodes, on_tick);
    }
    if (task->stop_requested.load()) { finish("stopped", ""); return; }

    std::map<uint64_t, FileNodePtr> by_id;
    FileNodePtr root = TreeBuilder::build(nodes, by_id);

    {
        std::lock_guard<std::mutex> lock(task->mtx);
        task->root = root;
        task->nodes = std::move(nodes);
        task->by_id = std::move(by_id);
    }

    if (!root) {
        finish("failed", "no directory tree (MFT 5) in the located $MFT");
        return;
    }

    finish("completed", "");

    // Persist the completed scan locally so it can be re-opened after a restart
    // without re-scanning the disk. Failure is non-fatal (recovery still works
    // from the in-memory tree for this process).
    save_scan_file(task, data_dir_);
}

void Scanner::raw_scan_all_worker(std::shared_ptr<ScanTask> meta) {
    auto last_emit = std::chrono::steady_clock::now() - std::chrono::seconds(1);
    auto emit = [&](bool force, const char* type = "progress") {
        auto now = std::chrono::steady_clock::now();
        if (!force && now - last_emit < std::chrono::milliseconds(50)) return;
        last_emit = now;
        emit_scan_progress(meta, type);
    };

    auto finish = [&](const std::string& status, const std::string& err) {
        {
            std::lock_guard<std::mutex> lock(meta->mtx);
            meta->status = status;
            meta->error = err;
            meta->current_path.clear();
        }
        if (status == "completed") meta->progress = 100;
        const char* t = (status == "completed") ? "done"
                        : (status == "stopped") ? "stopped"
                        : "error";
        emit(true, t);
    };

    const int disk = meta->disk_number;
    DiskReader reader;
    std::string err;
    if (!reader.open_physical(disk, &err)) {
        finish("failed", "open physical disk: " + err);
        return;
    }

    uint64_t full = reader.physical_size();
    if (full == 0) {
        finish("failed", "cannot determine disk size");
        return;
    }

    const std::string serial = read_disk_serial(disk);

    // 1. Locate every surviving $MFT record-0 across the WHOLE disk (not just
    //    the first 16 GiB — a multi-partition disk has one $MFT per volume,
    //    each ~3 GiB into its own volume). This phase used to be silent, which
    //    made the frontend look like WebSocket wasn't connected; emit a first
    //    event immediately, then report candidate-search progress [1, 9]%.
    {
        std::lock_guard<std::mutex> lock(meta->mtx);
        meta->current_path = "正在定位 NTFS 分区 ($MFT)…";
    }
    meta->progress = 1;
    emit(true);

    std::vector<RawMftCandidate> cands = scan_mft_candidates(
        reader, full, &meta->stop_requested,
        [&](uint64_t done) {
            if (meta->stop_requested.load()) return;
            int pct = (full == 0) ? 5 : 1 + static_cast<int>((8 * done) / full);
            if (pct > 9) pct = 9;
            if (pct > meta->progress.load()) {
                meta->progress = pct;
                emit(false);
            }
        });
    if (meta->stop_requested.load()) { finish("stopped", ""); return; }
    if (cands.empty()) {
        finish("failed", "no surviving $MFT found on the disk");
        return;
    }

    // 2. Keep candidates whose record-0 data runs are self-consistent (cluster
    //    size detectable), dedup by implied volume start, and drop tiny stale
    //    fragments (fewer than kMinRecords records — real volumes have tens of
    //    thousands; stale remnants are < 100).
    constexpr uint64_t kMinRecords = 256;
    struct Volume {
        uint64_t mft_offset = 0;
        uint64_t cluster_size = 0;
        uint64_t volume_start = 0;
        uint64_t total_records = 0;
    };
    std::vector<Volume> vols;
    MftParser parser(reader);
    std::vector<uint8_t> rec0(1024);

    for (const auto& cand : cands) {
        FileNode n0;
        size_t rg = 0;
        std::fill(rec0.begin(), rec0.end(), 0);
        if (!reader.read(cand.offset, rec0.data(), 1024, &rg) || rg < 512) continue;
        MftParser::apply_usa_fixup(rec0, 512);
        if (!parser.parse_record(rec0, n0) || n0.name != u"$MFT") continue;

        uint64_t tr = (n0.size >= 1024) ? (n0.size / 1024) : 0;
        if (tr < kMinRecords) continue;

        uint64_t cs = 0;
        if (tr >= 2 && !n0.data_runs.empty())
            cs = detect_cluster_size(reader, cand.offset, n0.data_runs, tr);
        if (cs == 0) continue; // not self-consistent → stale copy / mirror

        uint64_t vs = cand.offset - static_cast<uint64_t>(n0.data_runs[0].lcn) * cs;
        bool dup = false;
        for (const auto& v : vols) {
            if (v.volume_start == vs) { dup = true; break; }
        }
        if (dup) continue;
        vols.push_back({cand.offset, cs, vs, tr});
    }

    if (vols.empty()) {
        finish("failed", "no self-consistent NTFS volume found");
        return;
    }

    std::sort(vols.begin(), vols.end(), [](const Volume& a, const Volume& b) {
        return a.volume_start < b.volume_start;
    });

    meta->progress = 10;
    emit(true);

    // 3. Spawn one scan task per volume. Each gets a deterministic id/save name
    //    ("disk<N>_v<K>") so a restart reloads them the same way, and so a
    //    re-scan cleanly replaces the previous result.
    std::vector<std::shared_ptr<ScanTask>> subs;
    std::vector<std::string> part_ids;
    part_ids.reserve(vols.size());
    for (size_t k = 0; k < vols.size(); k++) {
        auto t = std::make_shared<ScanTask>();
        t->id = "disk" + std::to_string(disk) + "_v" + std::to_string(k);
        part_ids.push_back(t->id);
        t->save_name = "scan_disk" + std::to_string(disk) + "_v" + std::to_string(k);
        char path[64];
        snprintf(path, sizeof(path), "\\\\.\\PhysicalDrive%d", disk);
        t->drive = path;
        t->mode = "raw";
        t->disk_number = disk;
        t->raw = true;
        t->mft_offset_hint = vols[k].mft_offset;
        t->status = "running";
        t->progress = 0;

        {
            std::lock_guard<std::mutex> lock(mutex_);
            tasks_[t->id] = t;
        }
        {
            std::lock_guard<std::mutex> lock(threads_mutex_);
            threads_.emplace_back(&Scanner::raw_scan_worker, this, t);
        }
        subs.push_back(t);
    }

    // 4. Wait for all per-volume scans, then aggregate. While waiting, surface a
    //    rolling aggregate (counters + the active partition's current file) via
    //    WS, and honor pause/stop by forwarding them to the per-volume tasks.
    for (;;) {
        if (meta->stop_requested.load()) {
            for (const auto& s : subs) s->stop_requested.store(true);
        } else if (meta->pause_requested.load()) {
            for (const auto& s : subs) s->pause_requested.store(true);
        } else {
            for (const auto& s : subs) s->pause_requested.store(false);
        }

        if (meta->pause_requested.load() && !meta->stop_requested.load()) {
            { std::lock_guard<std::mutex> lock(meta->mtx); meta->status = "paused"; }
            emit(true, "paused");
            while (meta->pause_requested.load() && !meta->stop_requested.load())
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            { std::lock_guard<std::mutex> lock(meta->mtx); meta->status = "running"; }
        }

        uint64_t tf = 0, del = 0, dirs = 0;
        int64_t pct_sum = 0;
        bool any_running = false;
        std::string hint;
        for (size_t k = 0; k < subs.size(); ++k) {
            std::string st, cp;
            { std::lock_guard<std::mutex> lock(subs[k]->mtx); st = subs[k]->status; cp = subs[k]->current_path; }
            tf += subs[k]->total_files.load();
            del += subs[k]->deleted_files.load();
            dirs += subs[k]->directories.load();
            pct_sum += subs[k]->progress.load();
            if (st == "running" || st == "paused") {
                any_running = true;
                if (hint.empty()) {
                    hint = "分区 " + std::to_string(k + 1) + "/" + std::to_string(subs.size());
                    if (!cp.empty()) hint += " · " + cp;
                }
            }
        }
        meta->total_files.store(tf);
        meta->deleted_files.store(del);
        meta->directories.store(dirs);
        meta->progress = subs.empty() ? 10
            : 10 + static_cast<int>((90 * (pct_sum / subs.size())) / 100);
        { std::lock_guard<std::mutex> lock(meta->mtx); meta->current_path = hint; }

        if (meta->stop_requested.load()) break;
        emit(false);

        if (!any_running) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    if (meta->stop_requested.load()) {
        finish("stopped", "");
        return;
    }

    uint64_t tf = 0, del = 0, dirs = 0;
    bool any_failed = false;
    for (const auto& s : subs) {
        std::string st;
        { std::lock_guard<std::mutex> lock(s->mtx); st = s->status; }
        if (st != "completed") any_failed = true;
        tf += s->total_files.load();
        del += s->deleted_files.load();
        dirs += s->directories.load();
    }
    meta->total_files.store(tf);
    meta->deleted_files.store(del);
    meta->directories.store(dirs);

    // Record this whole-disk scan as one session, keyed by the disk's serial
    // number (same serial → replace the previous record). Done even if a single
    // partition failed, so the user still sees the other completed partitions.
    record_session(serial, disk, part_ids);

    finish(any_failed ? "failed" : "completed",
           any_failed ? "one or more partitions failed to scan" : "");
}

} // namespace recovery
