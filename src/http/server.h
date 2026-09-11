#pragma once
// HTTP API layer: wires the libuvcpp HTTP + WebSocket servers to the recovery
// services. Recovery progress is pushed to connected WebSocket clients via an
// async handle that marshals events from worker threads onto the loop thread.

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>

#include <web/uvcpp_http_server.h>
#include <web/uvcpp_static_server.h>
#include <web/uvcpp_ws_server.h>
#include <web/uvcpp_ws_connection.h>
#include <handle/uvcpp_async.h>

#include "../recovery/scanner.h"
#include "../recovery/restorer.h"

namespace recovery {

class HttpApi {
public:
    HttpApi(Scanner& scanner, Restorer& restorer)
        : scanner_(scanner), restorer_(restorer) {}
    ~HttpApi();

    // Register the catch-all request handler and the WebSocket connection
    // handler. `static_svr`, when non-null, serves the bundled frontend for any
    // request that is not an API route. `ws`, when non-null, upgrades WS
    // connections on `server` (it must already be attached).
    void setup(uvcpp::uvcpp_http_server& server,
               uvcpp::uvcpp_static_server* static_svr,
               uvcpp::uvcpp_ws_server* ws);

    // Register a callback that stops the whole service (scanner + loop). It is
    // invoked by POST /api/shutdown on a detached thread, after the HTTP
    // response has flushed, so the client reliably receives {ok:true} first.
    void set_shutdown_callback(std::function<void()> cb) {
        shutdown_cb_ = std::move(cb);
    }

    // Wake the event loop from another thread (uv_async_send). The shutdown
    // path calls this after stop_loop(): uv_stop() only sets a flag and does
    // NOT wake a loop blocked in GetQueuedCompletionStatus, so without this the
    // process never leaves uv_run() and the console window stays open.
    void wake();

    // Ask the background stats poller to stop so it can be joined without
    // blocking shutdown. Idempotent and safe to call from any thread.
    void stop_stats();

private:
    void handle_request(uvcpp::uvcpp_http_request& req,
                        uvcpp::uvcpp_http_response& resp,
                        uvcpp::uvcpp_tcp_client* client);

    // --- WebSocket hub ------------------------------------------------------
    struct Hub;  // queue + async handle; shared with the recovery callback so a
                 // worker outlives this object without a dangling reference.

    void on_ws_connection(uvcpp::uvcpp_ws_connection* conn);
    void on_ws_command(uvcpp::uvcpp_ws_connection* conn, const std::string& msg);
    void remove_client(uvcpp::uvcpp_ws_connection* conn);
    void broadcast(const std::string& msg);   // loop thread only
    void flush_pending(uvcpp::uvcpp_async*);  // async callback (loop thread)

    // Background stats poller. The per-second disk sampling (CreateFileA +
    // IOCTL_DISK_PERFORMANCE + hot-plug enumeration) is synchronous and can
    // block on a slow/stalled disk, so it must NOT run on the event-loop
    // thread. The poller thread fills these mutex-guarded frames; the loop
    // timer merely swaps them out and broadcasts.
    void start_stats_poller();

    Scanner&  scanner_;
    Restorer& restorer_;
    uvcpp::uvcpp_static_server* static_ = nullptr;
    uvcpp::uvcpp_ws_server*     ws_     = nullptr;

    std::function<void()> shutdown_cb_;

    std::shared_ptr<Hub> hub_;
    std::mutex clients_mtx_;
    std::set<uvcpp::uvcpp_ws_connection*> clients_;

    std::thread       stats_thread_;
    std::atomic<bool> stats_stop_{false};
    std::mutex        stats_mtx_;
    std::string       sys_frame_;    // latest {type:"sys"} JSON (guarded by stats_mtx_)
    std::string       disks_frame_;  // latest {type:"disks"} JSON, only on change
};

} // namespace recovery
