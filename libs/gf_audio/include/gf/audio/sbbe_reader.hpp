#pragma once
#include "sbbe_types.hpp"
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace gf::audio {

class SbbeReader {
public:
    static std::optional<SbbeFile>    read(const std::string& path,
                                           std::string*       errorOut = nullptr);

    // Read raw 16-bit signed PCM samples for one SLOT
    static std::vector<int16_t>       readSlotAudio(const std::string& path,
                                                    const SbbeSlot&    slot,
                                                    std::string*       errorOut = nullptr);
};

} // namespace gf::audio
