#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace gf::audio {

struct SbkrEntry {
    uint16_t soundId = 0;
    uint8_t  flags   = 0;   // byte[5] of record: 0, 1, 2, or 7
    uint8_t  extra[4]{};    // bytes[6-9]: family/ref data
};

struct SbkrFile {
    std::string             filePath;
    std::string             baseName;   // filename without extension
    uint32_t                timestamp  = 0;
    uint32_t                entryCount = 0;  // from main header (authoritative)
    std::vector<SbkrEntry>  entries;
    bool                    valid = false;
};

} // namespace gf::audio
