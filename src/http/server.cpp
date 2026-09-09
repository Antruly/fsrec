#include "server.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <functional>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

#include <windows.h>
#include <winioctl.h>

namespace fs = std::filesystem;

#include <nlohmann/json.hpp>

#include "../include/util.h"
#include "../include/version.h"
#include "../ntfs/disk_reader.h"

#include <handle/uvcpp_timer.h>

using json = nlohmann::json;

namespace recovery {

namespace {

// ---------------------------------------------------------------------------
// Response helpers
// ---------------------------------------------------------------------------
void add_cors(uvcpp::uvcpp_http_response& resp) {
    resp.set_header("Access-Control-Allow-Origin", "*");
    resp.set_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    resp.set_header("Access-Control-Allow-Headers", "Content-Type");
}

void set_json(uvcpp::uvcpp_http_response& resp, uvcpp::http_status code,
              const std::string& body) {
    resp = uvcpp::uvcpp_http_response::make(code, body.data(), body.size(),
                                            "application/json");
    add_cors(resp);
}

void set_error(uvcpp::uvcpp_http_response& resp, uvcpp::http_status code,
               const std::string& msg) {
    json j;
    j["error"] = msg;
    set_json(resp, code, j.dump());
}

void set_options(uvcpp::uvcpp_http_response& resp) {
    resp = uvcpp::uvcpp_http_response::make(uvcpp::http_status::NO_CONTENT);
    add_cors(resp);
}

// ---------------------------------------------------------------------------
// String helpers
// ---------------------------------------------------------------------------
std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string wtoa(const std::wstring& w) {
    if (w.empty()) return "";
    int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                                  nullptr, 0, nullptr, nullptr);
    if (len <= 0) return "";
    std::string out(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                        &out[0], len, nullptr, nullptr);
    return out;
}

std::vector<std::string> split_path(const std::string& path) {
    std::vector<std::string> segs;
    size_t start = 0;
    while (start <= path.size()) {
        size_t slash = path.find('/', start);
        std::string seg = path.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        if (!seg.empty()) segs.push_back(seg);
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    return segs;
}

// ---------------------------------------------------------------------------
// Tree serialization
// ---------------------------------------------------------------------------
json node_to_json(const FileNodePtr& node, bool as_root, int depth, int max_depth) {
    json j;
    j["name"] = as_root ? "/" : utf16_to_utf8(node->name);
    j["type"] = node->is_directory ? "folder" : "file";
    j["mft_id"] = node->mft_id;
    j["is_deleted"] = node->is_deleted;
    j["size"] = node->size;
    j["modified_time"] = filetime_to_iso8601(node->modified_time);
    j["recoverable"] = node->is_directory ? true : node->recoverable;

    if (node->is_directory) {
        j["children"] = json::array();
        if (max_depth < 0 || depth < max_depth) {
            for (const auto& c : node->children) {
                j["children"].push_back(node_to_json(c, false, depth + 1, max_depth));
            }
        }
    }
    return j;
}

// ---------------------------------------------------------------------------
// Disk enumeration
// ---------------------------------------------------------------------------

// Map a drive letter ("C:") to the physical disk number(s) it lives on via
// IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS. Used to forbid selecting the source
// disk as a recovery destination.
std::vector<int> volume_disk_numbers(const std::string& letter) {
    std::vector<int> disks;
    std::string vol = "\\\\.\\" + letter;  // e.g. "\\.\C:"
    HANDLE h = CreateFileA(vol.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return disks;

    std::vector<uint8_t> buf(sizeof(VOLUME_DISK_EXTENTS) + sizeof(DISK_EXTENT) * 16, 0);
    DWORD bytes = 0;
    if (DeviceIoControl(h, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, nullptr, 0,
                        buf.data(), static_cast<DWORD>(buf.size()), &bytes, nullptr)) {
        auto* ext = reinterpret_cast<VOLUME_DISK_EXTENTS*>(buf.data());
        for (DWORD k = 0; k < ext->NumberOfDiskExtents; k++)
            disks.push_back(static_cast<int>(ext->Extents[k].DiskNumber));
    }
    CloseHandle(h);
    return disks;
}

json enumerate_disks() {
    json arr = json::array();
    DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; i++) {
        if (!(mask & (1u << i))) continue;
        char letter = static_cast<char>('A' + i);
        std::string rootA = std::string(1, letter) + ":";

        UINT dtype = GetDriveTypeA(rootA.c_str());
        if (dtype != DRIVE_FIXED && dtype != DRIVE_REMOVABLE) continue;

        std::wstring wroot(1, static_cast<wchar_t>(letter));
        wroot += L":\\";
        wchar_t wlabel[256] = {0};
        wchar_t wfs[64] = {0};
        DWORD serial = 0, maxlen = 0, flags = 0;
        BOOL ok = GetVolumeInformationW(wroot.c_str(), wlabel, 256, &serial,
                                        &maxlen, &flags, wfs, 64);

        ULARGE_INTEGER free_avail, total, total_free;
        free_avail.QuadPart = total.QuadPart = total_free.QuadPart = 0;
        GetDiskFreeSpaceExA(rootA.c_str(), &free_avail, &total, &total_free);

        std::string fs = ok ? wtoa(wfs) : "";

        json phys = json::array();
        for (int n : volume_disk_numbers(std::string(1, letter) + ":"))
            phys.push_back(n);

        json d;
        d["letter"] = std::string(1, letter) + ":";
        d["root"] = rootA;
        d["label"] = ok ? wtoa(wlabel) : "";
        d["fs_type"] = fs;
        d["total_size"] = total.QuadPart;
        d["free_size"] = total_free.QuadPart;
        d["is_ntfs"] = (fs == "NTFS");
        d["physical_disks"] = phys;
        arr.push_back(d);
    }
    return arr;
}

// List only the subdirectories of `path` (the recovery destination picker
// navigates folders). Returns { ok, error?, path, parent, entries:[{name,path}] }.
json list_directories(const std::string& path) {
    json out;
    std::error_code ec;
    fs::path p = fs::u8path(path);
    if (!fs::exists(p, ec) || !fs::is_directory(p, ec)) {
        out["ok"] = false;
        out["error"] = "path does not exist or is not a directory";
        out["path"] = path;
        out["parent"] = "";
        out["entries"] = json::array();
        return out;
    }
    std::vector<json> entries;
    fs::directory_iterator it(p, ec);
    fs::directory_iterator end;
    for (; !ec && it != end; it.increment(ec)) {
        std::error_code sec;
        if (!it->is_directory(sec)) continue;
        json e;
        e["name"] = wtoa(it->path().filename().wstring());
        e["path"] = wtoa(it->path().wstring());
        entries.push_back(std::move(e));
    }
    std::sort(entries.begin(), entries.end(), [](const json& a, const json& b) {
        return a["name"].get<std::string>() < b["name"].get<std::string>();
    });
    out["ok"] = true;
    out["path"] = path;
    fs::path parent = p.parent_path();
    out["parent"] = (parent.empty() || parent == p) ? "" : wtoa(parent.wstring());
    out["entries"] = entries;
    return out;
}

// Free/total bytes on the volume containing `path`.
json space_info(const std::string& path) {
    json out;
    out["path"] = path;
    std::string rootA = "C:\\";
    size_t i = 0;
    while (i < path.size() && std::isspace(static_cast<unsigned char>(path[i]))) i++;
    if (i + 1 < path.size() && std::isalpha(static_cast<unsigned char>(path[i])) &&
        path[i + 1] == ':') {
        rootA = std::string(1, static_cast<char>(toupper(static_cast<unsigned char>(path[i])))) + ":\\";
    }
    ULARGE_INTEGER free_avail, total, total_free;
    free_avail.QuadPart = total.QuadPart = total_free.QuadPart = 0;
    if (GetDiskFreeSpaceExA(rootA.c_str(), &free_avail, &total, &total_free)) {
        out["total_size"] = total.QuadPart;
        out["free_size"] = total_free.QuadPart;
    } else {
        out["total_size"] = 0;
        out["free_size"] = 0;
    }
    out["root"] = rootA;
    return out;
}

// Enumerate physical disks (\\\\.\\PhysicalDriveN) regardless of whether they
// currently hold a mounted, recognized volume — needed to locate a drive whose
// NTFS filesystem has been overwritten (e.g. reformatted as a PE USB stick).
json enumerate_physical_disks() {
    json arr = json::array();
    for (int i = 0; i < 32; i++) {
        char path[64];
        snprintf(path, sizeof(path), "\\\\.\\PhysicalDrive%d", i);
        HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;

        uint64_t size = 0;
        DWORD unused = 0;
        GET_LENGTH_INFORMATION li{};
        if (DeviceIoControl(h, IOCTL_DISK_GET_LENGTH_INFO, nullptr, 0,
                            &li, sizeof(li), &unused, nullptr)) {
            size = static_cast<uint64_t>(li.Length.QuadPart);
        }

        std::string model;
        std::string serial;
        std::vector<uint8_t> desc_buf(sizeof(STORAGE_DEVICE_DESCRIPTOR) + 512, 0);
        STORAGE_PROPERTY_QUERY spq{};
        spq.PropertyId = StorageDeviceProperty;
        spq.QueryType = PropertyStandardQuery;
        if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &spq, sizeof(spq),
                            desc_buf.data(), static_cast<DWORD>(desc_buf.size()),
                            &unused, nullptr)) {
            auto* desc = reinterpret_cast<PSTORAGE_DEVICE_DESCRIPTOR>(desc_buf.data());
            if (desc->ProductIdOffset != 0) {
                const char* p = reinterpret_cast<const char*>(desc_buf.data() + desc->ProductIdOffset);
                model.assign(p, strnlen(p, 512));
            }
            if (desc->SerialNumberOffset != 0) {
                const char* p = reinterpret_cast<const char*>(desc_buf.data() + desc->SerialNumberOffset);
                serial.assign(p, strnlen(p, 512));
            }
        }
        CloseHandle(h);

        json d;
        d["disk_number"] = i;
        d["path"] = path;
        d["size"] = size;
        d["model"] = model;
        d["serial"] = serial;
        arr.push_back(d);
    }
    return arr;
}

