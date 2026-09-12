#include "restorer.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>

#include <nlohmann/json.hpp>

#include "../include/util.h"
#include "../ntfs/boot_parser.h"
#include "../ntfs/disk_reader.h"
#include "scanner.h"

namespace fs = std::filesystem;

namespace recovery {

namespace {

using json = nlohmann::json;

std::string make_id(const char* prefix, uint64_t n) {
    auto now = std::chrono::system_clock::now().time_since_epoch().count();
    char buf[64];
    snprintf(buf, sizeof(buf), "%s-%llu-%lld", prefix,
             static_cast<unsigned long long>(n),
             static_cast<long long>(now % 1000000));
    return std::string(buf);
}

std::string drive_letter_of(const std::string& path) {
    size_t i = 0;
    while (i < path.size() && (path[i] == ' ' || path[i] == '\\' || path[i] == '/')) i++;
    if (i < path.size() && std::isalpha(static_cast<unsigned char>(path[i])) &&
        (i + 1 < path.size() && path[i + 1] == ':')) {
        return std::string(1, static_cast<char>(toupper(static_cast<unsigned char>(path[i])))) + ":";
    }
    return "";
}

// Resolve a root-relative path ("/Documents/report.pdf") to a node.
FileNodePtr resolve_path(FileNodePtr root, const std::string& path) {
    if (!root) return nullptr;
    std::vector<std::string> parts;
    size_t start = 0;
    while (start <= path.size()) {
        size_t slash = path.find('/', start);
        std::string seg = path.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        if (!seg.empty()) parts.push_back(seg);
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    if (parts.empty()) return root;

    FileNodePtr cur = root;
    for (const auto& seg : parts) {
        FileNodePtr next = nullptr;
        for (const auto& c : cur->children) {
            if (utf16_to_utf8(c->name) == seg) { next = c; break; }
        }
        if (!next) return nullptr;
        cur = next;
    }
    return cur;
}

// Replace characters illegal in Windows file names with '_'.
std::string sanitize_name(const std::string& in) {
    std::string out = in;
    for (auto& c : out) {
        if (c == '<' || c == '>' || c == ':' || c == '"' || c == '|' ||
            c == '?' || c == '*') {
            c = '_';
        }
        unsigned char u = static_cast<unsigned char>(c);
        if (u < 0x20) c = '_';
    }
    if (out.empty()) out = "_";
    return out;
}

void count_files(const FileNodePtr& n, uint64_t& total, uint64_t& bytes) {
    if (!n->is_directory) { total++; bytes += n->size; return; }
    for (const auto& c : n->children) count_files(c, total, bytes);
}

// A single-producer / single-consumer ring of buffers that pipelines source
// reads ahead of destination writes during file recovery. The producer thread
// reads chunk N+1 into a free buffer while the consumer thread writes chunk N,
// so the read latency no longer adds to the write latency; throughput rises
// from read+write serialized toward the slower disk's speed.
class ReadPipeline {
public:
    struct Piece {
        const char* data;  // owned buffer (slot) or the zero buffer (slot == -1)
        size_t      len;   // bytes to write
        int         slot;  // buffer index to release, or -1 for a zero piece
    };

    ReadPipeline(size_t slots, size_t chunk) : chunk_(chunk) {
        buf_.resize(slots);
        for (auto& b : buf_) b.resize(chunk);
        for (int i = static_cast<int>(slots) - 1; i >= 0; --i) free_.push_back(i);
    }

    uint8_t* buffer(int slot) { return buf_[static_cast<size_t>(slot)].data(); }
    size_t chunk_size() const { return chunk_; }

    // Producer side.
    int acquire() {  // a free buffer slot, or -1 when closed
        std::unique_lock<std::mutex> lk(m_);
        cv_free_.wait(lk, [&] { return !free_.empty() || closed_; });
        if (closed_) return -1;
        int s = free_.back(); free_.pop_back();
        return s;
    }
    void publish(const Piece& p) {
        std::unique_lock<std::mutex> lk(m_);
        ready_.push_back(p);
        cv_ready_.notify_one();
    }
    void close() {
        std::unique_lock<std::mutex> lk(m_);
        closed_ = true;
        cv_ready_.notify_all();
        cv_free_.notify_all();
    }

    // Consumer side. Returns false once the stream is drained and closed.
    bool next(Piece* out) {
        std::unique_lock<std::mutex> lk(m_);
        cv_ready_.wait(lk, [&] { return !ready_.empty() || closed_; });
        if (ready_.empty()) return false;
        *out = ready_.front(); ready_.pop_front();
        return true;
    }
    void release(int slot) {
        if (slot < 0) return;
        std::unique_lock<std::mutex> lk(m_);
        free_.push_back(slot);
        cv_free_.notify_one();
    }

private:
    size_t chunk_;
    std::vector<std::vector<uint8_t>> buf_;
    std::mutex m_;
    std::condition_variable cv_free_, cv_ready_;
    std::vector<int> free_;
    std::deque<Piece> ready_;
    bool closed_ = false;
};

} // namespace

Restorer::~Restorer() {
    join_workers();
}

std::string Restorer::start_recover(std::shared_ptr<ScanTask> task,
                                    const std::vector<std::string>& paths,
                                    const std::string& output_dir) {
    auto job = std::make_shared<RecoverJob>();
    job->id = make_id("recv", counter_.fetch_add(1));
    job->task_id = task ? task->id : "";
    job->output_dir = output_dir;
    job->status = "running";
    job->progress = 0;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        jobs_[job->id] = job;
    }

