#pragma once
// NTFS data run list parser.

#include <cstdint>
#include <vector>

#include "../include/types.h"

namespace recovery {

class DataRunParser {
public:
    // Parse a data run list starting at `data` (length `size` bytes).
    // On success returns true and fills `runs` (absolute LCNs) and the total
    // number of clusters spanned in `total_clusters`.
    static bool parse(const uint8_t* data, size_t size,
                      std::vector<DataRun>& runs,
                      uint64_t* total_clusters = nullptr);
};

} // namespace recovery
