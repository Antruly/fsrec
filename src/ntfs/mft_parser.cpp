#include "mft_parser.h"

#include <algorithm>
#include <cstring>

#include "data_run.h"

namespace recovery {

// Little-endian readers (replicated to keep modules self-contained).
static inline uint16_t le16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}
static inline uint32_t le32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
static inline uint64_t le64(const uint8_t* p) {
    return static_cast<uint64_t>(le32(p)) | (static_cast<uint64_t>(le32(p + 4)) << 32);
}

// One entry in a resident $ATTRIBUTE_LIST value.
struct AttrListEntry {
    uint32_t type = 0;
    uint8_t  name_len = 0;
    uint64_t start_vcn = 0;
    uint64_t mft_ref = 0;   // low 48 bits = MFT record number
};

// Parse a resident $ATTRIBUTE_LIST value into its entries.
static std::vector<AttrListEntry> parse_attribute_list(const uint8_t* v, uint32_t vlen) {
    std::vector<AttrListEntry> out;
    const uint8_t* end = v + vlen;
    const uint8_t* e = v;
    while (e + 0x18 <= end) {
        uint32_t type = le32(e + 0x00);
        uint16_t elen = le16(e + 0x04);
        if (elen < 0x18 || e + elen > end) break;
        AttrListEntry a;
        a.type = type;
        a.name_len = e[0x06];
        a.start_vcn = le64(e + 0x08);
        a.mft_ref = le64(e + 0x10) & 0x0000FFFFFFFFFFFFULL;
        out.push_back(a);
        e += elen;
    }
    return out;
}

// Extract the unnamed, non-resident $DATA run list from a USA-fixed record.
static bool extract_data_runs(const std::vector<uint8_t>& buffer, std::vector<DataRun>& runs) {
    const uint8_t* rec = buffer.data();
    size_t rec_size = buffer.size();
    if (rec_size < 0x30 || std::memcmp(rec, "FILE", 4) != 0) return false;
    uint16_t first_attr = le16(rec + 0x14);
    if (first_attr == 0 || first_attr >= rec_size) return false;

    size_t attr_off = first_attr;
    while (attr_off + 8 <= rec_size) {
        uint32_t type = le32(rec + attr_off);
        if (type == AT_END || type == 0x00000000) break;
        uint32_t length = le32(rec + attr_off + 0x04);
        if (length < 8 || attr_off + length > rec_size) break;
        uint8_t non_resident = rec[attr_off + 0x08];
        uint8_t name_len = rec[attr_off + 0x09];
        if (type == AT_DATA && non_resident && name_len == 0) {
            uint16_t run_off = le16(rec + attr_off + 0x20);
            if (run_off > 0 && attr_off + run_off < attr_off + length) {
                return DataRunParser::parse(rec + attr_off + run_off,
                                            attr_off + length - (attr_off + run_off),
                                            runs);
            }
        }
        attr_off += length;
    }
    return false;
}

// Reassemble a $DATA run list split across extension records. `node.data_runs`
// already holds the base extent (lowest VCN 0, absolute LCNs); extension extents
// referenced by the attribute list are read via `ext_reader` and appended in
// ascending start-VCN order. The run list is one continuous stream, so a
// fragment's first offset is a delta relative to the previous fragment's last
// LCN: add `prev_lcn` to every non-sparse run (sparse runs have no LCN).
static void merge_extension_data_runs(FileNode& node, uint64_t base_number,
                                      const std::vector<AttrListEntry>& entries,
                                      const std::function<bool(uint64_t, std::vector<uint8_t>&)>& ext_reader) {
    std::vector<AttrListEntry> exts;
    for (const auto& e : entries)
        if (e.type == AT_DATA && e.name_len == 0 && e.mft_ref != base_number)
            exts.push_back(e);
    if (exts.empty()) return;

    std::sort(exts.begin(), exts.end(),
              [](const AttrListEntry& a, const AttrListEntry& b) { return a.start_vcn < b.start_vcn; });

    int64_t prev_lcn = 0;
    for (const auto& r : node.data_runs) if (!r.sparse) prev_lcn = r.lcn;

    for (const auto& e : exts) {
        std::vector<uint8_t> ext_buf;
        if (!ext_reader(e.mft_ref, ext_buf)) continue;
        std::vector<DataRun> frag;
        if (!extract_data_runs(ext_buf, frag)) continue;
        for (auto& r : frag) {
            if (!r.sparse) {
                r.lcn += prev_lcn;
                prev_lcn = r.lcn;
            }
        }
        node.data_runs.insert(node.data_runs.end(), frag.begin(), frag.end());
    }

    if (!node.data_runs.empty()) node.has_data = true;
}

