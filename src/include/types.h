#pragma once
// Common type definitions shared across the recovery server modules.

#include <cstdint>
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <atomic>

namespace recovery {

// ---------------------------------------------------------------------------
// NTFS boot sector / BPB parameters
// ---------------------------------------------------------------------------
struct NTFSBootInfo {
    uint16_t bytes_per_sector    = 512;
    uint8_t  sectors_per_cluster = 8;
    uint64_t total_sectors       = 0;
    uint64_t mft_lcn             = 0;   // $MFT start cluster
    uint64_t mft_mirr_lcn        = 0;   // $MFTMirr start cluster
    uint32_t mft_record_size     = 1024; // file record segment size
    uint32_t index_buffer_size   = 4096;
    bool     valid               = false;

    uint64_t cluster_size() const {
        return static_cast<uint64_t>(bytes_per_sector) * sectors_per_cluster;
    }
    uint64_t total_size() const {
        return total_sectors * bytes_per_sector;
    }
};

// ---------------------------------------------------------------------------
// Filesystem families recognized by the scanner. NTFS is the original target;
// FAT12/16/32 and exFAT share the same FileNode/DataRun model so browsing,
// searching and recovery work unchanged.
// ---------------------------------------------------------------------------
enum class FsType {
    Unknown,
    NTFS,
    FAT12,
    FAT16,
    FAT32,
    exFAT,
};

inline const char* fs_type_name(FsType t) {
    switch (t) {
        case FsType::NTFS:  return "NTFS";
        case FsType::FAT12: return "FAT12";
        case FsType::FAT16: return "FAT16";
        case FsType::FAT32: return "FAT32";
        case FsType::exFAT: return "exFAT";
        default:            return "Unknown";
    }
}

inline FsType fs_type_from_name(const std::string& s) {
    if (s == "NTFS")  return FsType::NTFS;
    if (s == "FAT12") return FsType::FAT12;
    if (s == "FAT16") return FsType::FAT16;
    if (s == "FAT32") return FsType::FAT32;
    if (s == "exFAT") return FsType::exFAT;
    return FsType::Unknown;
}

// ---------------------------------------------------------------------------
// A single data run (extent) of a non-resident attribute.
// ---------------------------------------------------------------------------
struct DataRun {
    int64_t  lcn    = 0;   // absolute logical cluster number (physical cluster)
    uint64_t length = 0;   // length in clusters
    bool     sparse = false; // sparse run (reads as zeroes)
};

// ---------------------------------------------------------------------------
// A parsed file/directory node. Doubles as the scan output tree node.
// ---------------------------------------------------------------------------
struct FileNode {
    uint64_t mft_id         = 0;
    uint64_t parent_mft_id  = 0;   // 0 => no parent (root / orphan)
    std::u16string name;           // raw UTF-16 name from $FILE_NAME
    bool     is_directory    = false;
    bool     is_deleted      = false; // in-use flag 0x00
    uint64_t size            = 0;   // real size in bytes
    uint64_t allocated_size  = 0;
    int64_t  modified_time   = 0;   // FILETIME (100ns since 1601-01-01)
    int64_t  creation_time   = 0;
    uint8_t  name_namespace  = 0;

    // Data descriptor (captured during scan so recovery needs no MFT re-read).
    bool     has_data        = false;
    bool     resident        = false;
    std::vector<uint8_t> resident_data;
    std::vector<DataRun>  data_runs;
    bool     recoverable     = false;

    // Tree linkage (populated by TreeBuilder).
    std::vector<std::shared_ptr<FileNode>> children;
};

using FileNodePtr = std::shared_ptr<FileNode>;

// ---------------------------------------------------------------------------
// Scan / recover task status enums shared by the service layer.
// ---------------------------------------------------------------------------
inline const char* task_status_name(const std::string& s) { return s.c_str(); }

} // namespace recovery
