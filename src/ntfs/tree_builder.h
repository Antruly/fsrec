#pragma once
// Rebuild the directory tree from a flat list of parsed MFT entries.

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "../include/types.h"

namespace recovery {

class TreeBuilder {
public:
    // Build the tree in place: links each node to its parent via
    // parent_mft_id. Returns the root node (mft_id 5), or nullptr if no root.
    // `nodes` is the flat list (also available via `by_id`).
    static FileNodePtr build(std::vector<FileNodePtr>& nodes,
                             std::map<uint64_t, FileNodePtr>& by_id);
};

} // namespace recovery