uint64_t MftParser::record_offset(uint64_t mft_number) const {
    const NTFSBootInfo& b = reader_.boot();
    return b.mft_lcn * b.cluster_size() + mft_number * b.mft_record_size;
}

bool MftParser::read_record(uint64_t mft_number, std::vector<uint8_t>& buffer) {
    const NTFSBootInfo& b = reader_.boot();
    buffer.assign(b.mft_record_size, 0);
    uint64_t off = record_offset(mft_number);
    size_t got = 0;
    if (!reader_.read(off, buffer.data(), b.mft_record_size, &got)) return false;
    if (got < 512) return false; // not even a full header
    return true;
}

void MftParser::apply_usa_fixup(std::vector<uint8_t>& buffer, uint32_t sector_size) {
    if (buffer.size() < 0x30 || sector_size == 0) return;
    uint16_t usa_offset = le16(buffer.data() + 0x04);
    uint16_t usa_count  = le16(buffer.data() + 0x06);
    if (usa_count < 2) return;
    if (usa_offset + usa_count * 2 > buffer.size()) return;

    uint16_t usn = le16(buffer.data() + usa_offset);
    for (uint16_t i = 1; i < usa_count; i++) {
        uint64_t sector_end = static_cast<uint64_t>(i) * sector_size;
        if (sector_end < 2 || sector_end > buffer.size()) break;
        uint16_t orig = le16(buffer.data() + usa_offset + i * 2);
        buffer[sector_end - 2] = static_cast<uint8_t>(orig & 0xFF);
        buffer[sector_end - 1] = static_cast<uint8_t>(orig >> 8);
    }
    (void)usn;
}

