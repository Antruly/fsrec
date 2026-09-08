#include "disk_reader.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>

#include <winioctl.h>

namespace recovery {

// Process-wide raw-read byte counter (see total_bytes_read() in the header).
static std::atomic<uint64_t> g_total_bytes_read{0};

// Per-disk raw-read byte counters, indexed by PhysicalDriveN number.
static std::atomic<uint64_t> g_disk_bytes[32]{};

uint64_t total_bytes_read() {
    return g_total_bytes_read.load(std::memory_order_relaxed);
}

uint64_t disk_bytes_read(int disk_number) {
    if (disk_number < 0 || disk_number >= 32) return 0;
    return g_disk_bytes[disk_number].load(std::memory_order_relaxed);
}

DiskReader::~DiskReader() {
    close();
}

void DiskReader::close() {
    if (handle_ != INVALID_HANDLE_VALUE) {
        CloseHandle(handle_);
        handle_ = INVALID_HANDLE_VALUE;
    }
    base_offset_ = 0;
    physical_size_ = 0;
    disk_number_ = -1;
}

bool DiskReader::open(const std::string& drive, std::string* error) {
    close();

    std::string d = drive;
    // Normalize to "X:" form.
    // Trim whitespace.
    d.erase(0, d.find_first_not_of(" \t"));
    d.erase(d.find_last_not_of(" \t") + 1);
    if (d.empty()) {
        if (error) *error = "empty drive";
        return false;
    }
    // Strip trailing backslashes.
    while (!d.empty() && (d.back() == '\\' || d.back() == '/')) d.pop_back();

    std::string volume_path;
    if (d.size() >= 4 && d[0] == '\\' && d[1] == '\\') {
        volume_path = d; // already a device path
    } else {
        // Assume a single drive letter.
        char letter = static_cast<char>(toupper(static_cast<unsigned char>(d[0])));
        volume_path = std::string("\\\\.\\") + letter + ":";
        d = std::string(1, letter) + ":";
    }

    handle_ = CreateFileA(volume_path.c_str(),
                          GENERIC_READ,
                          FILE_SHARE_READ | FILE_SHARE_WRITE,
                          nullptr,
                          OPEN_EXISTING,
                          FILE_ATTRIBUTE_NORMAL | FILE_FLAG_NO_BUFFERING,
                          nullptr);
    if (handle_ == INVALID_HANDLE_VALUE) {
        if (error) {
            DWORD e = GetLastError();
            char buf[128];
            snprintf(buf, sizeof(buf), "cannot open %s (error %lu, run as Administrator)",
                     volume_path.c_str(), static_cast<unsigned long>(e));
            *error = buf;
        }
        return false;
    }
    drive_ = d;
    return true;
}

bool DiskReader::open_physical(int disk_number, std::string* error) {
    close();

    char path[64];
    snprintf(path, sizeof(path), "\\\\.\\PhysicalDrive%d", disk_number);

    handle_ = CreateFileA(path,
                          GENERIC_READ,
                          FILE_SHARE_READ | FILE_SHARE_WRITE,
                          nullptr,
                          OPEN_EXISTING,
                          FILE_ATTRIBUTE_NORMAL | FILE_FLAG_NO_BUFFERING,
                          nullptr);
    if (handle_ == INVALID_HANDLE_VALUE) {
        if (error) {
            DWORD e = GetLastError();
            char buf[128];
            snprintf(buf, sizeof(buf), "cannot open %s (error %lu, run as Administrator)",
                     path, static_cast<unsigned long>(e));
            *error = buf;
        }
        return false;
    }
    drive_ = path;
    // Physical disks expose 512-byte logical sectors (assumed for alignment);
    // the scan sets the real NTFS geometry once a volume is located.
    boot_.bytes_per_sector = 512;

    GET_LENGTH_INFORMATION li{};
    DWORD unused = 0;
    if (DeviceIoControl(handle_, IOCTL_DISK_GET_LENGTH_INFO, nullptr, 0,
                        &li, sizeof(li), &unused, nullptr)) {
        physical_size_ = static_cast<uint64_t>(li.Length.QuadPart);
    }
    return true;
}

bool DiskReader::read(uint64_t offset, void* buffer, size_t size, size_t* bytes_read) {
    if (handle_ == INVALID_HANDLE_VALUE) return false;

    // FILE_FLAG_NO_BUFFERING requires sector-aligned offsets and lengths.
    // To keep the API simple we accept unaligned requests and read through a
    // small aligned bounce buffer when necessary.
    const size_t sector = boot_.bytes_per_sector ? boot_.bytes_per_sector : 512;
    size_t done = 0;
    uint8_t* out = static_cast<uint8_t*>(buffer);

    // Read sector-aligned prefix if offset is not sector aligned.
    uint64_t cur = offset;
    size_t pre_off = static_cast<size_t>(cur % sector);
    if (pre_off != 0) {
        size_t head = static_cast<size_t>((std::min<uint64_t>)(size, sector - pre_off));
        std::vector<uint8_t> tmp(sector);
        uint64_t base = cur - pre_off;
        if (!read_aligned(base, tmp.data(), sector)) {
            if (bytes_read) *bytes_read = done;
            return false;
        }
        std::memcpy(out, tmp.data() + pre_off, head);
        out += head;
        cur += head;
        done += head;
        size -= head;
        if (size == 0) {
            if (bytes_read) *bytes_read = done;
            return true;
        }
    }

    // Main aligned read.
    size_t aligned = size - (size % sector);
    if (aligned > 0) {
        if (!read_aligned(cur, out, aligned)) {
            if (bytes_read) *bytes_read = done;
            return false;
        }
        out += aligned;
        cur += aligned;
        done += aligned;
        size -= aligned;
    }

    // Trailing partial sector.
    if (size > 0) {
        std::vector<uint8_t> tmp(sector);
        if (!read_aligned(cur, tmp.data(), sector)) {
            if (bytes_read) *bytes_read = done;
            return false;
        }
        std::memcpy(out, tmp.data(), size);
        done += size;
    }

    if (bytes_read) *bytes_read = done;
    return true;
}

bool DiskReader::read_aligned(uint64_t offset, void* buffer, size_t size) {
    LARGE_INTEGER li;
    li.QuadPart = static_cast<LONGLONG>(offset + base_offset_);
    if (!SetFilePointerEx(handle_, li, nullptr, FILE_BEGIN)) return false;

    uint8_t* p = static_cast<uint8_t*>(buffer);
    size_t total = 0;
    while (total < size) {
        DWORD got = 0;
        if (!ReadFile(handle_, p + total, static_cast<DWORD>(size - total), &got, nullptr)) {
            return false;
        }
        if (got == 0) break; // EOF
        total += got;
    }
    g_total_bytes_read.fetch_add(total, std::memory_order_relaxed);
    return total == size;
}

bool DiskReader::read_sector(uint64_t sector, void* buffer) {
    uint64_t off = sector * boot_.bytes_per_sector;
    return read_aligned(off, buffer, boot_.bytes_per_sector);
}

bool DiskReader::read_cluster(uint64_t cluster, void* buffer) {
    uint64_t off = cluster * boot_.cluster_size();
    return read_aligned(off, buffer, static_cast<size_t>(boot_.cluster_size()));
}

} // namespace recovery