// Is an all-zero (or empty) serial usable? Many disks / USB bridges report
// "0000...00" — treat that as "no serial" so we fall back to model+size.
bool is_zero_serial(const std::string& s) {
    return s.empty() || s.find_first_not_of('0') == std::string::npos;
}

// Compare a disk identity recorded at scan time against one physical disk
// (`cur`, a /api/physical_disks element) currently attached. Serial is the
// strongest key; when absent/all-zero we require model + size to both match
// (whichever are known). If there is no usable identity at all we return false
// — it is always safer to refuse recovery than to guess.
bool disk_identity_matches(const std::string& serial, const std::string& model,
                           uint64_t size, const json& cur) {
    if (!is_zero_serial(serial)) {
        return cur.value("serial", "") == serial;
    }
    bool has_model = !model.empty();
    bool has_size = size > 0;
    if (!has_model && !has_size) return false;
    if (has_model && cur.value("model", "") != model) return false;
    if (has_size && cur.value("size", static_cast<uint64_t>(0)) != size) return false;
    return true;
}

// Does the physical disk currently present at `disk_number` still match the
// identity recorded at scan time? Returns false when the disk is absent or has
// been replaced by a *different* disk (a reused \\.\PhysicalDriveN slot).
bool source_disk_connected(int disk_number, const std::string& serial,
                           const std::string& model, uint64_t size,
                           const json& disks) {
    for (const auto& d : disks) {
        if (d.value("disk_number", -1) == disk_number)
            return disk_identity_matches(serial, model, size, d);
    }
    return false;
}

} // namespace

