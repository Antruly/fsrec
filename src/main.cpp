#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <thread>

#include <windows.h>
#include <shellapi.h>

#include <web/uvcpp_http_server.h>
#include <web/uvcpp_static_server.h>
#include <web/uvcpp_ws_server.h>

#include "http/server.h"
#include "include/version.h"
#include "include/logger.h"
#include "recovery/restorer.h"
#include "recovery/scanner.h"

namespace {

// Pointers used by the console control handler and the HTTP shutdown hook to
// stop workers + the libuv loop from any thread. They are set once in main()
// before any shutdown path can fire.
static recovery::Scanner*   g_scanner  = nullptr;
static recovery::Restorer*  g_restorer = nullptr;
static uvcpp::uvcpp_http_server* g_server = nullptr;
static std::atomic<bool> g_shutting_down{false};
static std::function<void()> g_wake_loop;   // pokes the libuv loop (uv_async_send)
static std::function<void()> g_stop_stats;  // stops the background stats poller

// Last-resort shutdown watchdog, armed ONCE at startup. Every shutdown path
// (console Ctrl handler, HTTP "退出" button, …) sets g_shutting_down; if the
// process is somehow still alive 5 seconds after that — e.g. a worker thread or
// the stats poller stuck in a blocking syscall, or a log write blocked on a
// console that is already closing — force-exit so the console window can never
// hang open. Arming it here (rather than inside each shutdown path) means it
// protects even a path that blocks before it could arm its own timer.
// Detached: killed harmlessly when a normal exit wins.
void arm_shutdown_watchdog() {
    std::thread([]() {
        for (;;) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (g_shutting_down.load()) {
                std::this_thread::sleep_for(std::chrono::seconds(5));
                // Force-exit immediately. Deliberately NO stdio call before
                // ExitProcess: during teardown the CRT stream locks can be held
                // by a thread stuck in a destructor, so fprintf/fflush here could
                // deadlock and never reach ExitProcess — leaving the console open.
                ExitProcess(0);
            }
        }
    }).detach();
}

// Handle Ctrl+C, window close (X), logoff and shutdown. We take over the close
// so the process can stop its worker threads and the event loop gracefully,
// then let run() return normally — instead of the OS killing the process and
// leaving it in a half-dead state that requires Task Manager.
BOOL WINAPI console_ctrl_handler(DWORD ctrl_type) {
    switch (ctrl_type) {
    case CTRL_C_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_SHUTDOWN_EVENT:
    case CTRL_LOGOFF_EVENT:
        if (!g_shutting_down.exchange(true)) {
            FSLOG_WARN("正在安全关闭服务，请稍候…");
            if (g_stop_stats) g_stop_stats();
            if (g_scanner)  g_scanner->shutdown();
            if (g_restorer) g_restorer->shutdown();
            if (g_server)   g_server->get_tcp_server()->stop_loop();
            if (g_wake_loop) g_wake_loop(); // unblock uv_run so the process exits
        }
        return TRUE; // handled — prevent the default terminate
    default:
        return FALSE;
    }
}

// Resolve the project root from the executable path. Supports two layouts:
//   installed:  <app>\recovery_server.exe                (frontend is a sibling)
//   dev build:  <root>\build\Release\recovery_server.exe  (root is 3 levels up)
// Walk up from the exe's directory until we find frontend\dist\index.html, so
// the result is independent of the current working directory. Falls back to
// the exe's own directory if the frontend can't be located.
std::string app_root_dir() {
    char buf[MAX_PATH] = {0};
    if (GetModuleFileNameA(nullptr, buf, MAX_PATH) == 0)
        return ".";
    std::filesystem::path dir = std::filesystem::path(buf).parent_path();
    for (int i = 0; i < 8; ++i) {
        std::error_code ec;
        if (std::filesystem::exists(dir / "frontend" / "dist" / "index.html", ec))
            return dir.string();
        std::filesystem::path parent = dir.parent_path();
        if (parent == dir) break;
        dir = parent;
    }
    return std::filesystem::path(buf).parent_path().string();
}

