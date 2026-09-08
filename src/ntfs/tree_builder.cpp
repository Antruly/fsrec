#include "tree_builder.h"

#include <algorithm>
#include <functional>

namespace recovery {

FileNodePtr TreeBuilder::build(std::vector<FileNodePtr>& nodes,
                               std::map<uint64_t, FileNodePtr>& by_id) {
    by_id.clear();
    for (auto& n : nodes) {
        if (n) by_id[n->mft_id] = n;
    }

    // Root directory is MFT record 5.
    auto it = by_id.find(5);
    if (it == by_id.end()) {
        // Fall back to the node named "." with parent == self, or the first node.
        for (auto& n : nodes) {
            if (n && (n->name == u"." || n->parent_mft_id == n->mft_id)) {
                by_id[5] = n;
                n->mft_id = 5;
                n->is_directory = true;
                it = by_id.find(5);
                break;
            }
        }
    }
    if (it == by_id.end()) return nullptr;

    FileNodePtr root = it->second;
    root->is_directory = true;
    root->parent_mft_id = 0;

    for (auto& n : nodes) {
        if (!n || n == root) continue;
        if (n->mft_id == 5) continue; // root handled above

        // Skip attaching a node to itself (would create a cycle).
        if (n->parent_mft_id == n->mft_id) {
            root->children.push_back(n);
            continue;
        }

        auto p = by_id.find(n->parent_mft_id);
        if (p != by_id.end() && p->second && p->second != n) {
            p->second->children.push_back(n);
        } else {
            // Orphan: attach to root.
            root->children.push_back(n);
        }
    }

    // Sort children: directories first, then case-insensitive name order.
    std::function<void(const FileNodePtr&)> sort_children =
        [&](const FileNodePtr& node) {
            std::sort(node->children.begin(), node->children.end(),
                      [](const FileNodePtr& a, const FileNodePtr& b) {
                          if (a->is_directory != b->is_directory)
                              return a->is_directory > b->is_directory;
                          return a->name < b->name;
                      });
            for (auto& c : node->children) sort_children(c);
        };
    sort_children(root);

    return root;
}

} // namespace recovery
