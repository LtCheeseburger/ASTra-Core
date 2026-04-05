#include "gf/audio/sbbe_reader.hpp"

#include <cstring>
#include <fstream>
#include <limits>

namespace gf::audio {

namespace {

[[nodiscard]] uint16_t readBE16(const uint8_t* p) noexcept
{
    return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8u) | p[1]);
}

[[nodiscard]] uint32_t readBE32(const uint8_t* p) noexcept
{
    return (static_cast<uint32_t>(p[0]) << 24u)
         | (static_cast<uint32_t>(p[1]) << 16u)
         | (static_cast<uint32_t>(p[2]) <<  8u)
         |  static_cast<uint32_t>(p[3]);
}

[[nodiscard]] std::string basenameNoExt(const std::string& path)
{
    const std::size_t slash = path.find_last_of("/\\");
    std::string name = (slash == std::string::npos) ? path : path.substr(slash + 1);
    const std::size_t dot = name.rfind('.');
    if (dot != std::string::npos)
        name.resize(dot);
    return name;
}

// Search buf[start..end) for a 4-byte magic sequence.
// Returns the offset of the first match, or std::string::npos if not found.
[[nodiscard]] std::size_t findMagic4(const std::vector<uint8_t>& buf,
                                     std::size_t start,
                                     std::size_t end,
                                     uint8_t a, uint8_t b, uint8_t c, uint8_t d)
{
    if (end > buf.size()) end = buf.size();
    if (end < 4) return std::string::npos;
    const std::size_t limit = end - 4;
    for (std::size_t i = start; i <= limit; ++i) {
        if (buf[i] == a && buf[i+1] == b && buf[i+2] == c && buf[i+3] == d)
            return i;
    }
    return std::string::npos;
}

} // anonymous namespace

