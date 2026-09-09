#pragma once
// NTFS $MFT record parser.

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "../include/types.h"
#include "disk_reader.h"

namespace recovery {

// NTFS attribute type ids.
enum AttributeType : uint32_t {
    AT_STANDARD_INFORMATION = 0x10,
    AT_ATTRIBUTE_LIST       = 0x20,
    AT_FILE_NAME            = 0x30,
    AT_OBJECT_ID            = 0x40,
    AT_VOLUME_NAME          = 0x60,
    AT_VOLUME_INFORMATION   = 0x70,
    AT_DATA                 = 0x80,
    AT_INDEX_ROOT           = 0x90,
    AT_INDEX_ALLOCATION     = 0xA0,
    AT_BITMAP               = 0xB0,
    AT_END                  = 0xFFFFFFFF,
};

class MftParser {
public:
    explicit MftParser(DiskReader& reader) : reader_(reader) {}

    // Byte offset on disk of the given MFT record number.
    uint64_t record_offset(uint64_t mft_number) const;

    // Read the raw record (record_size bytes) into `buffer`.
    // Returns false on read error or empty buffer.
    bool read_record(uint64_t mft_number, std::vector<uint8_t>& buffer);

    // Parse a raw record buffer into a FileNode. Returns false if the record
    // is not a valid "FILE" record or is a non-base (extension) record.
    //
    // `ext_reader` (optional) reads an extension record by number into `buffer`
    // (already USA-fixed) and returns true on success. When provided, a resident
    // $ATTRIBUTE_LIST is followed so a non-resident $DATA whose run list is
    // split across extension records is reassembled into `node.data_runs` in VCN
    // order; without it such a file is listed but marked unrecoverable.
    bool parse_record(const std::vector<uint8_t>& buffer, FileNode& node,
                      const std::function<bool(uint64_t, std::vector<uint8_t>&)>& ext_reader = {});

    // Apply the Update Sequence Array fixup in place. Returns false on
    // inconsistent USA (which we tolerate by leaving data as-is).
    static void apply_usa_fixup(std::vector<uint8_t>& buffer, uint32_t sector_size);

private:
    DiskReader& reader_;
};

} // namespace recovery