// ---------------------------------------------------------------------------
// WebSocket hub
// ---------------------------------------------------------------------------

struct HttpApi::Hub {
    std::mutex mtx;
    std::vector<std::string> queue;
    uvcpp::uvcpp_async* async = nullptr;
};

void HttpApi::remove_client(uvcpp::uvcpp_ws_connection* conn) {
    std::lock_guard<std::mutex> lock(clients_mtx_);
    clients_.erase(conn);
}

void HttpApi::broadcast(const std::string& msg) {
    std::lock_guard<std::mutex> lock(clients_mtx_);
    for (auto* c : clients_) {
        if (c) c->send_text(msg.c_str(), msg.size());
    }
}

// Async callback — runs on the event-loop thread; drains queued recovery
// progress snapshots and broadcasts them to every connected client.
void HttpApi::flush_pending(uvcpp::uvcpp_async*) {
    std::vector<std::string> pending;
    {
        std::lock_guard<std::mutex> lock(hub_->mtx);
        pending.swap(hub_->queue);
    }
    for (const auto& m : pending) broadcast(m);
}

// Wake the loop from another thread. uv_stop() (via stop_loop) only sets
// stop_flag and does NOT wake a loop blocked in GetQueuedCompletionStatus, so
// the shutdown path calls this to unblock uv_run() so it can observe the flag.
void HttpApi::wake() {
    if (hub_ && hub_->async) hub_->async->send();
}

void HttpApi::on_ws_command(uvcpp::uvcpp_ws_connection* conn,
                            const std::string& msg) {
    (void)conn;
    try {
        json j = json::parse(msg);
        std::string cmd = j.value("cmd", "");
        std::string job_id = j.value("job_id", "");
        if (cmd == "pause")       restorer_.pause(job_id);
        else if (cmd == "resume") restorer_.resume(job_id);
        else if (cmd == "stop")   restorer_.stop(job_id);
    } catch (...) {
        // ignore malformed frames
    }
}

void HttpApi::on_ws_connection(uvcpp::uvcpp_ws_connection* conn) {
    {
        std::lock_guard<std::mutex> lock(clients_mtx_);
        clients_.insert(conn);
    }
    conn->on_text([this, conn](const std::string& msg) { on_ws_command(conn, msg); });

    // Graceful WS close (CLOSE frame). We only remove the connection here; the
    // TCP close callback below owns the final delete (see comment there).
    conn->on_close([this, conn](uvcpp::ws_close_code, const std::string&) {
        remove_client(conn);
    });

    // Abrupt disconnects (socket EOF / error) do not produce a CLOSE frame, and
    // the library would otherwise auto-delete the TCP client behind our back,
    // leaving `conn` dangling in `clients_`. Override the TCP close callback to
    // (a) drop the connection from the set and (b) delete the ws_connection —
    // safe here because no further callback can reference it once the socket is
    // closed (the WS-level on_close above never runs after this).
    if (uvcpp::uvcpp_tcp_client* tcp = conn->get_tcp_client()) {
        tcp->set_on_close([this, conn]() {
            remove_client(conn);
            delete conn;
        });
    }
}