bool is_admin() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        return false;

    // Under UAC, TokenElevation tells us whether this process is actually
    // elevated — not merely that the user account belongs to the
    // Administrators group (a filtered, non-elevated admin token reports
    // TokenIsElevated == 0).
    TOKEN_ELEVATION elev{};
    DWORD len = 0;
    if (GetTokenInformation(token, TokenElevation, &elev, sizeof(elev), &len)) {
        CloseHandle(token);
        return elev.TokenIsElevated != 0;
    }

    // UAC disabled: TokenElevation is unsupported. Fall back to checking
    // whether the Administrators SID is enabled in the token.
    BOOL isMember = FALSE;
    SID_IDENTIFIER_AUTHORITY ntAuth = SECURITY_NT_AUTHORITY;
    PSID adminGroup = nullptr;
    if (AllocateAndInitializeSid(&ntAuth, 2, SECURITY_BUILTIN_DOMAIN_RID,
                                  DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0,
                                  &adminGroup)) {
        CheckTokenMembership(token, adminGroup, &isMember);
        FreeSid(adminGroup);
    }
    CloseHandle(token);
    return isMember != FALSE;
}

} // namespace

int main(int argc, char** argv) {
    // Print UTF-8 text (e.g. the Chinese status messages below) correctly on a
    // GBK (codepage 936) console: tell the console to interpret our output
    // bytes as UTF-8 instead of mangling them.
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    // Optional CLI flags:
    //   --port <n>    listen port (default 8080)
    //   --host <ip>   listen address (default 127.0.0.1 = loopback only)
    //   --no-browser  don't auto-open the web UI in the default browser
    int port = 8080;
    std::string host = "127.0.0.1";
    bool open_browser = true;
    bool port_overridden = false;
    bool host_overridden = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--no-browser") open_browser = false;
        else if (a == "--port" && i + 1 < argc) { port = std::atoi(argv[++i]); port_overridden = true; }
        else if (a == "--host" && i + 1 < argc) { host = argv[++i]; host_overridden = true; }
    }

    // Raw volume access requires an elevated (Run as administrator) process.
    if (!is_admin()) {
        std::fprintf(stderr,
                     "[recovery_server] ERROR: not running with elevated privileges.\n"
                     "Raw disk access requires an elevated process. Your account may be in the\n"
                     "Administrators group, but this console window is NOT elevated (UAC).\n"
                     "Start PowerShell as Administrator, or run:\n"
                     "    Start-Process powershell -Verb RunAs\n"
                     "then launch recovery_server.exe from the elevated window.\n");
        return 1;
    }

    std::string root = app_root_dir();
    std::string static_dir = root + "\\frontend\\dist";
    std::string data_dir = root + "\\data";

    // Read the configured service port from port.txt (written by the installer),
    // so the installed service listens on the port the user chose during setup.
    // An explicit --port on the command line always wins.
    if (!port_overridden) {
        std::ifstream pf(root + "\\port.txt");
        int p = 0;
        if (pf >> p && p > 0 && p < 65536)
            port = p;
    }

    // Read the configured listen address from host.txt (written by the
    // installer), so the service binds to the address chosen during setup.
    // Default is 127.0.0.1 (loopback only) — the web UI stays local unless the
    // user explicitly opts into 0.0.0.0 (all interfaces) or a specific IP.
    // An explicit --host on the command line always wins.
    if (!host_overridden) {
        std::ifstream hf(root + "\\host.txt");
        std::string h;
        if (std::getline(hf, h)) {
            while (!h.empty() && (h.back() == '\r' || h.back() == '\n' ||
                                  h.back() == ' ' || h.back() == '\t'))
                h.pop_back();
            if (!h.empty()) host = h;
        }
    }

    // Make sure the data dir exists so the first scan can be persisted.
    {
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::u8path(data_dir), ec);
    }

    recovery::Scanner scanner;
    scanner.load_saved(data_dir);
    recovery::Restorer restorer;
    recovery::HttpApi api(scanner, restorer);

    uvcpp::uvcpp_static_server static_svr(static_dir, "/", "index.html");
    uvcpp::uvcpp_http_server server;
    uvcpp::uvcpp_ws_server ws;
    ws.attach(&server);
    api.setup(server, &static_svr, &ws);

    // Wire graceful shutdown: the console close handler above, plus the HTTP
    // "退出关闭服务" button (POST /api/shutdown). Both stop the workers and then
    // the libuv loop so run() returns and the process exits cleanly.
    g_scanner = &scanner;
    g_restorer = &restorer;
    g_server = &server;
    g_wake_loop = [&api]() { api.wake(); };
    g_stop_stats = [&api]() { api.stop_stats(); };
    SetConsoleCtrlHandler(console_ctrl_handler, TRUE);
    api.set_shutdown_callback([&]() {
        if (!g_shutting_down.exchange(true)) {
            FSLOG_WARN("正在安全关闭服务…");
            api.stop_stats();
            scanner.shutdown();
            restorer.shutdown();
            server.get_tcp_server()->stop_loop();
            api.wake(); // unblock uv_run so the process exits and closes the console
        }
    });

    const char* ip = host.c_str();
    if (server.bind(ip, port) != 0) {
        std::fprintf(stderr, "[recovery_server] ERROR: failed to bind %s:%d\n", ip, port);
        return 1;
    }
    if (server.listen() != 0) {
        std::fprintf(stderr, "[recovery_server] ERROR: failed to listen\n");
        return 1;
    }

    // For the auto-opened browser and the friendly URL, use loopback whenever the
    // server listens on a wildcard/loopback address; otherwise the literal IP.
    bool loopback = (host == "0.0.0.0" || host == "127.0.0.1" ||
                     host == "::" || host == "::1" || host == "localhost");
    std::string url = "http://" + std::string(loopback ? "localhost" : host) +
                      ":" + std::to_string(port);

    // Initialize the console/file logger. The dashboard banner pins the version
    // and the web UI URL at the top (so they're never scrolled away), and every
    // entry is also appended to a daily file under <root>\logs\fsrec_YYYY-MM-DD.log.
    fslog::Logger::instance().init(root + "\\logs", FSREC_VERSION, url);

    FSLOG_INFO("服务已启动，监听 %s:%d", ip, port);
    FSLOG_INFO("静态目录 %s", static_dir.c_str());
    FSLOG_INFO("数据目录 %s", data_dir.c_str());
    if (host == "0.0.0.0" || host == "::") {
        FSLOG_WARN("正监听在所有网络接口（%s），网页界面可能被局域网内其他主机访问", ip);
    }

    if (open_browser) {
        // Open the web UI in the default browser. Best-effort: return values
        // > 32 indicate success; <= 32 is an error we deliberately ignore.
        ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }

    // Arm the last-resort watchdog so that no matter which shutdown path fires
    // (Ctrl handler, HTTP "退出" button), the process is guaranteed to exit.
    arm_shutdown_watchdog();

    server.run();

    // Graceful teardown. The shutdown callback / Ctrl handler has already asked
    // the workers to stop and stopped the event loop, so run() has returned.
    // Join the workers now so an in-progress scan/recovery can wind down, then
    // exit the process directly — bypassing the C++ destructors, two of which
    // can hang the console window for many seconds:
    //   • ~HttpApi() joins the stats poller, which may be blocked inside a
    //     synchronous disk call (CreateFileA / DeviceIoControl on a stalled USB
    //     bridge or card reader);
    //   • ~uvcpp_tcp_server() pumps a loop that still holds the stats timer,
    //     the WebSocket async handle and any live clients.
    // The watchdog above remains the final backstop if a worker thread itself
    // is stuck on a stalled source/target disk.
    scanner.shutdown();
    restorer.shutdown();
    scanner.join_workers();
    restorer.join_workers();

    fslog::Logger::instance().shutdown();
    ExitProcess(0);
}
