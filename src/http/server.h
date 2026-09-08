#pragma once
// HTTP API layer: wires the libuvcpp HTTP + WebSocket servers to the recovery
// services. Recovery progress is pushed to connected WebSocket clients via an
// async handle that marshals events from worker threads onto the loop thread.

#include <memory>
#include <mutex>
#include <set>
#include <string>

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

    // Register the catch-all request handler and the WebSocket connection
    // handler. `static_svr`, when non-null, serves the bundled frontend for any
    // request that is not an API route. `ws`, when non-null, upgrades WS
    // connections on `server` (it must already be attached).
    void setup(uvcpp::uvcpp_http_server& server,
               uvcpp::uvcpp_static_server* static_svr,
               uvcpp::uvcpp_ws_server* ws);

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

    Scanner&  scanner_;
    Restorer& restorer_;
    uvcpp::uvcpp_static_server* static_ = nullptr;
    uvcpp::uvcpp_ws_server*     ws_     = nullptr;

    std::shared_ptr<Hub> hub_;
    std::mutex clients_mtx_;
    std::set<uvcpp::uvcpp_ws_connection*> clients_;
};

} // namespace recovery
