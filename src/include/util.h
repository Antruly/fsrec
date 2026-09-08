#pragma once
// Small header-only helpers: UTF conversion, time formatting, URL helpers.

#include <cstdint>
#include <map>
#include <string>

namespace recovery {

// Convert a Windows FILETIME (100ns since 1601-01-01 UTC) to an ISO-8601
// string like "2026-01-15T10:30:00Z". Returns empty string for 0/negative.
std::string filetime_to_iso8601(int64_t filetime);

// Minimal UTF-16LE -> UTF-8 converter (handles BMP + surrogate pairs).
std::string utf16_to_utf8(const std::u16string& in);

// Minimal UTF-8 -> UTF-16 converter (handles BMP + surrogate pairs).
std::u16string utf8_to_utf16(const std::string& in);

// Percent-decode a URL component (also translates '+' to space in query form).
std::string url_decode(const std::string& in);

// Parse a query string ("a=1&b=2") into a key/value map (values decoded).
std::map<std::string, std::string> parse_query(const std::string& query);

} // namespace recovery
