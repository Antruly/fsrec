#include "util.h"

#include <cstdio>
#include <cstdlib>
#include <ctime>

namespace recovery {

std::string filetime_to_iso8601(int64_t filetime) {
    if (filetime <= 0) return "";
    // 100ns intervals since 1601 -> seconds since 1970.
    const int64_t kEpochOffset = 116444736000000000LL; // 1601 -> 1970 in 100ns
    int64_t seconds = (filetime - kEpochOffset) / 10000000LL;
    if (seconds < 0) return "";

    time_t t = static_cast<time_t>(seconds);
    struct tm tm_buf;
    gmtime_s(&tm_buf, &t);
    char buf[32];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02dZ",
             tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
             tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec);
    return std::string(buf);
}

std::string utf16_to_utf8(const std::u16string& in) {
    std::string out;
    out.reserve(in.size() * 3);
    for (size_t i = 0; i < in.size(); i++) {
        uint32_t cp = static_cast<uint16_t>(in[i]);
        // Surrogate pair.
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < in.size()) {
            uint32_t lo = static_cast<uint16_t>(in[i + 1]);
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                i++;
            }
        }
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    return out;
}

std::u16string utf8_to_utf16(const std::string& in) {
    std::u16string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); i++) {
        uint32_t cp = static_cast<unsigned char>(in[i]);
        size_t extra = 0;
        if (cp < 0x80) {
            extra = 0;
        } else if ((cp & 0xE0) == 0xC0) {
            cp &= 0x1F; extra = 1;
        } else if ((cp & 0xF0) == 0xE0) {
            cp &= 0x0F; extra = 2;
        } else if ((cp & 0xF8) == 0xF0) {
            cp &= 0x07; extra = 3;
        } else {
            cp = 0xFFFD; extra = 0;
        }
        if (i + extra >= in.size()) { cp = 0xFFFD; extra = 0; }
        for (size_t k = 0; k < extra; k++) {
            uint32_t cc = static_cast<unsigned char>(in[i + 1 + k]);
            if ((cc & 0xC0) != 0x80) { cp = 0xFFFD; extra = k; break; }
            cp = (cp << 6) | (cc & 0x3F);
        }
        i += extra;
        if (cp >= 0x10000) {
            cp -= 0x10000;
            out.push_back(static_cast<char16_t>(0xD800 + (cp >> 10)));
            out.push_back(static_cast<char16_t>(0xDC00 + (cp & 0x3FF)));
        } else {
            out.push_back(static_cast<char16_t>(cp));
        }
    }
    return out;
}

std::string url_decode(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); i++) {
        char c = in[i];
        if (c == '+') {
            out.push_back(' ');
        } else if (c == '%' && i + 2 < in.size()) {
            auto hex = [](char ch) -> int {
                if (ch >= '0' && ch <= '9') return ch - '0';
                if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
                if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
                return -1;
            };
            int hi = hex(in[i + 1]);
            int lo = hex(in[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
            } else {
                out.push_back(c);
            }
        } else {
            out.push_back(c);
        }
    }
    return out;
}

std::map<std::string, std::string> parse_query(const std::string& query) {
    std::map<std::string, std::string> result;
    size_t start = 0;
    while (start <= query.size()) {
        size_t amp = query.find('&', start);
        std::string pair = query.substr(start, amp == std::string::npos ? std::string::npos : amp - start);
        if (!pair.empty()) {
            size_t eq = pair.find('=');
            std::string key = eq == std::string::npos ? pair : pair.substr(0, eq);
            std::string val = eq == std::string::npos ? "" : pair.substr(eq + 1);
            result[url_decode(key)] = url_decode(val);
        }
        if (amp == std::string::npos) break;
        start = amp + 1;
    }
    return result;
}

} // namespace recovery