    threads_.emplace_back(&Restorer::recover_worker, this, job, task, paths);
    return job->id;
}

std::shared_ptr<RecoverJob> Restorer::get_job(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = jobs_.find(id);
    if (it == jobs_.end()) return nullptr;
    return it->second;
}

std::vector<std::shared_ptr<RecoverJob>> Restorer::list_jobs() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::shared_ptr<RecoverJob>> out;
    out.reserve(jobs_.size());
    for (const auto& kv : jobs_) out.push_back(kv.second);
    return out;
}

bool Restorer::pause(const std::string& id) {
    auto job = get_job(id);
    if (!job) return false;
    {
        std::lock_guard<std::mutex> lock(job->mtx);
        if (job->status != "running" && job->status != "paused") return false;
    }
    job->pause_requested.store(true);
    return true;
}

bool Restorer::resume(const std::string& id) {
    auto job = get_job(id);
    if (!job) return false;
    job->pause_requested.store(false);
    return true;
}

bool Restorer::stop(const std::string& id) {
    auto job = get_job(id);
    if (!job) return false;
    job->stop_requested.store(true);
    job->pause_requested.store(false); // let the worker wake up and exit
    return true;
}

void Restorer::set_event_callback(std::function<void(const std::string&)> cb) {
    event_cb_ = std::move(cb);
}

