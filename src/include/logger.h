#pragma once
// Thread-safe console + file logger for fsrec.
//
// - Leveled, color-coded console output (INFO / SUCCESS / WARN / ERROR; DEBUG is
//   file-only so the console stays clean).
// - A pinned dashboard header (usage instructions + web UI URL) drawn above a
//   scrolling log region, so the access address is never scrolled away.
// - Daily-rotating log files under <log_dir>\fsrec_YYYY-MM-DD.log (every level).
//
// When stdout is not a real console (e.g. `recovery_server.exe > out.txt` or a
// service manager), the dashboard is disabled and logs fall back to plain,
// uncolored line output on stdout — the file sink still records everything.
//
// Header-only (all members inline) so it needs no CMake source-list change and
// can be included from any translation unit without ODR problems.

#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#include <windows.h>

namespace fslog {

enum class Level { Debug = 0, Info = 1, Success = 2, Warn = 3, Error = 4 };

class Logger {
public:
    static Logger& instance() {
        static Logger l;
        return l;
    }

    // Configure the log directory, create it if needed, and attach the console
    // dashboard (only when stdout is an interactive console). `version` and
    // `url` feed the pinned banner. Call once from the main thread at startup.
    void init(const std::string& log_dir, const std::string& version,
              const std::string& url) {
        std::lock_guard<std::mutex> lock(mutex_);
        log_dir_ = log_dir;
        version_ = version;
        url_ = url;
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::u8path(log_dir), ec);
        ensure_file_locked();

        hOut_ = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD mode = 0;
        if (hOut_ != INVALID_HANDLE_VALUE && hOut_ != nullptr &&
            GetConsoleMode(hOut_, &mode)) {
            CONSOLE_SCREEN_BUFFER_INFO csbi{};
            if (GetConsoleScreenBufferInfo(hOut_, &csbi)) {
                // Hide the caret so the repaint never shows a flickering block.
                CONSOLE_CURSOR_INFO ci{1, FALSE};
                SetConsoleCursorInfo(hOut_, &ci);
                console_ = true;
            }
        }
    }

    // Restore the console (caret + default colors), print a final line, and
    // flush/close the log file. Safe to call once, from the main thread.
    void shutdown() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (console_) {
            CONSOLE_SCREEN_BUFFER_INFO csbi{};
            if (GetConsoleScreenBufferInfo(hOut_, &csbi)) {
                SHORT rows = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
                SetConsoleCursorPosition(hOut_, {0, static_cast<SHORT>(rows - 1)});
            }
            SetConsoleTextAttribute(hOut_, FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
            CONSOLE_CURSOR_INFO ci{1, TRUE};
            SetConsoleCursorInfo(hOut_, &ci);
        }
        if (!log_path_.empty() && console_)
            std::fprintf(stdout, "\n[fsrec] 服务已停止，操作日志已保存至 %s\n", log_path_.c_str());
        if (file_.is_open()) file_.flush();
        file_.close();
        console_ = false;
    }

    // printf-style logging (e.g. FSLOG_INFO("磁盘 %d", n)).
    void log(Level lv, const char* fmt, ...) {
        char buf[1024];
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        log(lv, std::string(buf));
    }

    // Core logging path: timestamp + level tag + message, then file + console.
    void log(Level lv, const std::string& msg) {
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_file_locked();

        std::string line = timestamp_locked() + " " + tag(lv) + " " + msg;

        // File sink (plain text, always): every level is recorded.
        if (file_.is_open()) {
            file_ << line << "\n";
            file_.flush();
        }

        // Console / stdout: only Info and above. Debug is file-only so the
        // dashboard stays clean while the daily file keeps the full record.
        if (lv < Level::Info)
            return;

        if (console_) {
            // Dashboard: append to the on-screen ring and repaint the log region.
            ring_.push_back({lv, msg});
            int log_rows = 0;
            {
                CONSOLE_SCREEN_BUFFER_INFO csbi{};
                if (GetConsoleScreenBufferInfo(hOut_, &csbi))
                    log_rows = csbi.srWindow.Bottom - csbi.srWindow.Top + 1 - kBannerRows;
            }
            if (log_rows < 1) log_rows = 1;
            while ((int)ring_.size() > log_rows) ring_.erase(ring_.begin());
            repaint_locked();
        } else {
            // Redirected stdout: print the line plainly.
            std::fprintf(stdout, "%s\n", line.c_str());
            std::fflush(stdout);
        }
    }

private:
    static const int kBannerRows = 7;

