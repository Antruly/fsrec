#include "boot_parser.h"

#include <cstring>
#include <vector>

namespace recovery {

// Little-endian integer readers.
static inline uint16_t le16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}
static inline uint32_t le32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
static inline uint64_t le64(const uint8_t* p) {
    return static_cast<uint64_t>(le32(p)) |
           (static_cast<uint64_t>(le32(p + 4)) << 32);
}

bool BootParser::parse(DiskReader& reader, std::string* error) {
    NTFSBootInfo& b = reader.boot();

    std::vector<uint8_t> sector(512);
    if (!reader.read(0, sector.data(), 512)) {
        if (error) *error = "failed to read boot sector";
        return false;
    }

    // OEM ID must be "NTFS    ".
    static const char kOem[9] = "NTFS    ";
    if (std::memcmp(sector.data() + 3, kOem, 8) != 0) {
        if (error) *error = "not an NTFS volume (bad OEM id)";
        return false;
    }

    b.bytes_per_sector    = le16(sector.data() + 0x0B);
    b.sectors_per_cluster = sector[0x0D];
    // Reserved sectors (usually 0 for NTFS), not needed here.
    b.total_sectors       = le64(sector.data() + 0x28);
    b.mft_lcn             = le64(sector.data() + 0x30);
    b.mft_mirr_lcn        = le64(sector.data() + 0x38);

    // File record segment size: signed byte at 0x40.
    int8_t frs = static_cast<int8_t>(sector[0x40]);
    if (frs > 0) {
        b.mft_record_size = static_cast<uint32_t>(frs) * b.cluster_size();
    } else {
        b.mft_record_size = 1u << (-frs);
    }

    // Index buffer size: signed byte at 0x41.
    int8_t ibs = static_cast<int8_t>(sector[0x41]);
    if (ibs > 0) {
        b.index_buffer_size = static_cast<uint32_t>(ibs) * b.cluster_size();
    } else {
        b.index_buffer_size = 1u << (-ibs);
    }

    if (b.bytes_per_sector == 0 || b.sectors_per_cluster == 0) {
        if (error) *error = "invalid BPB geometry";
        return false;
    }
    if (b.mft_lcn == 0) {
        if (error) *error = "invalid $MFT location";
        return false;
    }

    b.valid = true;
    return true;
}

} // namespace recovery