void Restorer::recover_worker(std::shared_ptr<RecoverJob> job,
                              std::shared_ptr<ScanTask> task,
                              std::vector<std::string> paths) {
    // Snapshot the event callback (set once at startup, before any recover).
    std::function<void(const std::string&)> emit_raw = event_cb_;

    // Emit a throttled progress snapshot. `force` bypasses the rate limit (used
    // on file boundaries, pause/resume transitions, and completion).
    auto last_emit = std::chrono::steady_clock::now() - std::chrono::seconds(1);
    auto emit = [&](bool force, const char* type = "progress") {
        auto now = std::chrono::steady_clock::now();
        if (!force && now - last_emit < std::chrono::milliseconds(50)) return;
        last_emit = now;
        if (!emit_raw) return;
        json j;
        j["type"] = type;
        j["job_id"] = job->id;
        j["progress"] = job->progress.load();
        j["total_files"] = job->total_files.load();
        j["recovered_files"] = job->recovered_files.load();
        j["failed_files"] = job->failed_files.load();
        j["total_bytes"] = job->total_bytes.load();
        j["recovered_bytes"] = job->recovered_bytes.load();
        // ETA is driven by byte progress (recovered / total), which advances
        // continuously during a large file — unlike the coarse `progress` %
        // that only moves at file boundaries.
        {
            uint64_t rb = job->recovered_bytes.load();
            uint64_t tb = job->total_bytes.load();
            int pct = 0;
            if (tb > 0) pct = static_cast<int>((rb * 100) / tb);
            else {
                uint64_t done = job->recovered_files.load() + job->failed_files.load();
                uint64_t tf = job->total_files.load();
                if (tf > 0) pct = static_cast<int>((done * 100) / tf);
            }
            job->eta.update(pct);
            job->eta_seconds.store(job->eta.eta_seconds());
        }
        j["eta_seconds"] = job->eta_seconds.load();
        {
            std::lock_guard<std::mutex> lock(job->mtx);
            j["status"] = job->status;
            j["current_file"] = job->current_file;
            j["current_bytes"] = job->current_bytes;
            j["current_size"] = job->current_size;
        }
        emit_raw(j.dump());
    };

    auto finish = [&](const std::string& status, const std::string& err) {
        {
            std::lock_guard<std::mutex> lock(job->mtx);
            job->status = status;
            job->error = err;
            job->current_file.clear();
        }
        if (status == "completed") job->progress = 100;
        const char* t = (status == "completed") ? "done"
                        : (status == "stopped") ? "stopped"
                        : "error";
        emit(true, t);
    };

    if (!task) { finish("failed", "scan task not found"); return; }
    bool raw = false;
    int disk_number = -1;
    uint64_t volume_start = 0, cluster_size = 0;
    {
        std::lock_guard<std::mutex> lock(task->mtx);
        if (task->status != "completed") {
            finish("failed", "scan task is not completed");
            return;
        }
        raw = task->raw;
        disk_number = task->disk_number;
        volume_start = task->volume_start;
        cluster_size = task->cluster_size;
    }

    // Re-open the source read-only.
    DiskReader reader;
    std::string err;
    if (raw) {
        if (!reader.open_physical(disk_number, &err)) { finish("failed", "open physical disk: " + err); return; }
        reader.set_base_offset(volume_start);
        if (cluster_size >= 512) reader.set_cluster_size(cluster_size);
    } else {
        if (!reader.open(task->drive, &err)) { finish("failed", "open volume: " + err); return; }
        if (!BootParser::parse(reader, &err)) { finish("failed", "parse boot sector: " + err); return; }
    }

    FileNodePtr root;
    {
        std::lock_guard<std::mutex> lock(task->mtx);
        root = task->root;
    }
    if (!root) { finish("failed", "scan produced no result tree"); return; }

    // Validate output directory is on a different disk.
    const std::string out = job->output_dir;
    if (out.empty()) { finish("failed", "output_dir is required"); return; }
    std::string src_letter = drive_letter_of(task->drive);
    std::string dst_letter = drive_letter_of(out);
    if (!src_letter.empty() && !dst_letter.empty() && src_letter == dst_letter) {
        finish("failed", "output directory must be on a different disk than the source");
        return;
    }

    fs::path out_base = fs::u8path(out);
    std::error_code ec;
    fs::create_directories(out_base, ec);
    if (ec) { finish("failed", "cannot create output directory: " + out); return; }

    // Resolve selected paths, keeping their root-relative path so the original
    // directory structure is preserved on recovery.
    std::vector<std::pair<FileNodePtr, std::string>> selected;
    for (const auto& p : paths) {
        FileNodePtr n = resolve_path(root, p);
        if (!n) continue;
        std::string rel = p;
        while (!rel.empty() && (rel.front() == '/' || rel.front() == '\\'))
            rel.erase(rel.begin());
        selected.emplace_back(n, rel);
    }
    if (selected.empty()) { finish("failed", "none of the selected paths could be resolved"); return; }

    // Pre-count total files + bytes.
    uint64_t total = 0, total_bytes = 0;
    for (const auto& pr : selected) count_files(pr.first, total, total_bytes);
    job->total_files.store(total);
    job->total_bytes.store(total_bytes);
    if (total == 0) total = 1;

    // Block while paused; honors stop (which clears pause).
    auto wait_if_paused = [&]() {
        if (!job->pause_requested.load()) return;
        {
            std::lock_guard<std::mutex> lock(job->mtx);
            if (job->status == "running") job->status = "paused";
        }
        emit(true);
        while (job->pause_requested.load() && !job->stop_requested.load())
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (!job->stop_requested.load()) {
            std::lock_guard<std::mutex> lock(job->mtx);
            job->status = "running";
        }
        emit(true);
    };

    // Use the reader's override-aware cluster size. In raw mode boot_ may only
    // hold the default 512-byte geometry, while the real volume cluster size
    // (FAT/exFAT clusters can be 128 KiB, NTFS 512 B – 2 MiB) lives in the
    // override set from the scan task.
    const uint64_t cluster_size_bytes = reader.cluster_size();
    const uint64_t cs = cluster_size_bytes ? cluster_size_bytes : 4096;

    // Batched contiguous reads: a synchronous, unbuffered 4 KiB read per cluster
    // (NO_BUFFERING + SetFilePointerEx + ReadFile each time) caps recovery at
    // ~30 MB/s on an SSD. Reading each data-run span in 4 MiB batches turns
    // thousands of tiny reads into a handful of large sequential ones and lifts
    // throughput to near the disk's sequential speed. The buffers are large
    // enough to be page-aligned, satisfying NO_BUFFERING's sector alignment.
    constexpr size_t CHUNK = 4u * 1024u * 1024u;  // 4 MiB read/write batches
    std::vector<uint8_t> zeros(CHUNK, 0);

    // Recursive recovery. `rel` is the node's path relative to out_base.
    std::function<bool(const FileNodePtr&, const fs::path&)> recover_node;
    recover_node = [&](const FileNodePtr& node, const fs::path& rel) -> bool {
        if (node->is_directory) {
            fs::path dir = rel.empty() ? out_base : out_base / rel;
            std::error_code dec;
            fs::create_directories(dir, dec);
            bool ok = true;
            for (const auto& c : node->children) {
                wait_if_paused();
                if (job->stop_requested.load()) return false;
                fs::path child_rel = rel / fs::u8path(sanitize_name(utf16_to_utf8(c->name)));
                if (!recover_node(c, child_rel)) ok = false;
            }
            return ok;
        }

        // File.
        fs::path file = out_base / rel;
        std::error_code dec;
        fs::create_directories(file.parent_path(), dec);

        // Avoid silently overwriting an existing file.
        fs::path target = file;
        int suffix = 1;
        while (fs::exists(target)) {
            fs::path stem = file.stem();
            fs::path ext = file.extension();
            target = file.parent_path() / (stem.wstring() + L"_" + std::to_wstring(suffix++) + ext.wstring());
        }

        // Large output buffer so sequential writes turn into a few big
        // WriteFile calls rather than many small ones (declared before ofs so
        // it is destroyed after the stream).
        std::vector<char> wbuf(8u * 1024u * 1024u);
        std::ofstream ofs(target, std::ios::binary);
        if (!ofs.is_open()) {
            job->failed_files.fetch_add(1);
            emit(true);
            return false;
        }
        ofs.rdbuf()->pubsetbuf(wbuf.data(), static_cast<std::streamsize>(wbuf.size()));

        // Announce the current file and write its data with pause/stop checks.
        {
            std::lock_guard<std::mutex> lock(job->mtx);
            job->current_file = utf16_to_utf8(node->name);
            job->current_size = node->size;
            job->current_bytes = 0;
        }
        emit(true);

        bool ok = true;
        uint64_t remaining = node->size;

        if (node->resident) {
            wait_if_paused();
            if (job->stop_requested.load()) { ofs.close(); return false; }
            ofs.write(reinterpret_cast<const char*>(node->resident_data.data()),
                      static_cast<std::streamsize>(node->resident_data.size()));
            ok = ofs.good();
            remaining = 0;
            job->recovered_bytes.fetch_add(node->resident_data.size());
            {
                std::lock_guard<std::mutex> lock(job->mtx);
                job->current_bytes = node->resident_data.size();
            }
        } else if (!node->data_runs.empty()) {
            // Flatten the file's data runs into contiguous read segments.
            struct Seg { uint64_t off; uint64_t len; bool sparse; };
            std::vector<Seg> segs;
            {
                uint64_t rem = node->size;
                for (const auto& run : node->data_runs) {
                    if (rem == 0) break;
                    uint64_t rb = run.length * cs;
                    uint64_t n = std::min<uint64_t>(rem, rb);
                    if (n == 0) continue;
                    segs.push_back({static_cast<uint64_t>(run.lcn) * cs, n, run.sparse});
                    rem -= n;
                }
            }

            uint64_t delivered = 0;

            if (node->size <= CHUNK) {
                // Small file (a single batch): read and write inline. A prefetch
                // thread only pays off when the file spans multiple chunks.
                std::vector<uint8_t> scratch(CHUNK);
                for (const auto& seg : segs) {
                    uint64_t off = seg.off;
                    uint64_t left = seg.len;
                    while (left > 0) {
                        wait_if_paused();
                        if (job->stop_requested.load()) { ok = false; break; }
                        size_t step = left > CHUNK ? CHUNK : static_cast<size_t>(left);
                        const char* src;
                        if (seg.sparse) {
                            src = reinterpret_cast<const char*>(zeros.data());
                        } else if (reader.read(off, scratch.data(), step)) {
                            src = reinterpret_cast<const char*>(scratch.data());
                        } else {
                            src = reinterpret_cast<const char*>(zeros.data()); // bad sector -> zeroes
                        }
                        ofs.write(src, static_cast<std::streamsize>(step));
                        if (!ofs.good()) { ok = false; break; }
                        delivered += step;
                        job->recovered_bytes.fetch_add(step);
                        off += step;
                        left -= step;
                    }
                    if (!ok) break;
                }
            } else {
                // Pipeline reads ahead of writes: the producer reads chunk N+1
                // while this thread writes chunk N, overlapping the two disks
                // instead of paying read-latency + write-latency per chunk.
                ReadPipeline pipe(3, CHUNK);
                std::thread producer([&]() {
                    for (const auto& seg : segs) {
                        uint64_t off = seg.off;
                        uint64_t left = seg.len;
                        while (left > 0) {
                            if (job->stop_requested.load()) { pipe.close(); return; }
                            size_t step = left > CHUNK ? CHUNK : static_cast<size_t>(left);
                            if (seg.sparse) {
                                pipe.publish({reinterpret_cast<const char*>(zeros.data()), step, -1});
                            } else {
                                int slot = pipe.acquire();
                                if (slot < 0) return;  // closed
                                if (!reader.read(off, pipe.buffer(slot), step))
                                    std::memset(pipe.buffer(slot), 0, step); // bad sector -> zeroes
                                pipe.publish({reinterpret_cast<const char*>(pipe.buffer(slot)), step, slot});
                            }
                            off += step;
                            left -= step;
                        }
                    }
                    pipe.close(); // end-of-stream
                });

                ReadPipeline::Piece p;
                while (pipe.next(&p)) {
                    wait_if_paused();
                    if (job->stop_requested.load()) { pipe.close(); ok = false; break; }
                    ofs.write(p.data, static_cast<std::streamsize>(p.len));
                    if (!ofs.good()) { pipe.close(); ok = false; break; }
                    pipe.release(p.slot);
                    delivered += p.len;
                    job->recovered_bytes.fetch_add(p.len);
                    {
                        std::lock_guard<std::mutex> lock(job->mtx);
                        job->current_bytes = delivered;
                    }
                    emit(false);
                }
                if (producer.joinable()) producer.join();
            }

            if (delivered < node->size) ok = false;
        }
        ofs.close();

        if (ok) job->recovered_files.fetch_add(1);
        else job->failed_files.fetch_add(1);

        uint64_t done = job->recovered_files.load() + job->failed_files.load();
        uint64_t rb = job->recovered_bytes.load();
        if (total_bytes > 0)
            job->progress.store(static_cast<int>((rb * 100) / total_bytes));
        else
            job->progress.store(static_cast<int>((done * 100) / total));

        {
            std::lock_guard<std::mutex> lock(job->mtx);
            job->current_file.clear();
        }
        emit(true);
        return ok;
    };

    for (const auto& pr : selected) {
        wait_if_paused();
        if (job->stop_requested.load()) break;
        if (pr.first == root) {
            for (const auto& c : root->children) {
                wait_if_paused();
                if (job->stop_requested.load()) break;
                recover_node(c, fs::u8path(sanitize_name(utf16_to_utf8(c->name))));
            }
        } else {
            recover_node(pr.first, fs::u8path(pr.second));
        }
    }

    if (job->stop_requested.load()) {
        finish("stopped", "");
    } else {
        finish("completed", "");
    }
}

} // namespace recovery
