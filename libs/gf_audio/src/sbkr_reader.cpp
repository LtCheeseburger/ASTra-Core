#include "gf/audio/sbkr_reader.hpp"

#include <cstring>
#include <fstream>
#include <stdexcept>

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

} // anonymous namespace

std::optional<SbkrFile> SbkrReader::read(const std::string& path, std::string* errorOut)
{
    // --- Read entire file ---
    std::ifstream ifs(path, std::ios::binary | std::ios::ate);
    if (!ifs.is_open()) {
        if (errorOut) *errorOut = "Cannot open file: " + path;
        return std::nullopt;
    }

    const auto fileSize = static_cast<std::size_t>(ifs.tellg());
    if (fileSize < 0xB0 + 8) {
        if (errorOut) *errorOut = "File too small to be a valid SBKR: " + path;
        return std::nullopt;
    }

    ifs.seekg(0);
    std::vector<uint8_t> buf(fileSize);
    if (!ifs.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(fileSize))) {
        if (errorOut) *errorOut = "Read error: " + path;
        return std::nullopt;
    }

    // --- Validate magic "SBKR" ---
    if (buf[0] != 'S' || buf[1] != 'B' || buf[2] != 'K' || buf[3] != 'R') {
        if (errorOut) *errorOut = "Not a SBKR file (bad magic): " + path;
        return std::nullopt;
    }

    // --- Read header fields ---
    const uint32_t timestamp  = readBE32(buf.data() + 0x14);
    const uint32_t entryCount = readBE32(buf.data() + 0x1C);

    // --- Find "SNS1" starting from offset 0xB0 ---
    std::size_t sns1Pos = std::string::npos;
    const std::size_t searchEnd = (fileSize >= 4) ? (fileSize - 4) : 0;
    for (std::size_t i = 0xB0; i <= searchEnd; ++i) {
        if (buf[i] == 'S' && buf[i+1] == 'N' && buf[i+2] == 'S' && buf[i+3] == '1') {
            sns1Pos = i;
            break;
        }
    }

    if (sns1Pos == std::string::npos) {
        if (errorOut) *errorOut = "SNS1 section not found in: " + path;
        return std::nullopt;
    }

    // Records start immediately after "SNS1" (4) + count_byte (1) + 3 bytes padding = 8 bytes
    const std::size_t recordsStart = sns1Pos + 8;

    // Bounds check: need entryCount * 10 bytes
    const std::size_t totalRecordBytes = static_cast<std::size_t>(entryCount) * 10u;
    if (recordsStart + totalRecordBytes > fileSize) {
        if (errorOut) *errorOut = "Record data extends past end of file: " + path;
        return std::nullopt;
    }

    // --- Parse records ---
    SbkrFile result;
    result.filePath   = path;
    result.baseName   = basenameNoExt(path);
    result.timestamp  = timestamp;
    result.entryCount = entryCount;
    result.entries.reserve(entryCount);

    for (uint32_t i = 0; i < entryCount; ++i) {
        const uint8_t* rec = buf.data() + recordsStart + (static_cast<std::size_t>(i) * 10u);
        SbkrEntry entry;
        entry.soundId = readBE16(rec + 0);
        // rec[2] = 0x00, rec[3] = 0x02, rec[4] = 0x00 (expected, ignored)
        entry.flags = rec[5];
        std::memcpy(entry.extra, rec + 6, 4);
        result.entries.push_back(entry);
    }

    result.valid = true;
    return result;
}

} // namespace gf::audio
