#include "fat.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <unordered_set>
#include <vector>

#include <windows.h>

namespace recovery {

// ---------------------------------------------------------------------------
// Little-endian integer readers (same layout helpers as boot_parser.cpp).
// ---------------------------------------------------------------------------
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

// ---------------------------------------------------------------------------
// Filesystem detection + BPB decoding
// ---------------------------------------------------------------------------
FsType detect_fs_type(const uint8_t boot[512]) {
    if (std::memcmp(boot + 3, "NTFS    ", 8) == 0) return FsType::NTFS;
    if (std::memcmp(boot + 3, "EXFAT   ", 8) == 0) return FsType::exFAT;
    FatBootInfo f;
    if (parse_fat_boot(boot, f)) return f.type;
    return FsType::Unknown;
}

bool parse_fat_boot(const uint8_t* boot, FatBootInfo& out, std::string* error) {
    out = FatBootInfo{};

    // FAT boot sectors begin with a short/near jump (0xEB or 0xE9).
    if (boot[0] != 0xEB && boot[0] != 0xE9) {
        if (error) *error = "not a FAT boot sector (bad jump instruction)";
        return false;
    }
    if (le16(boot + 510) != 0xAA55) {
        if (error) *error = "missing 0x55AA boot signature";
        return false;
    }

    uint16_t bps      = le16(boot + 0x0B);
    uint8_t  spc      = boot[0x0D];
    uint16_t rsvd     = le16(boot + 0x0E);
    uint8_t  nfats    = boot[0x10];
    uint16_t root_ent = le16(boot + 0x11);
    uint16_t tot16    = le16(boot + 0x13);
    uint16_t fatsz16  = le16(boot + 0x16);
    uint32_t tot32    = le32(boot + 0x20);
    uint32_t fatsz32  = le32(boot + 0x24);
    uint32_t root_cl  = le32(boot + 0x2C);

    if (bps != 512 && bps != 1024 && bps != 2048 && bps != 4096) {
        if (error) *error = "invalid bytes per sector";
        return false;
    }
    if (spc == 0 || (spc & (spc - 1)) != 0) {
        if (error) *error = "sectors per cluster is not a power of two";
        return false;
    }
    if (rsvd == 0) {
        if (error) *error = "reserved sector count is zero";
        return false;
    }
    if (nfats == 0 || nfats > 2) {
        if (error) *error = "invalid FAT copy count";
        return false;
    }

    uint64_t tot   = tot16 ? tot16 : tot32;
    uint64_t fatsz = fatsz16 ? fatsz16 : fatsz32;
    if (tot == 0)  { if (error) *error = "total sector count is zero"; return false; }
    if (fatsz == 0) { if (error) *error = "FAT size is zero"; return false; }

    uint64_t root_sectors = (uint64_t(root_ent) * 32 + bps - 1) / bps;
    uint64_t first_data   = uint64_t(rsvd) + uint64_t(nfats) * fatsz + root_sectors;
    uint64_t data_sectors = tot - first_data;
    uint64_t clus_count   = data_sectors / spc;

    FsType t;
    if (clus_count < 4085)       t = FsType::FAT12;
    else if (clus_count < 65525) t = FsType::FAT16;
    else                         t = FsType::FAT32;

    out.type              = t;
    out.bytes_per_sector  = bps;
    out.sectors_per_cluster = spc;
    out.reserved_sectors  = rsvd;
    out.num_fats          = nfats;
    out.root_entry_count  = root_ent;
    out.total_sectors16   = tot16;
    out.total_sectors32   = tot32;
    out.fat_size16        = fatsz16;
    out.fat_size32        = fatsz32;
    out.root_cluster      = (t == FsType::FAT32) ? root_cl : 0;
    out.valid             = true;
    return true;
}

bool parse_exfat_boot(const uint8_t* boot, ExfatBootInfo& out, std::string* error) {
    out = ExfatBootInfo{};

    if (std::memcmp(boot + 3, "EXFAT   ", 8) != 0) {
        if (error) *error = "not an exFAT boot sector (bad OEM id)";
        return false;
    }
    if (le16(boot + 510) != 0xAA55) {
        if (error) *error = "missing 0x55AA boot signature";
        return false;
    }

    out.volume_length         = le64(boot + 0x48);
    out.fat_offset            = le32(boot + 0x50);
    out.fat_length            = le32(boot + 0x54);
    out.cluster_heap_offset   = le32(boot + 0x58);
    out.cluster_count         = le32(boot + 0x5C);
    out.root_cluster          = le32(boot + 0x60);
    out.bytes_per_sector_shift    = boot[0x6C];
    out.sectors_per_cluster_shift = boot[0x6D];
    out.num_fats              = boot[0x6E];

    if (out.bytes_per_sector_shift < 9 || out.bytes_per_sector_shift > 12) {
        if (error) *error = "invalid exFAT bytes-per-sector shift";
        return false;
    }
    if (out.sectors_per_cluster_shift > 25) {
        if (error) *error = "invalid exFAT sectors-per-cluster shift";
        return false;
    }
    if (out.cluster_count == 0) {
        if (error) *error = "exFAT cluster count is zero";
        return false;
    }
    if (out.fat_length == 0 || out.fat_offset == 0) {
        if (error) *error = "exFAT FAT geometry is zero";
        return false;
    }

    out.valid = true;
    return true;
}

// ---------------------------------------------------------------------------
// Text / timestamp helpers
// ---------------------------------------------------------------------------
static std::u16string oem_to_utf16(const std::string& s) {
    if (s.empty()) return u"";
    int n = MultiByteToWideChar(CP_OEMCP, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return u"";
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_OEMCP, 0, s.data(), static_cast<int>(s.size()), &w[0], n);
    return std::u16string(w.begin(), w.end());
}

static int64_t dos_to_filetime(uint16_t date, uint16_t time) {
    int year = 1980 + (date >> 9);
    int month = (date >> 5) & 0x0F;
    int day = date & 0x1F;
    if (year < 1980 || month < 1 || month > 12 || day < 1 || day > 31) return 0;
    SYSTEMTIME st{};
    st.wYear = static_cast<WORD>(year);
    st.wMonth = static_cast<WORD>(month);
    st.wDay = static_cast<WORD>(day);
    st.wHour = static_cast<WORD>((time >> 11) & 0x1F);
    st.wMinute = static_cast<WORD>((time >> 5) & 0x3F);
    st.wSecond = static_cast<WORD>((time & 0x1F) * 2);
    SYSTEMTIME utc{};
    FILETIME ft{};
    if (!TzSpecificLocalTimeToSystemTime(nullptr, &st, &utc)) return 0;
    if (!SystemTimeToFileTime(&utc, &ft)) return 0;
    return (static_cast<int64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

// ---------------------------------------------------------------------------
// FAT chain helpers (12/16/32-bit)
// ---------------------------------------------------------------------------
static uint32_t fat_next(const std::vector<uint8_t>& t, FsType type, uint32_t c) {
    if (type == FsType::FAT12) {
        uint32_t off = c + c / 2;
        if (off + 1 >= t.size()) return 0x0FFF;
        uint16_t v = le16(&t[off]);
        return (c & 1) ? (v >> 4) : (v & 0x0FFF);
    }
    if (type == FsType::FAT16) {
        uint32_t off = c * 2;
        if (off + 1 >= t.size()) return 0xFFFF;
        return le16(&t[off]);
    }
    uint32_t off = c * 4;
    if (off + 3 >= t.size()) return 0x0FFFFFFF;
    return le32(&t[off]) & 0x0FFFFFFF;
}

static bool fat_eoc(FsType type, uint32_t v) {
    if (type == FsType::FAT12) return v >= 0x0FF8;
    if (type == FsType::FAT16) return v >= 0xFFF8;
    return v >= 0x0FFFFFF8;
}

// ---------------------------------------------------------------------------
// exFAT chain helpers (32-bit)
// ---------------------------------------------------------------------------
static uint32_t exfat_next(const std::vector<uint8_t>& t, uint32_t c) {
    uint32_t off = c * 4;
    if (off + 3 >= t.size()) return 0xFFFFFFFF;
    return le32(&t[off]);
}
static bool exfat_eoc(uint32_t v) { return v >= 0xFFFFFFF8; }

// ---------------------------------------------------------------------------
// Directory reading
// ---------------------------------------------------------------------------
// Byte offset (volume-relative) of a data cluster (cluster numbering starts at 2).
static uint64_t cluster_byte(uint64_t first_data_cluster, uint64_t cs, uint32_t cl) {
    return (first_data_cluster + static_cast<uint64_t>(cl) - 2) * cs;
}

// Read a cluster-chain directory into `raw`, stopping at the 0x00 end marker.
template <typename Next, typename Eoc>
static bool read_dir_chain(DiskReader& reader, uint64_t first_data_cluster, uint64_t cs,
                           uint32_t first_cluster, Next next, Eoc eoc,
                           std::vector<uint8_t>& raw, const std::atomic<bool>& stop,
                           std::string* error) {
    raw.clear();
    std::vector<uint8_t> buf(static_cast<size_t>(cs));
    uint32_t cl = first_cluster;
    uint64_t guard = 0;
    while (cl >= 2 && cl < 0x0FFFFFF0 && guard++ < 1'000'000) {
        if (stop.load()) return false;
        if (!reader.read(cluster_byte(first_data_cluster, cs, cl), buf.data(), cs)) {
            if (error) *error = "failed to read directory cluster";
            return false;
        }
        // Locate the 0x00 end marker within this cluster (entries are 32 bytes).
        size_t take = 0;
        bool end = false;
        for (size_t i = 0; i < static_cast<size_t>(cs); i += 32) {
            if (buf[i] == 0x00) { end = true; break; }
            take = i + 32;
        }
        size_t old = raw.size();
        raw.resize(old + take);
        if (take) std::memcpy(raw.data() + old, buf.data(), take);
        if (end) break;
        uint32_t nxt = next(cl);
        if (eoc(nxt)) break;
        cl = nxt;
    }
    return true;
}

// Convert an ordered list of data clusters (cluster numbering ≥ 2) into merged
// DataRuns, mapping to absolute volume clusters via `first_data_cluster`.
static std::vector<DataRun> clusters_to_runs(const std::vector<uint32_t>& clusters,
                                             uint64_t first_data_cluster) {
    std::vector<DataRun> runs;
    for (uint32_t c : clusters) {
        bool sparse = (c < 2);
        uint64_t lcn = sparse ? 0 : (first_data_cluster + static_cast<uint64_t>(c) - 2);
        if (!runs.empty() && !runs.back().sparse && !sparse &&
            static_cast<uint64_t>(runs.back().lcn) + runs.back().length == lcn) {
            runs.back().length++;
        } else {
            DataRun r;
            r.lcn = static_cast<int64_t>(lcn);
            r.length = 1;
            r.sparse = sparse;
            runs.push_back(r);
        }
    }
    return runs;
}

// ---------------------------------------------------------------------------
// FAT directory entry parsing
// ---------------------------------------------------------------------------
struct FatDirEntry {
    std::u16string name;
    bool deleted = false;
    bool is_directory = false;
    uint32_t first_cluster = 0;
    uint32_t file_size = 0;
    int64_t mtime = 0;
};

static std::u16string lfn_chars(const uint8_t* e) {
    std::u16string s;
    s.reserve(13);
    const uint8_t* pos[3] = { e + 1, e + 14, e + 28 };
    const int len[3] = { 5, 6, 2 };
    for (int j = 0; j < 3; j++) {
        for (int k = 0; k < len[j]; k++) {
            uint16_t c = le16(pos[j] + k * 2);
            if (c == 0) return s;
            if (c != 0xFFFF) s.push_back(static_cast<char16_t>(c));
        }
    }
    return s;
}

static std::u16string sfn_name(const uint8_t* e, bool deleted) {
    std::string name;
    name.reserve(12);
    char buf[9] = {0};
    std::memcpy(buf, e, 8);
    if (e[0] == 0x05) buf[0] = static_cast<char>(0xE5);
    if (deleted) buf[0] = '_';
    int nl = 8; while (nl > 0 && buf[nl - 1] == ' ') nl--;
    name.append(buf, static_cast<size_t>(nl));
    char ext[4] = {0};
    std::memcpy(ext, e + 8, 3);
    int el = 3; while (el > 0 && ext[el - 1] == ' ') el--;
    if (el > 0) { name.push_back('.'); name.append(ext, static_cast<size_t>(el)); }
    return oem_to_utf16(name);
}

static std::vector<FatDirEntry> parse_fat_entries(const std::vector<uint8_t>& raw,
                                                  const std::atomic<bool>& stop) {
    std::vector<FatDirEntry> out;
    std::vector<std::pair<uint8_t, std::u16string>> lfns;  // (ordinal, chars)
    for (size_t i = 0; i + 31 < raw.size(); i += 32) {
        if (stop.load()) break;
        const uint8_t* e = raw.data() + i;
        uint8_t b0 = e[0];
        if (b0 == 0x00) break;
        uint8_t attr = e[11];

        if (attr == 0x0F) {
            // LFN entry. Deleted/orphaned (0xE5 or 0x80 bit) → discard the name.
            if (b0 == 0xE5 || (b0 & 0x80)) { lfns.clear(); continue; }
            lfns.push_back({ static_cast<uint8_t>(b0 & 0x1F), lfn_chars(e) });
            continue;
        }

        FatDirEntry de;
        de.deleted = (b0 == 0xE5);
        de.is_directory = (attr & 0x10) != 0;
        bool is_volume = (attr & 0x08) != 0;
        de.first_cluster = le16(e + 0x1A) | (static_cast<uint32_t>(le16(e + 0x14)) << 16);
        de.file_size = le32(e + 0x1C);
        de.mtime = dos_to_filetime(le16(e + 0x18), le16(e + 0x16));

        if (is_volume) { lfns.clear(); continue; }

        if (!lfns.empty()) {
            std::sort(lfns.begin(), lfns.end(),
                      [](const auto& a, const auto& b) { return a.first < b.first; });
            de.name.clear();
            for (const auto& p : lfns) de.name += p.second;
        } else {
            de.name = sfn_name(e, de.deleted);
        }
        lfns.clear();

        if (de.name == u"." || de.name == u"..") continue;
        out.push_back(std::move(de));
    }
    return out;
}

// ---------------------------------------------------------------------------
// FAT tree scan
// ---------------------------------------------------------------------------
struct FatState {
    DiskReader& reader;
    const FatBootInfo& fat;
    const std::vector<uint8_t>& fat_table;
    const std::atomic<bool>& stop;

    uint64_t next_id = 6;   // root is id 5 (matches NTFS root)
    std::vector<FileNodePtr> nodes;
    uint64_t total_files = 0;
    uint64_t deleted_files = 0;
    uint64_t directories = 0;
};

static std::vector<DataRun> fat_file_runs(FatState& st, uint32_t first_cluster,
                                          uint32_t size, bool deleted) {
    uint64_t cs = st.fat.cluster_size();
    uint64_t need = (static_cast<uint64_t>(size) + cs - 1) / cs;
    std::vector<uint32_t> clusters;
    clusters.reserve(static_cast<size_t>(need));

    if (deleted) {
        // The FAT chain was zeroed on deletion; assume the file was contiguous.
        for (uint64_t i = 0; i < need; i++)
            clusters.push_back(static_cast<uint32_t>(first_cluster + i));
    } else {
        uint32_t cl = first_cluster;
        uint64_t guard = 0;
        while (cl >= 2 && cl < 0x0FFFFFF0 && clusters.size() < need && guard++ < need + 64) {
            clusters.push_back(cl);
            uint32_t nxt = fat_next(st.fat_table, st.fat.type, cl);
            if (fat_eoc(st.fat.type, nxt)) break;
            cl = nxt;
        }
    }
    return clusters_to_runs(clusters, st.fat.first_data_cluster());
}

static void fat_scan_dir(FatState& st, uint32_t first_cluster, uint64_t fixed_entries,
                         FileNodePtr parent, std::unordered_set<uint32_t>& ancestors) {
    if (st.stop.load() || st.nodes.size() > 2'000'000) return;

    std::vector<uint8_t> raw;
    const bool fixed_root = (first_cluster == 0);
    if (fixed_root) {
        // FAT12/16 fixed root directory (no cluster number to cycle-guard).
        uint64_t nbytes = fixed_entries * 32;
        uint64_t off = (static_cast<uint64_t>(st.fat.reserved_sectors) +
                        static_cast<uint64_t>(st.fat.num_fats) * st.fat.fat_size_sectors()) *
                       st.fat.bytes_per_sector;
        raw.resize(static_cast<size_t>(nbytes));
        if (!st.reader.read(off, raw.data(), nbytes)) return;
    } else {
        // Cycle guard on the ancestor chain, not a global visited set: a deleted
        // directory entry and a live one may both point at the same (reused)
        // cluster and must both be scanned; only a cluster that appears among
        // its own ancestors is a true loop.
        if (!ancestors.insert(first_cluster).second) return;
        if (!read_dir_chain(st.reader, st.fat.first_data_cluster(), st.fat.cluster_size(),
                            first_cluster,
                            [&](uint32_t c) { return fat_next(st.fat_table, st.fat.type, c); },
                            [&](uint32_t v) { return fat_eoc(st.fat.type, v); },
                            raw, st.stop, nullptr)) {
            ancestors.erase(first_cluster);
            return;
        }
    }

    auto entries = parse_fat_entries(raw, st.stop);
    for (auto& e : entries) {
        if (st.stop.load()) break;
        auto node = std::make_shared<FileNode>();
        node->mft_id = st.next_id++;
        node->parent_mft_id = parent ? parent->mft_id : 0;
        node->name = e.name;
        node->is_directory = e.is_directory;
        node->is_deleted = e.deleted;
        node->size = e.file_size;
        node->modified_time = e.mtime;

        if (!e.is_directory && e.file_size > 0 && e.first_cluster >= 2) {
            node->has_data = true;
            node->recoverable = true;
            node->data_runs = fat_file_runs(st, e.first_cluster, e.file_size, e.deleted);
        }

        st.nodes.push_back(node);
        parent->children.push_back(node);

        if (e.is_directory) {
            st.directories++;
            if (e.first_cluster >= 2) fat_scan_dir(st, e.first_cluster, 0, node, ancestors);
        } else {
            st.total_files++;
            if (e.deleted) st.deleted_files++;
        }
    }

    if (!fixed_root) ancestors.erase(first_cluster);
}

bool scan_fat_volume(DiskReader& reader, const FatBootInfo& fat, FatScanResult& out,
                     const std::atomic<bool>& stop, std::string* error) {
    reader.boot().bytes_per_sector = fat.bytes_per_sector;
    reader.boot().sectors_per_cluster = fat.sectors_per_cluster;

    std::vector<uint8_t> fat_table;
    uint64_t fat_bytes = fat.fat_size_sectors() * fat.bytes_per_sector;
    fat_table.resize(static_cast<size_t>(fat_bytes));
    uint64_t fat_off = static_cast<uint64_t>(fat.reserved_sectors) * fat.bytes_per_sector;
    if (!reader.read(fat_off, fat_table.data(), fat_bytes)) {
        if (error) *error = "failed to read FAT table";
        return false;
    }

    auto root = std::make_shared<FileNode>();
    root->mft_id = 5;
    root->is_directory = true;

    FatState st{reader, fat, fat_table, stop};
    st.nodes.push_back(root);

    std::unordered_set<uint32_t> ancestors;
    if (fat.type == FsType::FAT32) {
        fat_scan_dir(st, fat.root_cluster, 0, root, ancestors);
    } else {
        fat_scan_dir(st, 0, fat.root_entry_count, root, ancestors);
    }

    if (stop.load()) return false;

    out.root = root;
    out.nodes = std::move(st.nodes);
    out.total_files = st.total_files;
    out.deleted_files = st.deleted_files;
    out.directories = st.directories;
    return true;
}

// ---------------------------------------------------------------------------
// exFAT directory entry parsing
// ---------------------------------------------------------------------------
struct ExfatEntry {
    std::u16string name;
    bool deleted = false;
    bool is_directory = false;
    uint32_t first_cluster = 0;
    uint64_t data_length = 0;
    bool no_fat_chain = false;
};

struct ExfatState {
    DiskReader& reader;
    const ExfatBootInfo& exfat;
    const std::vector<uint8_t>& fat_table;
    const std::atomic<bool>& stop;

    uint64_t next_id = 6;
    std::vector<FileNodePtr> nodes;
    uint64_t total_files = 0;
    uint64_t deleted_files = 0;
    uint64_t directories = 0;
};

static std::vector<DataRun> exfat_file_runs(ExfatState& st, const ExfatEntry& e) {
    uint64_t cs = st.exfat.cluster_size();
    uint64_t need = (e.data_length + cs - 1) / cs;
    std::vector<uint32_t> clusters;
    clusters.reserve(static_cast<size_t>(need));

    if (e.deleted || e.no_fat_chain) {
        // Deleted entries lose their FAT chain; NoFatChain entries are already
        // contiguous. Both reduce to the contiguity assumption.
        for (uint64_t i = 0; i < need; i++)
            clusters.push_back(static_cast<uint32_t>(e.first_cluster + i));
    } else {
        uint32_t cl = e.first_cluster;
        uint64_t guard = 0;
        while (cl >= 2 && cl < 0x0FFFFFF0 && clusters.size() < need && guard++ < need + 64) {
            clusters.push_back(cl);
            uint32_t nxt = exfat_next(st.fat_table, cl);
            if (exfat_eoc(nxt)) break;
            cl = nxt;
        }
    }
    return clusters_to_runs(clusters, st.exfat.first_data_cluster());
}

static void exfat_scan_dir(ExfatState& st, uint32_t first_cluster, FileNodePtr parent,
                           std::unordered_set<uint32_t>& ancestors) {
    if (st.stop.load() || st.nodes.size() > 2'000'000) return;
    // Cycle guard on the ancestor chain (see fat_scan_dir): a deleted entry and
    // a live entry can share a reused cluster and must both be scanned; only an
    // ancestor self-reference is a real loop.
    if (!ancestors.insert(first_cluster).second) return;

    std::vector<uint8_t> raw;
    if (!read_dir_chain(st.reader, st.exfat.first_data_cluster(), st.exfat.cluster_size(),
                        first_cluster,
                        [&](uint32_t c) { return exfat_next(st.fat_table, c); },
                        [&](uint32_t v) { return exfat_eoc(v); },
                        raw, st.stop, nullptr)) {
        ancestors.erase(first_cluster);
        return;
    }

    for (size_t i = 0; i + 31 < raw.size(); ) {
        if (st.stop.load()) break;
        const uint8_t* e = raw.data() + i;
        uint8_t type = e[0];
        if (type == 0x00) break;
        uint8_t code = type & 0x7F;

        if (code == 0x05) {
            // File directory entry (primary). Secondary count N ⇒ 1 stream
            // extension + (N-1) file-name entries immediately follow.
            bool in_use = (type & 0x80) != 0;
            uint8_t sec_count = e[1];
            uint16_t attrs = le16(e + 4);
            ExfatEntry ent;
            ent.deleted = !in_use;
            ent.is_directory = (attrs & 0x10) != 0;

            if (sec_count >= 1 && i + 64 <= raw.size()) {
                const uint8_t* se = raw.data() + i + 32;
                if ((se[0] & 0x7F) == 0x40) {  // stream extension
                    uint8_t flags = se[1];
                    ent.no_fat_chain = (flags & 0x02) != 0;
                    uint8_t name_len = se[3];
                    ent.first_cluster = le32(se + 20);
                    ent.data_length = le64(se + 24);
                    uint8_t name_entries = sec_count - 1;
                    uint8_t chars_read = 0;
                    for (uint8_t n = 0; n < name_entries && chars_read < name_len; n++) {
                        if (i + 64 + n * 32 + 31 >= raw.size()) break;
                        const uint8_t* ne = raw.data() + i + 64 + n * 32;
                        if ((ne[0] & 0x7F) != 0x41) break;  // file name entry
                        for (int k = 0; k < 15 && chars_read < name_len; k++) {
                            ent.name.push_back(static_cast<char16_t>(le16(ne + 2 + k * 2)));
                            chars_read++;
                        }
                    }
                }
            }

            if (ent.name == u"." || ent.name == u"..") {
                i += 32 * (1 + sec_count);
                continue;
            }

            auto node = std::make_shared<FileNode>();
            node->mft_id = st.next_id++;
            node->parent_mft_id = parent ? parent->mft_id : 0;
            node->name = ent.name;
            node->is_directory = ent.is_directory;
            node->is_deleted = ent.deleted;
            node->size = ent.data_length;

            if (!ent.is_directory && ent.data_length > 0 && ent.first_cluster >= 2) {
                node->has_data = true;
                node->recoverable = true;
                node->data_runs = exfat_file_runs(st, ent);
            }

            st.nodes.push_back(node);
            parent->children.push_back(node);

            if (ent.is_directory) {
                st.directories++;
                if (ent.first_cluster >= 2) exfat_scan_dir(st, ent.first_cluster, node, ancestors);
            } else {
                st.total_files++;
                if (ent.deleted) st.deleted_files++;
            }

            i += 32 * (1 + sec_count);
        } else {
            // Volume label / bitmap / up-case table / vendor entries: skip.
            i += 32;
        }
    }

    ancestors.erase(first_cluster);
}

bool scan_exfat_volume(DiskReader& reader, const ExfatBootInfo& exfat, FatScanResult& out,
                       const std::atomic<bool>& stop, std::string* error) {
    reader.boot().bytes_per_sector = static_cast<uint16_t>(exfat.bytes_per_sector());
    // Note: sectors_per_cluster can exceed 255 for exFAT (e.g. 128 KiB clusters =
    // 256 sectors), so we don't store it in boot_. The scan uses the exFAT
    // cluster size directly (read_dir_chain / file runs), and recovery reads it
    // back from the task's stored cluster_size via set_cluster_size().
    reader.boot().sectors_per_cluster =
        static_cast<uint8_t>(std::min<uint64_t>(exfat.sectors_per_cluster(), 255));

    std::vector<uint8_t> fat_table;
    uint64_t fat_bytes = static_cast<uint64_t>(exfat.fat_length) * exfat.bytes_per_sector();
    fat_table.resize(static_cast<size_t>(fat_bytes));
    uint64_t fat_off = static_cast<uint64_t>(exfat.fat_offset) * exfat.bytes_per_sector();
    if (!reader.read(fat_off, fat_table.data(), fat_bytes)) {
        if (error) *error = "failed to read exFAT FAT table";
        return false;
    }

    auto root = std::make_shared<FileNode>();
    root->mft_id = 5;
    root->is_directory = true;

    ExfatState st{reader, exfat, fat_table, stop};
    st.nodes.push_back(root);
    std::unordered_set<uint32_t> ancestors;
    exfat_scan_dir(st, exfat.root_cluster, root, ancestors);

    if (stop.load()) return false;

    out.root = root;
    out.nodes = std::move(st.nodes);
    out.total_files = st.total_files;
    out.deleted_files = st.deleted_files;
    out.directories = st.directories;
    return true;
}

} // namespace recovery