bool MftParser::parse_record(const std::vector<uint8_t>& buffer, FileNode& node,
                             const std::function<bool(uint64_t, std::vector<uint8_t>&)>& ext_reader) {
    if (buffer.size() < 0x30) return false;
    if (std::memcmp(buffer.data(), "FILE", 4) != 0) return false;

    const uint8_t* rec = buffer.data();
    size_t rec_size = buffer.size();

    uint16_t flags = le16(rec + 0x16);
    uint16_t first_attr = le16(rec + 0x14);
    uint64_t base_record = le64(rec + 0x20);
    uint32_t mft_number = 0;
    // MFT record number field exists on NTFS 3.1+ (offset 0x2C).
    if (rec_size >= 0x30) mft_number = le32(rec + 0x2C);

    node = FileNode();
    node.mft_id = mft_number;
    node.is_deleted = ((flags & 0x0001) == 0);
    node.is_directory = ((flags & 0x0002) != 0);

    // An extension record (non-zero base) has no $FILE_NAME; skip it.
    if (base_record != 0) return false;

    if (first_attr == 0 || first_attr >= rec_size) return false;

    bool has_file_name = false;
    uint8_t best_ns = 0xFF;
    bool saw_data = false;
    std::vector<AttrListEntry> attr_entries;
    bool saw_attribute_list = false;

    size_t attr_off = first_attr;
    while (attr_off + 8 <= rec_size) {
        uint32_t type = le32(rec + attr_off);
        if (type == AT_END || type == 0x00000000) break;

        uint32_t length = le32(rec + attr_off + 0x04);
        if (length < 8 || attr_off + length > rec_size) break;

        uint8_t  non_resident = rec[attr_off + 0x08];
        uint8_t  name_len     = rec[attr_off + 0x09];
        // uint16_t name_offset  = le16(rec + attr_off + 0x0A);

        if (type == AT_STANDARD_INFORMATION && !non_resident) {
            uint32_t vlen = le32(rec + attr_off + 0x10);
            uint16_t voff = le16(rec + attr_off + 0x14);
            if (attr_off + voff + vlen <= rec_size && vlen >= 0x20) {
                const uint8_t* v = rec + attr_off + voff;
                node.creation_time  = static_cast<int64_t>(le64(v + 0x00));
                node.modified_time  = static_cast<int64_t>(le64(v + 0x08));
            }
        } else if (type == AT_FILE_NAME && !non_resident) {
            uint32_t vlen = le32(rec + attr_off + 0x10);
            uint16_t voff = le16(rec + attr_off + 0x14);
            if (attr_off + voff + vlen <= rec_size && vlen >= 0x42) {
                const uint8_t* v = rec + attr_off + voff;
                uint64_t parent = le64(v + 0x00); // 6-byte ref, low 48 bits
                uint64_t parent_mft = parent & 0x0000FFFFFFFFFFFFULL;
                int64_t  mod_time  = static_cast<int64_t>(le64(v + 0x10));
                uint64_t real_size = le64(v + 0x30);
                uint64_t alloc_size = le64(v + 0x28);
                uint8_t  nlen      = v[0x40];
                uint8_t  ns        = v[0x41];
                const uint8_t* name = v + 0x42;
                if (0x42 + static_cast<size_t>(nlen) * 2 <= vlen) {
                    // Prefer Win32 (1) / Win32+DOS (3) names, then POSIX (0), then DOS (2).
                    int rank;
                    switch (ns) {
                        case 1: rank = 0; break;
                        case 3: rank = 1; break;
                        case 0: rank = 2; break;
                        default: rank = 3; break; // 2 (DOS) and anything else
                    }
                    if (rank < static_cast<int>(best_ns)) {
                        std::u16string u16name;
                        u16name.reserve(nlen);
                        for (uint8_t i = 0; i < nlen; i++) {
                            u16name.push_back(static_cast<char16_t>(le16(name + i * 2)));
                        }
                        node.name = u16name;
                        node.parent_mft_id = parent_mft;
                        node.size = real_size;
                        node.allocated_size = alloc_size;
                        node.modified_time = mod_time;
                        node.name_namespace = ns;
                        best_ns = static_cast<uint8_t>(rank);
                        has_file_name = true;
                    }
                }
            }
        } else if (type == AT_ATTRIBUTE_LIST && !non_resident) {
            uint32_t vlen = le32(rec + attr_off + 0x10);
            uint16_t voff = le16(rec + attr_off + 0x14);
            if (attr_off + voff + vlen <= rec_size) {
                attr_entries = parse_attribute_list(rec + attr_off + voff, vlen);
                saw_attribute_list = true;
            }
        } else if (type == AT_DATA) {
            if (non_resident) {
                uint16_t run_off = le16(rec + attr_off + 0x20);
                uint64_t real_size = le64(rec + attr_off + 0x30);
                uint64_t alloc_size = le64(rec + attr_off + 0x28);
                if (run_off > 0 && attr_off + run_off < attr_off + length) {
                    std::vector<DataRun> runs;
                    if (DataRunParser::parse(rec + attr_off + run_off,
                                             attr_off + length - (attr_off + run_off),
                                             runs)) {
                        node.has_data = true;
                        node.resident = false;
                        node.data_runs = std::move(runs);
                        node.size = real_size;
                        node.allocated_size = alloc_size;
                        saw_data = true;
                    }
                }
            } else {
                uint32_t vlen = le32(rec + attr_off + 0x10);
                uint16_t voff = le16(rec + attr_off + 0x14);
                if (attr_off + voff + vlen <= rec_size) {
                    node.has_data = true;
                    node.resident = true;
                    node.resident_data.assign(rec + attr_off + voff,
                                              rec + attr_off + voff + vlen);
                    if (node.size == 0) node.size = vlen;
                    saw_data = true;
                }
            }
        }

        attr_off += length;
    }

    if (!has_file_name) return false;

    // Follow the $ATTRIBUTE_LIST to reassemble a $DATA run list split across
    // extension records (only when the caller supplied an extension reader).
    if (ext_reader && saw_attribute_list && !attr_entries.empty()) {
        merge_extension_data_runs(node, mft_number, attr_entries, ext_reader);
    }

    // Recoverable when the $DATA stream parsed with actual content.
    if (node.resident) {
        node.recoverable = node.has_data && !node.resident_data.empty();
    } else {
        node.recoverable = false;
        for (const auto& r : node.data_runs) {
            if (!r.sparse && r.length > 0) { node.recoverable = true; break; }
        }
    }
    (void)saw_data;
    return true;
}

} // namespace recovery