    static WORD attr(Level lv) {
        switch (lv) {
        case Level::Debug:   return 8;   // dark gray
        case Level::Info:    return 11;  // bright cyan
        case Level::Success: return 10;  // bright green
        case Level::Warn:    return 14;  // bright yellow
        case Level::Error:   return 12;  // bright red
        }
        return 7;
    }

    static const char* tag(Level lv) {
        switch (lv) {
        case Level::Debug:   return "[DEBUG]";
        case Level::Info:    return "[INFO ]";
        case Level::Success: return "[ OK  ]";
        case Level::Warn:    return "[WARN ]";
        case Level::Error:   return "[ERROR]";
        }
        return "[INFO ]";
    }

    static std::string date_str() {
        SYSTEMTIME st;
        GetLocalTime(&st);
        char b[16];
        std::snprintf(b, sizeof(b), "%04d-%02d-%02d", st.wYear, st.wMonth, st.wDay);
        return b;
    }

    static std::string timestamp_locked() {
        SYSTEMTIME st;
        GetLocalTime(&st);
        char b[32];
        std::snprintf(b, sizeof(b), "%02d:%02d:%02d.%03d",
                      st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
        return b;
    }

    // Open the daily file if the date (or the configured dir) changed.
    void ensure_file_locked() {
        std::string d = date_str();
        std::string path = log_dir_.empty() ? "" : log_dir_ + "\\fsrec_" + d + ".log";
        if (path == log_path_ && file_.is_open()) return;
        if (file_.is_open()) file_.close();
        log_path_ = path;
        if (!path.empty()) file_.open(std::filesystem::u8path(path), std::ios::app);
    }

    // Truncate a UTF-8 string to at most `max_cols` display columns without
    // splitting a multi-byte sequence (CJK chars count as 2 columns).
    static std::string fit_columns(const std::string& s, int max_cols) {
        if (max_cols <= 0) return "";
        int cols = 0;
        size_t i = 0, cut = 0;
        while (i < s.size() && cols < max_cols) {
            unsigned char c = static_cast<unsigned char>(s[i]);
            int seq = 1;
            if (c >= 0xC0) {
                if ((c & 0xE0) == 0xC0) seq = 2;
                else if ((c & 0xF0) == 0xE0) seq = 3;
                else if ((c & 0xF8) == 0xF0) seq = 4;
            }
            int w = (c < 0x80) ? 1 : 2;
            if (cols + w > max_cols) break;
            cols += w;
            i += seq;
            cut = i;
        }
        return s.substr(0, cut);
    }

    // Write one segment at (row, col) with a given color; col is advanced by
    // the caller only via WriteConsoleA's cursor movement.
    void put_locked(int row, int col, const std::string& text, WORD color,
                    int left, int max_cols) {
        SetConsoleCursorPosition(hOut_, {static_cast<SHORT>(left + col), static_cast<SHORT>(row)});
        SetConsoleTextAttribute(hOut_, color);
        DWORD written = 0;
        WriteConsoleA(hOut_, text.c_str(), static_cast<DWORD>(text.size()), &written, nullptr);
        (void)max_cols;
    }

    void repaint_locked() {
        if (!console_) return;
        CONSOLE_SCREEN_BUFFER_INFO csbi{};
        if (!GetConsoleScreenBufferInfo(hOut_, &csbi)) return;

        int left = csbi.srWindow.Left;
        int top = csbi.srWindow.Top;
        int cols = csbi.srWindow.Right - csbi.srWindow.Left + 1;
        int rows = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
        if (cols < 20 || rows < 5) return;

        DWORD cells = static_cast<DWORD>(cols) * static_cast<DWORD>(rows);
        DWORD written = 0;
        COORD origin{static_cast<SHORT>(left), static_cast<SHORT>(top)};
        FillConsoleOutputCharacterA(hOut_, ' ', cells, origin, &written);
        FillConsoleOutputAttribute(hOut_,
                                   FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE,
                                   cells, origin, &written);

        const WORD c_dim = 8, c_bright = 15, c_cyan = 11, c_green = 10;
        std::string rule(cols, '=');

        // Banner (rows 0..kBannerRows-1).
        put_locked(top, 0, rule, c_dim, left, cols);
        {
            std::string title = " fsrec v" + version_ + "  ·  NTFS 数据恢复服务";
            put_locked(top + 1, 0, fit_columns(title, cols), c_cyan, left, cols);
        }
        {
            std::string label = "  访问地址  ";
            put_locked(top + 2, 0, label, c_dim, left, cols);
            put_locked(top + 2, static_cast<int>(label.size()), fit_columns(url_, cols - (int)label.size()), c_bright, left, cols);
        }
        {
            std::string label = "  健康检查  ";
            put_locked(top + 3, 0, label, c_dim, left, cols);
            put_locked(top + 3, static_cast<int>(label.size()), fit_columns(url_ + "/ping", cols - (int)label.size()), c_green, left, cols);
        }
        {
            std::string label = "  日志文件  ";
            put_locked(top + 4, 0, label, c_dim, left, cols);
            put_locked(top + 4, static_cast<int>(label.size()), fit_columns(log_path_, cols - (int)label.size()), c_cyan, left, cols);
        }
        {
            std::string label = "  停止服务  ";
            put_locked(top + 5, 0, label, c_dim, left, cols);
            put_locked(top + 5, static_cast<int>(label.size()), "Ctrl+C 或 网页右上角「退出」", c_dim, left, cols);
        }
        put_locked(top + kBannerRows - 1, 0, rule, c_dim, left, cols);

        // Log region below the banner.
        int log_rows = rows - kBannerRows;
        int start = top + kBannerRows;
        // Show the newest `log_rows` entries; ring_ already holds <= log_rows.
        int offset = static_cast<int>(ring_.size()) - log_rows;
        if (offset < 0) offset = 0;
        for (int i = 0; i < log_rows; ++i) {
            int idx = offset + i;
            int row = start + i;
            if (idx < 0 || idx >= static_cast<int>(ring_.size())) {
                put_locked(row, 0, "", c_dim, left, cols);
                continue;
            }
            const auto& e = ring_[idx];
            std::string line = timestamp_locked() + " " + tag(e.first) + " " + e.second;
            put_locked(row, 0, fit_columns(line, cols), attr(e.first), left, cols);
        }
    }

    std::mutex mutex_;
    std::string log_dir_;
    std::string version_;
    std::string url_;
    std::string log_path_;
    std::ofstream file_;

    bool console_ = false;
    HANDLE hOut_ = INVALID_HANDLE_VALUE;
    std::vector<std::pair<Level, std::string>> ring_;
};

} // namespace fslog

// Convenience macros for printf-style logging.
#define FSLOG_DEBUG(...)   fslog::Logger::instance().log(fslog::Level::Debug,   __VA_ARGS__)
#define FSLOG_INFO(...)    fslog::Logger::instance().log(fslog::Level::Info,    __VA_ARGS__)
#define FSLOG_SUCCESS(...) fslog::Logger::instance().log(fslog::Level::Success, __VA_ARGS__)
#define FSLOG_WARN(...)    fslog::Logger::instance().log(fslog::Level::Warn,    __VA_ARGS__)
#define FSLOG_ERROR(...)   fslog::Logger::instance().log(fslog::Level::Error,   __VA_ARGS__)
