#pragma once
// NTFS boot sector (BPB) parser.

#include <string>

#include "../include/types.h"
#include "disk_reader.h"

namespace recovery {

class BootParser {
public:
    // Parse the boot sector of the currently-open volume and populate `out`.
    // Returns true and sets out.valid on success.
    static bool parse(DiskReader& reader, std::string* error = nullptr);
};

} // namespace recovery