std::optional<SbbeFile> SbbeReader::read(const std::string& path, std::string* errorOut)
{
    // --- Read entire file ---
    std::ifstream ifs(path, std::ios::binary | std::ios::ate);
    if (!ifs.is_open()) {
        if (errorOut) *errorOut = "Cannot open file: " + path;
        return std::nullopt;
    }

    const auto fileSize = static_cast<std::size_t>(ifs.tellg());
    if (fileSize < 16) {
        if (errorOut) *errorOut = "File too small to be a valid SBbe: " + path;
        return std::nullopt;
    }

    ifs.seekg(0);
    std::vector<uint8_t> buf(fileSize);
    if (!ifs.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(fileSize))) {
        if (errorOut) *errorOut = "Read error: " + path;
        return std::nullopt;
    }

    // --- Validate magic "SBbe" ---
    if (buf[0] != 'S' || buf[1] != 'B' || buf[2] != 'b' || buf[3] != 'e') {
        if (errorOut) *errorOut = "Not a SBbe file (bad magic): " + path;
        return std::nullopt;
    }

    SbbeFile result;
    result.filePath = path;
    result.baseName = basenameNoExt(path);

    // hashField at 0x0C
    if (fileSize >= 0x10)
        result.hashField = readBE32(buf.data() + 0x0C);

    // --- Scan for SLOT sections ---
    // Search entire file for "SLOT" magic (0x534C4F54)
    std::size_t searchPos = 0;
    while (searchPos + 4 <= fileSize) {
        const std::size_t slotPos = findMagic4(buf, searchPos, fileSize,
                                               'S', 'L', 'O', 'T');
        if (slotPos == std::string::npos)
            break;

        // Read sizeField at slotPos+4 (big-endian uint32)
        if (slotPos + 8 > fileSize) {
            searchPos = slotPos + 1;
            continue;
        }
        const uint32_t sizeField = readBE32(buf.data() + slotPos + 4);

        // Find ".RAM" magic starting at slotPos+8, within next 64 bytes
        const std::size_t ramSearchEnd = slotPos + 8 + 64;
        const std::size_t ramPos = findMagic4(buf, slotPos + 8, ramSearchEnd,
                                              '.', 'R', 'A', 'M');
        if (ramPos == std::string::npos) {
            searchPos = slotPos + 4;
            continue;
        }

        // sub_hdr_start = ramPos + 8
        // sampleRate = readBE16 at sub_hdr_start + 14 = ramPos + 22
        if (ramPos + 23 > fileSize) {
            searchPos = slotPos + 4;
            continue;
        }
        const uint32_t sampleRate = readBE16(buf.data() + ramPos + 22);

        SbbeSlot slot;
        slot.slotOffset = static_cast<uint64_t>(slotPos);
        slot.sizeField  = sizeField;
        slot.ramOffset  = static_cast<uint64_t>(ramPos);
        slot.sampleRate = (sampleRate > 0) ? sampleRate : 44100u;
        slot.index      = static_cast<int>(result.audioSlots.size());

        // Validate: audioBytes > 0 and audioEnd <= fileSize
        const uint64_t aEnd   = slot.audioEnd();
        const uint64_t aStart = slot.audioStart();
        if (aEnd > static_cast<uint64_t>(fileSize) || aEnd <= aStart) {
            // invalid or degenerate slot — skip
            searchPos = slotPos + 4;
            continue;
        }

        result.audioSlots.push_back(slot);
        // Advance past the entire SLOT section to avoid false positives inside audio.
        // audio_end gives the precise end of audio; section ends at same point.
        searchPos = static_cast<std::size_t>(aEnd);
    }

    // --- Find DSET → .SID → global IDs ---
    const std::size_t dsetPos = findMagic4(buf, 0, fileSize, 'D', 'S', 'E', 'T');
    if (dsetPos != std::string::npos) {
        // Find ".SID" within 512 bytes of DSET
        const std::size_t sidSearchEnd = dsetPos + 512;
        const std::size_t sidPos = findMagic4(buf, dsetPos, sidSearchEnd,
                                              '.', 'S', 'I', 'D');
        if (sidPos != std::string::npos) {
            // Global ID pointer at sidPos + 0x10
            if (sidPos + 0x14 <= fileSize) {
                const uint32_t idsPtr = readBE32(buf.data() + sidPos + 0x10);
                const std::size_t idsOffset = static_cast<std::size_t>(idsPtr);
                const std::size_t slotCount = result.audioSlots.size();
                // Read slotCount × uint32 from idsOffset
                if (idsOffset + slotCount * 4u <= fileSize) {
                    for (std::size_t i = 0; i < slotCount; ++i) {
                        result.audioSlots[i].globalId =
                            readBE32(buf.data() + idsOffset + i * 4u);
                    }
                }
            }
        }
    }

    result.valid = true;
    return result;
}

std::vector<int16_t> SbbeReader::readSlotAudio(const std::string& path,
                                                const SbbeSlot&    slot,
                                                std::string*       errorOut)
{
    std::ifstream ifs(path, std::ios::binary | std::ios::ate);
    if (!ifs.is_open()) {
        if (errorOut) *errorOut = "Cannot open file: " + path;
        return {};
    }

    const auto fileSize = static_cast<uint64_t>(ifs.tellg());
    const uint64_t aStart = slot.audioStart();
    const uint64_t aEnd   = slot.audioEnd();

    if (aStart >= aEnd || aEnd > fileSize) {
        if (errorOut) *errorOut = "Slot audio range out of file bounds";
        return {};
    }

    const uint64_t byteCount = aEnd - aStart;
    if (byteCount % 2u != 0u) {
        // Truncate to even number of bytes
    }
    const std::size_t sampleCount = static_cast<std::size_t>(byteCount / 2u);

    ifs.seekg(static_cast<std::streamoff>(aStart));
    std::vector<int16_t> samples(sampleCount);
    if (!ifs.read(reinterpret_cast<char*>(samples.data()),
                  static_cast<std::streamsize>(sampleCount * 2u))) {
        if (errorOut) *errorOut = "Read error while reading slot audio";
        return {};
    }

    // PCM data is big-endian signed 16-bit — byte-swap each sample to LE for playback.
    for (int16_t& s : samples) {
        const auto u = static_cast<uint16_t>(s);
        s = static_cast<int16_t>(static_cast<uint16_t>((u >> 8u) | (u << 8u)));
    }
    return samples;
}

} // namespace gf::audio
