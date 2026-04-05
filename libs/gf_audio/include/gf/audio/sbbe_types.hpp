#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace gf::audio {

struct SbbeSlot {
    int      index       = 0;
    uint32_t globalId    = 0;
    uint64_t slotOffset  = 0;  // file offset of "SLOT" magic
    uint32_t sizeField   = 0;  // value at slot_offset+4
    uint64_t ramOffset   = 0;  // file offset of ".RAM" magic
    uint32_t sampleRate  = 44100;

    [[nodiscard]] uint64_t audioStart() const { return ramOffset + 64; }
    [[nodiscard]] uint64_t audioEnd()   const { return slotOffset + 4 + sizeField; }
    [[nodiscard]] uint64_t audioBytes() const {
        const uint64_t e = audioEnd(), s = audioStart();
        return (e > s) ? (e - s) : 0u;
    }
    [[nodiscard]] uint64_t sampleCount() const { return audioBytes() / 2; }
    [[nodiscard]] double   duration()    const {
        return sampleRate > 0u ? static_cast<double>(sampleCount()) / sampleRate : 0.0;
    }
};

struct SbbeFile {
    std::string            filePath;
    std::string            baseName;
    uint32_t               hashField = 0;
    std::vector<SbbeSlot>  audioSlots;  // renamed from 'slots' — Qt macro conflict
    bool                   valid = false;
};

} // namespace gf::audio