void HttpApi::setup(uvcpp::uvcpp_http_server& server,
                    uvcpp::uvcpp_static_server* static_svr,
                    uvcpp::uvcpp_ws_server* ws) {
    static_ = static_svr;
    ws_ = ws;

    // Recovery progress: worker threads enqueue a snapshot and poke the async
    // handle; the async callback (flush_pending) broadcasts it on the loop
    // thread, where WS writes are safe. The callback captures `hub_` (not
    // `this`) so an in-flight worker never holds a dangling HttpApi reference.
    hub_ = std::make_shared<Hub>();
    hub_->async = new uvcpp::uvcpp_async();
    hub_->async->init([this](uvcpp::uvcpp_async* a) { flush_pending(a); },
                      server.get_tcp_server()->get_loop());

    restorer_.set_event_callback([hub = hub_](const std::string& msg) {
        {
            std::lock_guard<std::mutex> lock(hub->mtx);
            hub->queue.push_back(msg);
        }
        if (hub->async) hub->async->send();
    });
    scanner_.set_event_callback([hub = hub_](const std::string& msg) {
        {
            std::lock_guard<std::mutex> lock(hub->mtx);
            hub->queue.push_back(msg);
        }
        if (hub->async) hub->async->send();
    });

    if (ws_) {
        ws_->on_connection([this](uvcpp::uvcpp_ws_connection* conn) {
            on_ws_connection(conn);
        });
    }

    // Live system-stats heartbeat: sample the process-wide raw read counter once
    // a second and push {type:"sys", read_mbps, read_bytes, disks:{...}} to every
    // WS client. Besides the disk-throughput gauge it doubles as a WS liveness
    // signal — the read-speed number keeps ticking while a scan runs, so the
    // browser can tell the socket is genuinely receiving server pushes (not just
    // "connected") — and carries per-disk read speeds for the Task-Manager-style
    // chart. Every few ticks it also re-enumerates physical disks and pushes a
    // {type:"disks", disks:[...]} frame when the set changes (hot-plug/removal),
    // so the UI reflects newly attached or pulled disks without a refresh.
    {
        auto* stats_timer = new uvcpp::uvcpp_timer(server.get_tcp_server()->get_loop());
        stats_timer->start([this](uvcpp::uvcpp_timer*) {
            static uint64_t last_bytes = total_bytes_read();
            static std::set<std::string> last_disk_set;
            static int tick = 0;
            static auto last_t = std::chrono::steady_clock::now();

            // Per-disk OS-level I/O counters (bytes read/written + busy time),
            // sampled via IOCTL_DISK_PERFORMANCE so idle disks and writes from
            // other processes are reflected too — like Task Manager.
            struct Perf { int64_t r = 0, w = 0, busy = 0, q = 0; };
            static Perf lastp[32];
            static bool lastp_ok[32] = {false};

            uint64_t b = total_bytes_read();
            auto now = std::chrono::steady_clock::now();
            double dt = std::chrono::duration<double>(now - last_t).count();
            double mbps = dt > 0.0
                ? static_cast<double>(b - last_bytes) / dt / (1024.0 * 1024.0)
                : 0.0;
            last_bytes = b;
            last_t = now;

            auto round1 = [](double v) {
                return static_cast<double>(static_cast<long long>(v * 10.0)) / 10.0;
            };

            json j;
            j["type"] = "sys";
            j["read_mbps"] = round1(mbps);
            j["read_bytes"] = b;

            json disks = json::object();
            for (int i = 0; i < 32; i++) {
                char pbuf[64];
                snprintf(pbuf, sizeof(pbuf), "\\\\.\\PhysicalDrive%d", i);
                HANDLE h = CreateFileA(pbuf, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                       nullptr, OPEN_EXISTING, 0, nullptr);
                if (h == INVALID_HANDLE_VALUE) continue;
                DISK_PERFORMANCE p{};
                DWORD used = 0;
                if (DeviceIoControl(h, IOCTL_DISK_PERFORMANCE, nullptr, 0,
                                    &p, sizeof(p), &used, nullptr)) {
                    int64_t r = p.BytesRead.QuadPart;
                    int64_t w = p.BytesWritten.QuadPart;
                    int64_t busy = p.ReadTime.QuadPart + p.WriteTime.QuadPart;
                    int64_t q = p.QueryTime.QuadPart;
                    if (lastp_ok[i]) {
                        double dts = static_cast<double>(q - lastp[i].q) / 1e7;
                        if (dts > 0.0) {
                            json d;
                            d["read_mbps"]  = round1(static_cast<double>(r - lastp[i].r) / dts / 1e6);
                            d["write_mbps"] = round1(static_cast<double>(w - lastp[i].w) / dts / 1e6);
                            double util = static_cast<double>(busy - lastp[i].busy) /
                                          static_cast<double>(q - lastp[i].q) * 100.0;
                            if (util < 0.0) util = 0.0;
                            if (util > 100.0) util = 100.0;
                            d["util"] = round1(util);
                            disks[std::to_string(i)] = std::move(d);
                        }
                    }
                    lastp[i] = {r, w, busy, q};
                    lastp_ok[i] = true;
                }
                CloseHandle(h);
            }
            j["disks"] = disks;
            broadcast(j.dump());

            // Hot-plug detection (every 3s): if the set of visible physical disks
            // changed, push the fresh list so the UI updates without a refresh.
            // Key on full identity (serial/model/size), not just the slot number,
            // so swapping a *different* disk into the same \\.\PhysicalDriveN slot
            // is detected too — disk_number alone would miss it and the UI would
            // keep showing a stale "connected" status.
            if (++tick % 3 == 0) {
                json list = enumerate_physical_disks();
                std::set<std::string> cur;
                for (const auto& d : list) {
                    cur.insert(std::to_string(d.value("disk_number", -1)) + "|" +
                               d.value("serial", "") + "|" +
                               d.value("model", "") + "|" +
                               std::to_string(d.value("size", static_cast<uint64_t>(0))));
                }
                if (cur != last_disk_set) {
                    last_disk_set = cur;
                    json h;
                    h["type"] = "disks";
                    h["disks"] = list;
                    broadcast(h.dump());
                }
            }
        }, 1000, 1000);
    }

    server.on_request([this, &server](uvcpp::uvcpp_http_request& req,
                                      uvcpp::uvcpp_http_response& resp,
                                      uvcpp::uvcpp_tcp_client* client) {
        // Route: /api/* and /ping go to the JSON handlers; everything else is
        // served as a static file (frontend bundle). OPTIONS preflight for the
        // API is answered with CORS headers.
        std::string p = req.url;
        size_t q = p.find('?');
        if (q != std::string::npos) p = p.substr(0, q);

        bool is_api = (p == "/ping") || (p.rfind("/api/", 0) == 0) ||
                      (p == "/api");
        bool is_opt = (req.method == uvcpp::http_method::HTTP_OPTIONS);

        if (is_api || is_opt) {
            handle_request(req, resp, client);
        } else if (static_) {
            static_->serve(req, resp, client, &server);
        } else {
            set_error(resp, uvcpp::http_status::NOT_FOUND, "unknown endpoint");
        }
    });
}

