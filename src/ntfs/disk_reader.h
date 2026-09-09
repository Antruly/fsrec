#pragma once
// Raw read-only access to a NTFS volume or physical disk via Windows API
// (\\\\.\\X: and \\\\.\\PhysicalDriveN).

#include <cstdint>
#include <string>
#include <windows.h>

#include "../include/types.h"

namespace recovery {

class DiskReader {
public:
    DiskReader() = default;
    ~DiskReader();

    DiskReader(const DiskReader&) = delete;
    DiskReader& operator=(const DiskReader&) = delete;

    // Open a volume for raw read. `drive` may be "D", "D:", "D:\\" or
    // "\\\\.\\D:". Returns true on success. Requires administrator rights.
    bool open(const std::string& drive, std::string* error = nullptr);

    // Open a physical disk for raw read: "\\\\.\\PhysicalDriveN".
    bool open_physical(int disk_number, std::string* error = nullptr);

    void close();
    bool is_open() const { return handle_ != INVALID_HANDLE_VALUE; }

    // Read `size` bytes starting at absolute byte `offset` into `buffer`.
    // Returns true if `size` bytes were read. `offset` is relative to the
    // volume start (i.e. base_offset() is added internally).
    bool read(uint64_t offset, void* buffer, size_t size, size_t* bytes_read = nullptr);

    // Read a single sector (bytes_per_sector sized) into `buffer`.
    bool read_sector(uint64_t sector, void* buffer);

    // Read a whole cluster into `buffer` (must hold cluster_size() bytes).
    bool read_cluster(uint64_t cluster, void* buffer);

    const NTFSBootInfo& boot() const { return boot_; }
    NTFSBootInfo& boot() { return boot_; }

    // Cluster size (bytes) used by read_cluster. Defaults to the boot-sector
    // derived value; set_cluster_size overrides it so a recovered volume with an
    // arbitrary (non-512-multiple) cluster size — e.g. exFAT 128 KiB clusters —
    // reads correctly during recovery.
    void set_cluster_size(uint64_t sz) { cluster_size_override_ = sz; }
    uint64_t cluster_size() const {
        return cluster_size_override_ ? cluster_size_override_ : boot_.cluster_size();
    }

    // Byte offset added to every read. For a drive-letter volume this is 0;
    // for a carved volume on a physical disk it is the volume's start byte.
    void set_base_offset(uint64_t base) { base_offset_ = base; }
    uint64_t base_offset() const { return base_offset_; }

    // Human-readable device string this reader is bound to (e.g. "D:" or
    // "\\\\.\\PhysicalDrive1").
    const std::string& drive() const { return drive_; }

    // Total size in bytes of the physical disk (0 if unknown).
    uint64_t physical_size() const { return physical_size_; }

private:
    // Sector-aligned raw read (offset and size must be sector multiples).
    bool read_aligned(uint64_t offset, void* buffer, size_t size);

    HANDLE handle_ = INVALID_HANDLE_VALUE;
    NTFSBootInfo boot_;
    std::string drive_;
    uint64_t base_offset_ = 0;
    uint64_t physical_size_ = 0;
    uint64_t cluster_size_override_ = 0;
};

// Cumulative raw bytes actually read across every DiskReader in this process
// (scan + recover + candidate search). Used to derive a live read-throughput
// figure for the UI. Thread-safe (atomic).
uint64_t total_bytes_read();

} // namespace recovery
