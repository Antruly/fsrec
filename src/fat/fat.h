#pragma once
// FAT12/16/32 + exFAT parsing: boot-sector detection / BPB decode and
// directory-tree reconstruction. Emits the same FileNode / DataRun model as the
// NTFS path (names as u16string, data_runs with lcn = absolute volume cluster),
// so browsing, searching and recovery work unchanged.

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "../include/types.h"
#include "../ntfs/disk_reader.h"

namespace recovery {

// Identify the filesystem described by a 512-byte boot sector.
FsType detect_fs_type(const uint8_t boot[512]);

// FAT12/16/32 BPB (decoded byte offsets from the boot sector).
struct FatBootInfo {
    FsType type = FsType::Unknown;

    uint16_t bytes_per_sector    = 0;
    uint8_t  sectors_per_cluster = 0;
    uint16_t reserved_sectors    = 0;
    uint8_t  num_fats            = 0;
    uint16_t root_entry_count    = 0;   // 0 for FAT32
    uint16_t total_sectors16     = 0;   // 0 => use total_sectors32
    uint32_t total_sectors32     = 0;
    uint16_t fat_size16          = 0;   // 0 for FAT32
    uint32_t fat_size32          = 0;   // FAT32 only
    uint32_t root_cluster        = 0;   // FAT32 only

    bool valid = false;

    uint64_t cluster_size() const { return uint64_t(bytes_per_sector) * sectors_per_cluster; }
    uint64_t total_sectors() const { return total_sectors16 ? total_sectors16 : total_sectors32; }
    uint64_t fat_size_sectors() const { return fat_size16 ? fat_size16 : fat_size32; }
    uint64_t root_dir_sectors() const {
        return (uint64_t(root_entry_count) * 32 + bytes_per_sector - 1) / bytes_per_sector;
    }
    uint64_t first_data_sector() const {
        return uint64_t(reserved_sectors) + uint64_t(num_fats) * fat_size_sectors()
             + root_dir_sectors();
    }
    // Absolute volume-cluster index of FAT cluster 2 (the first data cluster).
    uint64_t first_data_cluster() const { return first_data_sector() / sectors_per_cluster; }
    uint64_t cluster_count() const {
        return (total_sectors() - first_data_sector()) / sectors_per_cluster;
    }
};

// exFAT boot sector (Main Boot Sector).
struct ExfatBootInfo {
    uint64_t volume_length       = 0;   // sectors
    uint32_t fat_offset          = 0;   // sectors from volume start
    uint32_t fat_length          = 0;   // sectors per FAT
    uint32_t cluster_heap_offset = 0;   // sectors from volume start
    uint32_t cluster_count       = 0;
    uint32_t root_cluster        = 0;
    uint8_t  bytes_per_sector_shift   = 0;
    uint8_t  sectors_per_cluster_shift = 0;
    uint8_t  num_fats            = 0;

    bool valid = false;

    uint64_t bytes_per_sector() const { return 1ull << bytes_per_sector_shift; }
    uint64_t sectors_per_cluster() const { return 1ull << sectors_per_cluster_shift; }
    uint64_t cluster_size() const { return bytes_per_sector() * sectors_per_cluster(); }
    // Absolute volume-cluster index of exFAT cluster 2.
    uint64_t first_data_cluster() const { return cluster_heap_offset / sectors_per_cluster(); }
};

// Parse a FAT12/16/32 boot sector. Returns false (with a reason in `error`)
// when the sector is not a valid FAT boot sector.
bool parse_fat_boot(const uint8_t boot[512], FatBootInfo& out, std::string* error = nullptr);

// Parse an exFAT boot sector.
bool parse_exfat_boot(const uint8_t boot[512], ExfatBootInfo& out, std::string* error = nullptr);

// Result of scanning a single FAT/exFAT volume.
struct FatScanResult {
    FileNodePtr root;
    std::vector<FileNodePtr> nodes;
    uint64_t total_files   = 0;
    uint64_t deleted_files = 0;
    uint64_t directories   = 0;
};

// Scan a FAT12/16/32 volume. `reader` must be open on the physical disk with
// base_offset set to `volume_start` (the volume's byte offset on the disk).
// Populates `out`; returns false with a reason on failure. Honors `stop`.
bool scan_fat_volume(DiskReader& reader, const FatBootInfo& fat, FatScanResult& out,
                     const std::atomic<bool>& stop, std::string* error);

// Scan an exFAT volume (same contract as scan_fat_volume).
bool scan_exfat_volume(DiskReader& reader, const ExfatBootInfo& exfat, FatScanResult& out,
                       const std::atomic<bool>& stop, std::string* error);

} // namespace recovery