void HttpApi::handle_request(uvcpp::uvcpp_http_request& req,
                             uvcpp::uvcpp_http_response& resp,
                             uvcpp::uvcpp_tcp_client* client) {
    (void)client;

    std::string url = req.url;
    std::string path = url;
    std::string query;
    size_t q = url.find('?');
    if (q != std::string::npos) {
        path = url.substr(0, q);
        query = url.substr(q + 1);
    }

    bool is_get  = (req.method == uvcpp::http_method::HTTP_GET);
    bool is_post = (req.method == uvcpp::http_method::HTTP_POST);
    bool is_opt  = (req.method == uvcpp::http_method::HTTP_OPTIONS);

    if (is_opt) { set_options(resp); return; }

    auto segs = split_path(path);

    // Health check.
    if (path == "/ping" && is_get) {
        json j;
        j["status"]  = "ok";
        j["name"]    = FSREC_NAME;
        j["version"] = FSREC_VERSION;
        set_json(resp, uvcpp::http_status::OK, j.dump());
        return;
    }

    if (segs.size() >= 2 && segs[0] == "api") {
        const std::string& r = segs[1];

        // GET /api/disks
        if (r == "disks" && segs.size() == 2 && is_get) {
            set_json(resp, uvcpp::http_status::OK, enumerate_disks().dump());
            return;
        }

        // GET /api/physical_disks
        if (r == "physical_disks" && segs.size() == 2 && is_get) {
            set_json(resp, uvcpp::http_status::OK, enumerate_physical_disks().dump());
            return;
        }

        // GET /api/active — the running scan coordinator and the running recover
        // job, so the frontend can restore live progress after a page refresh and
        // still pause/resume/stop the in-flight work.
        if (r == "active" && segs.size() == 2 && is_get) {
            json out;
            out["scan"] = nullptr;
            out["recover"] = nullptr;

            // Prefer the raw_scan_all coordinator (meta) task; fall back to any
            // other running/paused scan.
            std::shared_ptr<ScanTask> run_scan;
            for (const auto& t : scanner_.list_tasks()) {
                std::string st;
                { std::lock_guard<std::mutex> lock(t->mtx); st = t->status; }
                if (st != "running" && st != "paused") continue;
                if (!run_scan || t->meta) run_scan = t;
                if (t->meta) break;
            }
            if (run_scan) {
                json s;
                s["task_id"] = run_scan->id;
                s["progress"] = run_scan->progress.load();
                s["total_files"] = run_scan->total_files.load();
                s["directories"] = run_scan->directories.load();
                s["disk_number"] = run_scan->disk_number;
                s["raw"] = run_scan->raw;
                {
                    std::lock_guard<std::mutex> lock(run_scan->mtx);
                    s["status"] = run_scan->status;
                    s["current_path"] = run_scan->current_path;
                    if (!run_scan->error.empty()) s["error"] = run_scan->error;
                }
                out["scan"] = s;
            }

            std::shared_ptr<RecoverJob> run_job;
            for (const auto& j : restorer_.list_jobs()) {
                std::string st;
                { std::lock_guard<std::mutex> lock(j->mtx); st = j->status; }
                if (st == "running" || st == "paused") { run_job = j; break; }
            }
            if (run_job) {
                json r;
                r["job_id"] = run_job->id;
                r["progress"] = run_job->progress.load();
                r["total_files"] = run_job->total_files.load();
                r["recovered_files"] = run_job->recovered_files.load();
                r["failed_files"] = run_job->failed_files.load();
                r["total_bytes"] = run_job->total_bytes.load();
                r["recovered_bytes"] = run_job->recovered_bytes.load();
                r["output_dir"] = run_job->output_dir;
                {
                    std::lock_guard<std::mutex> lock(run_job->mtx);
                    r["status"] = run_job->status;
                    r["current_file"] = run_job->current_file;
                    r["current_bytes"] = run_job->current_bytes;
                    r["current_size"] = run_job->current_size;
                    if (!run_job->error.empty()) r["error"] = run_job->error;
                }
                out["recover"] = r;
            }
            set_json(resp, uvcpp::http_status::OK, out.dump());
            return;
        }

        // GET /api/debug/find_mft?disk=N&start=..&end=..
        // Read-only diagnostic: locate every surviving $MFT record-0 on a
        // physical disk within [start, end) byte offsets (end=0 → whole disk).
        if (r == "debug" && segs.size() == 3 && segs[2] == "find_mft" && is_get) {
            auto params = parse_query(query);
            int disk = params.count("disk") ? std::atoi(params["disk"].c_str()) : -1;
            if (disk < 0) { set_error(resp, uvcpp::http_status::BAD_REQUEST, "disk is required"); return; }
            uint64_t start = params.count("start") ? strtoull(params["start"].c_str(), nullptr, 10) : 0;
            uint64_t end   = params.count("end")   ? strtoull(params["end"].c_str(), nullptr, 10)   : 0;
            set_json(resp, uvcpp::http_status::OK, scanner_.debug_find_mft(disk, start, end));
            return;
        }

        // GET /api/scans  — one record per scanned physical disk (keyed by
        // serial), each expandable into its per-partition results. Only the last
        // scan per serial survives; re-scanning the same disk replaces it.
        if (r == "scans" && segs.size() == 2 && is_get) {
            json arr = json::array();
            json disks = enumerate_physical_disks();
            for (const auto& s : scanner_.list_sessions()) {
                json j;
                j["key"] = s.key;
                j["serial"] = s.serial;
                j["model"] = s.model;
                j["size"] = s.size;
                j["disk_number"] = s.disk_number;
                j["scanned_at"] = s.scanned_at;
                // Authoritative "still attached" verdict, computed server-side
                // from the disk's recorded identity — the frontend must trust
                // this rather than re-deriving it (or matching by disk number).
                j["connected"] = source_disk_connected(s.disk_number, s.serial,
                                                       s.model, s.size, disks);

                json parts = json::array();
                for (const auto& id : s.partitions) {
                    auto t = scanner_.get_task(id);
                    if (!t) continue;
                    std::string status;
                    { std::lock_guard<std::mutex> lock(t->mtx); status = t->status; }
                    if (status != "completed") continue;
                    json p;
                    p["task_id"] = t->id;
                    p["disk_number"] = t->disk_number;
                    p["persisted"] = t->persisted;
                    p["save_name"] = t->save_name;
                    p["fs"] = fs_type_name(t->fs_type);
                    p["total_files"] = t->total_files.load();
                    p["deleted_files"] = t->deleted_files.load();
                    p["directories"] = t->directories.load();
                    {
                        std::lock_guard<std::mutex> lock(t->mtx);
                        p["mft_offset"] = t->mft_offset;
                        p["cluster_size"] = t->cluster_size;
                        p["volume_start"] = t->volume_start;
                    }
                    parts.push_back(std::move(p));
                }
                j["partitions"] = parts;
                arr.push_back(std::move(j));
            }
            set_json(resp, uvcpp::http_status::OK, arr.dump());
            return;
        }

        // POST /api/scans/delete  {key}  — remove a scan session (its per-
        // partition files + in-memory tasks). The frontend double-confirms.
        if (r == "scans" && segs.size() == 3 && segs[2] == "delete" && is_post) {
            try {
                json body = json::parse(req.body.to_string().empty() ? "{}" : req.body.to_string());
                std::string key = body.value("key", "");
                if (key.empty()) { set_error(resp, uvcpp::http_status::BAD_REQUEST, "key is required"); return; }
                bool ok = scanner_.delete_scan(key);
                if (!ok) { set_error(resp, uvcpp::http_status::NOT_FOUND, "scan session not found"); return; }
                json out; out["ok"] = true;
                set_json(resp, uvcpp::http_status::OK, out.dump());
            } catch (const std::exception& e) {
                set_error(resp, uvcpp::http_status::BAD_REQUEST, std::string("invalid JSON: ") + e.what());
            }
            return;
        }

        // POST /api/shutdown  — graceful shutdown (frontend "退出关闭服务").
        // Respond first, then stop the loop on a detached thread so the reply
        // is flushed before the server tears down.
        if (r == "shutdown" && segs.size() == 2 && is_post) {
            json out; out["ok"] = true;
            set_json(resp, uvcpp::http_status::OK, out.dump());
            if (shutdown_cb_) {
                std::thread([cb = shutdown_cb_]() {
                    std::this_thread::sleep_for(std::chrono::milliseconds(300));
                    cb();
                }).detach();
            }
            return;
        }

        // POST /api/raw_scan  {disk_number, mft_offset?}
        if (r == "raw_scan" && segs.size() == 2 && is_post) {
            try {
                json body = json::parse(req.body.to_string().empty() ? "{}" : req.body.to_string());
                int disk = body.value("disk_number", -1);
                uint64_t hint = body.value("mft_offset", static_cast<uint64_t>(0));
                if (disk < 0) { set_error(resp, uvcpp::http_status::BAD_REQUEST, "disk_number is required"); return; }
                std::string id = scanner_.start_raw_scan(disk, hint);
                json out; out["task_id"] = id;
                set_json(resp, uvcpp::http_status::OK, out.dump());
            } catch (const std::exception& e) {
                set_error(resp, uvcpp::http_status::BAD_REQUEST, std::string("invalid JSON: ") + e.what());
            }
            return;
        }

        // POST /api/raw_scan_all  {disk_number}  — scan every NTFS volume on the
        // disk (multi-partition). Returns a coordinator task id; poll it, then
        // reload /api/scans to see one entry per discovered volume.
        if (r == "raw_scan_all" && segs.size() == 2 && is_post) {
            try {
                json body = json::parse(req.body.to_string().empty() ? "{}" : req.body.to_string());
                int disk = body.value("disk_number", -1);
                if (disk < 0) { set_error(resp, uvcpp::http_status::BAD_REQUEST, "disk_number is required"); return; }
                std::string id = scanner_.start_raw_scan_all(disk);
                json out; out["task_id"] = id;
                set_json(resp, uvcpp::http_status::OK, out.dump());
            } catch (const std::exception& e) {
                set_error(resp, uvcpp::http_status::BAD_REQUEST, std::string("invalid JSON: ") + e.what());
            }
            return;
        }

        // POST /api/scan
        if (r == "scan" && segs.size() == 2 && is_post) {
            try {
                json body = json::parse(req.body.to_string().empty() ? "{}" : req.body.to_string());
                std::string drive = body.value("drive", "");
                std::string mode  = body.value("scan_mode", "quick");
                if (drive.empty()) { set_error(resp, uvcpp::http_status::BAD_REQUEST, "drive is required"); return; }
                std::string id = scanner_.start_scan(drive, mode);
                json out; out["task_id"] = id;
                set_json(resp, uvcpp::http_status::OK, out.dump());
            } catch (const std::exception& e) {
                set_error(resp, uvcpp::http_status::BAD_REQUEST, std::string("invalid JSON: ") + e.what());
            }
            return;
        }

        // GET /api/scan/{id}/status
        if (r == "scan" && segs.size() == 4 && segs[3] == "status" && is_get) {
            auto t = scanner_.get_task(segs[2]);
            if (!t) { set_error(resp, uvcpp::http_status::NOT_FOUND, "scan task not found"); return; }
            json out;
            out["task_id"] = t->id;
            out["progress"] = t->progress.load();
            out["total_files"] = t->total_files.load();
            out["deleted_files"] = t->deleted_files.load();
            out["directories"] = t->directories.load();
            out["raw"] = t->raw;
            out["disk_number"] = t->disk_number;
            out["fs"] = fs_type_name(t->fs_type);
            {
                std::lock_guard<std::mutex> lock(t->mtx);
                out["status"] = t->status;
                if (!t->error.empty()) out["error"] = t->error;
                out["current_path"] = t->current_path;
                out["mft_offset"] = t->mft_offset;
                out["cluster_size"] = t->cluster_size;
                out["volume_start"] = t->volume_start;
                out["mft_records_total"] = t->mft_records_total;
                json cands = json::array();
                for (const auto& c : t->candidates) {
                    json jc;
                    jc["offset"] = c.offset;
                    jc["record_count"] = c.record_count;
                    cands.push_back(jc);
                }
                out["candidates"] = cands;
            }
            set_json(resp, uvcpp::http_status::OK, out.dump());
            return;
        }

        // GET /api/scan/{id}/results
        if (r == "scan" && segs.size() == 4 && segs[3] == "results" && is_get) {
            auto t = scanner_.get_task(segs[2]);
            if (!t) { set_error(resp, uvcpp::http_status::NOT_FOUND, "scan task not found"); return; }
            FileNodePtr root;
            std::string status;
            {
                std::lock_guard<std::mutex> lock(t->mtx);
                status = t->status;
                root = t->root;
            }
            if (status != "completed" || !root) {
                set_error(resp, uvcpp::http_status::CONFLICT, "scan not completed yet");
                return;
            }
            set_json(resp, uvcpp::http_status::OK, node_to_json(root, true, 0, -1).dump());
            return;
        }

        // POST /api/scan/{id}/pause | /resume | /stop
        if (r == "scan" && segs.size() == 4 && is_post &&
            (segs[3] == "pause" || segs[3] == "resume" || segs[3] == "stop")) {
            bool ok = false;
            if (segs[3] == "pause")       ok = scanner_.pause(segs[2]);
            else if (segs[3] == "resume") ok = scanner_.resume(segs[2]);
            else                          ok = scanner_.stop(segs[2]);
            if (!ok) { set_error(resp, uvcpp::http_status::NOT_FOUND, "scan task not found or already finished"); return; }
            json out; out["ok"] = true;
            set_json(resp, uvcpp::http_status::OK, out.dump());
            return;
        }

        // GET /api/search?task_id=..&q=..&type=..
        if (r == "search" && segs.size() == 2 && is_get) {
            auto params = parse_query(query);
            auto t = scanner_.get_task(params["task_id"]);
            if (!t) { set_error(resp, uvcpp::http_status::NOT_FOUND, "scan task not found"); return; }

            std::string keyword = params.count("q") ? params["q"] : "";
            std::string type    = params.count("type") ? params["type"] : "all";
            if (keyword.empty()) { set_json(resp, uvcpp::http_status::OK, "[]"); return; }

            FileNodePtr root;
            {
                std::lock_guard<std::mutex> lock(t->mtx);
                root = t->root;
            }
            if (!root) { set_error(resp, uvcpp::http_status::CONFLICT, "scan not completed yet"); return; }

            json results = json::array();
            std::string kw = to_lower(keyword);
            std::function<void(const FileNodePtr&, const std::string&)> dfs;
            dfs = [&](const FileNodePtr& n, const std::string& prefix) {
                std::string name = utf16_to_utf8(n->name);
                std::string full = prefix + "/" + name;
                bool is_dir = n->is_directory;
                bool type_ok = (type == "all" || (type == "file" && !is_dir) ||
                                (type == "folder" && is_dir));
                if (type_ok && to_lower(name).find(kw) != std::string::npos) {
                    json r;
                    r["path"] = full;
                    r["name"] = name;
                    r["type"] = is_dir ? "folder" : "file";
                    r["mft_id"] = n->mft_id;
                    r["is_deleted"] = n->is_deleted;
                    r["size"] = n->size;
                    results.push_back(r);
                }
                for (const auto& c : n->children) dfs(c, full);
            };
            for (const auto& c : root->children) dfs(c, "");
            set_json(resp, uvcpp::http_status::OK, results.dump());
            return;
        }

        // POST /api/recover
        if (r == "recover" && segs.size() == 2 && is_post) {
            try {
                json body = json::parse(req.body.to_string().empty() ? "{}" : req.body.to_string());
                std::string task_id = body.value("task_id", "");
                std::string output_dir = body.value("output_dir", "");
                std::vector<std::string> paths;
                if (body.contains("selected_paths") && body["selected_paths"].is_array()) {
                    for (const auto& p : body["selected_paths"]) paths.push_back(p.get<std::string>());
                }
                if (task_id.empty()) { set_error(resp, uvcpp::http_status::BAD_REQUEST, "task_id is required"); return; }
                if (output_dir.empty()) { set_error(resp, uvcpp::http_status::BAD_REQUEST, "output_dir is required"); return; }
                if (paths.empty()) { set_error(resp, uvcpp::http_status::BAD_REQUEST, "selected_paths is required"); return; }

                auto task = scanner_.get_task(task_id);
                if (!task) { set_error(resp, uvcpp::http_status::NOT_FOUND, "scan task not found"); return; }

                // Safety: the source disk must still be the SAME physical disk
                // that was scanned. A reused \\.\PhysicalDriveN slot (original
                // disk unplugged and a different one now occupying that number)
                // must never recover the wrong disk's data — validate by serial,
                // falling back to model+size. No usable identity → refuse (safe).
                if (task->raw && task->disk_number >= 0) {
                    json disks = enumerate_physical_disks();
                    if (!source_disk_connected(task->disk_number, task->serial,
                                               task->model, task->disk_size, disks)) {
                        set_error(resp, uvcpp::http_status::CONFLICT,
                                  "源磁盘已断开连接或已被其他磁盘替换，已禁止恢复；请重新挂载原磁盘后重试。");
                        return;
                    }
                }

                // Safety: refuse a destination that lives on the source disk.
                {
                    size_t i = 0;
                    while (i < output_dir.size() &&
                           std::isspace(static_cast<unsigned char>(output_dir[i]))) i++;
                    std::string letter;
                    if (i + 1 < output_dir.size() &&
                        std::isalpha(static_cast<unsigned char>(output_dir[i])) &&
                        output_dir[i + 1] == ':')
                        letter = std::string(1, static_cast<char>(toupper(static_cast<unsigned char>(output_dir[i])))) + ":";
                    if (!letter.empty()) {
                        auto out_disks = volume_disk_numbers(letter);
                        if (task->raw &&
                            std::find(out_disks.begin(), out_disks.end(), task->disk_number) != out_disks.end()) {
                            set_error(resp, uvcpp::http_status::BAD_REQUEST,
                                      "output directory must not be on the source disk");
                            return;
                        }
                    }
                }

                std::string job_id = restorer_.start_recover(task, paths, output_dir);
                json out; out["job_id"] = job_id;
                set_json(resp, uvcpp::http_status::OK, out.dump());
            } catch (const std::exception& e) {
                set_error(resp, uvcpp::http_status::BAD_REQUEST, std::string("invalid JSON: ") + e.what());
            }
            return;
        }

        // GET /api/recover/{id}/status
        if (r == "recover" && segs.size() == 4 && segs[3] == "status" && is_get) {
            auto job = restorer_.get_job(segs[2]);
            if (!job) { set_error(resp, uvcpp::http_status::NOT_FOUND, "recover job not found"); return; }
            json out;
            out["job_id"] = job->id;
            out["progress"] = job->progress.load();
            out["total_files"] = job->total_files.load();
            out["recovered_files"] = job->recovered_files.load();
            out["failed_files"] = job->failed_files.load();
            out["total_bytes"] = job->total_bytes.load();
            out["recovered_bytes"] = job->recovered_bytes.load();
            {
                std::lock_guard<std::mutex> lock(job->mtx);
                out["status"] = job->status;
                if (!job->error.empty()) out["error"] = job->error;
                out["current_file"] = job->current_file;
                out["current_bytes"] = job->current_bytes;
                out["current_size"] = job->current_size;
            }
            set_json(resp, uvcpp::http_status::OK, out.dump());
            return;
        }

        // POST /api/recover/{id}/pause | /resume | /stop
        if (r == "recover" && segs.size() == 4 && is_post &&
            (segs[3] == "pause" || segs[3] == "resume" || segs[3] == "stop")) {
            bool ok = false;
            if (segs[3] == "pause")       ok = restorer_.pause(segs[2]);
            else if (segs[3] == "resume") ok = restorer_.resume(segs[2]);
            else                          ok = restorer_.stop(segs[2]);
            if (!ok) { set_error(resp, uvcpp::http_status::NOT_FOUND, "recover job not found or already finished"); return; }
            json out; out["ok"] = true;
            set_json(resp, uvcpp::http_status::OK, out.dump());
            return;
        }

        // ---- Filesystem browser for the recovery destination picker ---------

        // GET /api/fs/drives — drives with physical-disk mapping + free space.
        if (r == "fs" && segs.size() == 3 && segs[2] == "drives" && is_get) {
            set_json(resp, uvcpp::http_status::OK, enumerate_disks().dump());
            return;
        }

        // GET /api/fs/list?path=C:\foo — subdirectories of `path`.
        if (r == "fs" && segs.size() == 3 && segs[2] == "list" && is_get) {
            auto params = parse_query(query);
            std::string path = params.count("path") ? params["path"] : "";
            if (path.empty()) { set_error(resp, uvcpp::http_status::BAD_REQUEST, "path is required"); return; }
            set_json(resp, uvcpp::http_status::OK, list_directories(path).dump());
            return;
        }

        // POST /api/fs/mkdir {path, name} — create a folder.
        if (r == "fs" && segs.size() == 3 && segs[2] == "mkdir" && is_post) {
            try {
                json body = json::parse(req.body.to_string().empty() ? "{}" : req.body.to_string());
                std::string path = body.value("path", "");
                std::string name = body.value("name", "");
                if (path.empty() || name.empty()) {
                    set_error(resp, uvcpp::http_status::BAD_REQUEST, "path and name are required");
                    return;
                }
                // Reject separators / traversal in the folder name.
                if (name.find_first_of("\\/:*?\"<>|") != std::string::npos) {
                    set_error(resp, uvcpp::http_status::BAD_REQUEST, "invalid folder name");
                    return;
                }
                fs::path dir = fs::u8path(path) / fs::u8path(name);
                std::error_code ec;
                fs::create_directory(dir, ec);
                if (ec) { set_error(resp, uvcpp::http_status::BAD_REQUEST, "cannot create folder: " + ec.message()); return; }
                json out;
                out["ok"] = true;
                out["path"] = wtoa(dir.wstring());
                set_json(resp, uvcpp::http_status::OK, out.dump());
            } catch (const std::exception& e) {
                set_error(resp, uvcpp::http_status::BAD_REQUEST, std::string("invalid JSON: ") + e.what());
            }
            return;
        }

        // GET /api/fs/space?path=C:\foo — free/total bytes on that volume.
        if (r == "fs" && segs.size() == 3 && segs[2] == "space" && is_get) {
            auto params = parse_query(query);
            std::string path = params.count("path") ? params["path"] : "";
            if (path.empty()) { set_error(resp, uvcpp::http_status::BAD_REQUEST, "path is required"); return; }
            set_json(resp, uvcpp::http_status::OK, space_info(path).dump());
            return;
        }
    }

    set_error(resp, uvcpp::http_status::NOT_FOUND, "unknown endpoint");
}

} // namespace recovery
