#include "data_run.h"

namespace recovery {

bool DataRunParser::parse(const uint8_t* data, size_t size,
                          std::vector<DataRun>& runs,
                          uint64_t* total_clusters) {
    runs.clear();
    if (total_clusters) *total_clusters = 0;

    size_t pos = 0;
    int64_t lcn = 0;
    uint64_t total = 0;

    while (pos < size) {
        uint8_t header = data[pos];
        if (header == 0) break; // end of run list

        uint8_t len_bytes = header & 0x0F;   // length field size
        uint8_t off_bytes = (header >> 4) & 0x0F; // offset field size
        pos++;

        if (pos + len_bytes + off_bytes > size) return false;

        // Length (unsigned, clusters).
        uint64_t length = 0;
        for (uint8_t i = 0; i < len_bytes; i++) {
            length |= static_cast<uint64_t>(data[pos + i]) << (8 * i);
        }
        pos += len_bytes;

        // Offset (signed, relative LCN delta).
        int64_t delta = 0;
        if (off_bytes > 0) {
            for (uint8_t i = 0; i < off_bytes; i++) {
                delta |= static_cast<int64_t>(data[pos + i]) << (8 * i);
            }
            // Sign-extend.
            if (off_bytes < 8 && (data[pos + off_bytes - 1] & 0x80)) {
                delta |= (~0LL) << (8 * off_bytes);
            }
        }
        pos += off_bytes;

        DataRun run;
        if (off_bytes == 0) {
            // Sparse run: no physical clusters.
            run.lcn = 0;
            run.sparse = true;
        } else {
            lcn += delta;
            run.lcn = lcn;
        }
        run.length = length;
        runs.push_back(run);
        total += length;
    }

    if (total_clusters) *total_clusters = total;
    return true;
}

} // namespace recovery
