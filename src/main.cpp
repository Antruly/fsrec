#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include <windows.h>
#include <shellapi.h>

#include <web/uvcpp_http_server.h>
#include <web/uvcpp_static_server.h>
#include <web/uvcpp_ws_server.h>

#include "http/server.h"
#include "include/version.h"
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
            std::fprintf(stderr, "[fsrec] 正在安全关闭服务，请稍候…\n");
            if (g_scanner)  g_scanner->shutdown();
            if (g_restorer) g_restorer->shutdown();
            if (g_server)   g_server->get_tcp_server()->stop_loop();
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
    // Optional CLI flags: --port <n> (default 8080), --no-browser (don't
    // auto-open the web UI in the default browser on startup).
    int port = 8080;
    bool open_browser = true;
    bool port_overridden = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--no-browser") open_browser = false;
        else if (a == "--port" && i + 1 < argc) { port = std::atoi(argv[++i]); port_overridden = true; }
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
    SetConsoleCtrlHandler(console_ctrl_handler, TRUE);
    api.set_shutdown_callback([&]() {
        if (!g_shutting_down.exchange(true)) {
            std::fprintf(stderr, "[fsrec] 正在安全关闭服务…\n");
            scanner.shutdown();
            restorer.shutdown();
            server.get_tcp_server()->stop_loop();
        }
    });

    const char* ip = "0.0.0.0";
    if (server.bind(ip, port) != 0) {
        std::fprintf(stderr, "[recovery_server] ERROR: failed to bind %s:%d\n", ip, port);
        return 1;
    }
    if (server.listen() != 0) {
        std::fprintf(stderr, "[recovery_server] ERROR: failed to listen\n");
        return 1;
    }

    std::string url = "http://localhost:" + std::to_string(port);
    std::printf("[%s] v%s listening on %s\n", FSREC_NAME, FSREC_VERSION, url.c_str());
    std::printf("[%s] health check: %s/ping\n", FSREC_NAME, url.c_str());
    std::printf("[%s] frontend:     %s/\n", FSREC_NAME, url.c_str());
    std::printf("[%s] static dir:   %s\n", FSREC_NAME, static_dir.c_str());
    std::printf("[%s] data dir:     %s\n", FSREC_NAME, data_dir.c_str());
    std::fflush(stdout);

    if (open_browser) {
        // Open the web UI in the default browser. Best-effort: return values
        // > 32 indicate success; <= 32 is an error we deliberately ignore.
        ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }

    server.run();
    return 0;
}
