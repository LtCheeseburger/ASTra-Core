#include "gf/media/ea_vp6.hpp"

#include <cassert>
#include <cstring>
#include <iomanip>
#include <limits>
#include <sstream>

namespace gf::media {

// ─────────────────────────────────────────────────────────────────────────────
// Internal byte-read / byte-write helpers
// ─────────────────────────────────────────────────────────────────────────────

static std::uint16_t readU16le(const std::uint8_t* p) noexcept
{
    return static_cast<std::uint16_t>(p[0]) |
           (static_cast<std::uint16_t>(p[1]) << 8);
}

static std::uint32_t readU32le(const std::uint8_t* p) noexcept
{
    return static_cast<std::uint32_t>(p[0])        |
           (static_cast<std::uint32_t>(p[1]) <<  8) |
           (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}

static void writeU32le(std::uint8_t* p, std::uint32_t v) noexcept
{
    p[0] = static_cast<std::uint8_t>(v);
    p[1] = static_cast<std::uint8_t>(v >>  8);
    p[2] = static_cast<std::uint8_t>(v >> 16);
    p[3] = static_cast<std::uint8_t>(v >> 24);
}

// Formats a size_t as "0x<hex>" for error messages.
static std::string hexOffset(std::size_t v)
{
    std::ostringstream ss;
    ss << "0x" << std::hex << v;
    return ss.str();
}

// ─────────────────────────────────────────────────────────────────────────────
// Chunk kind mapping
// ─────────────────────────────────────────────────────────────────────────────

EaVp6ChunkKind chunkKindFromTag(const std::string& tag) noexcept
{
    if (tag == "MVhd") return EaVp6ChunkKind::MVhd;
    if (tag == "MV0K") return EaVp6ChunkKind::MV0K;
    if (tag == "MV0F") return EaVp6ChunkKind::MV0F;
    return EaVp6ChunkKind::Unknown;
}

std::string tagFromChunkKind(EaVp6ChunkKind kind) noexcept
{
    switch (kind) {
    case EaVp6ChunkKind::MVhd: return "MVhd";
    case EaVp6ChunkKind::MV0K: return "MV0K";
    case EaVp6ChunkKind::MV0F: return "MV0F";
    default:                   return "(unknown)";
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// MVhd payload decoder
// ─────────────────────────────────────────────────────────────────────────────

static EaVp6Header decodeMvhd(const std::vector<std::uint8_t>& payload,
                               std::vector<std::string>&        warnings)
{
    EaVp6Header h;

    if (payload.size() < 24) {
        warnings.push_back(
            "MVhd payload is " + std::to_string(payload.size()) +
            " bytes (expected >= 24); header metadata not decoded");
        return h;
    }

    const std::uint8_t* p = payload.data();

    h.codecId          = readU32le(p + 0x00);
    h.width            = readU16le(p + 0x04);
    h.height           = readU16le(p + 0x06);
    h.frameCount       = readU32le(p + 0x08);
    h.largestChunkSize = readU32le(p + 0x0C);
    h.fpsDenominator   = readU32le(p + 0x10);  // rate
    h.fpsNumerator     = readU32le(p + 0x14);  // scale

    if (h.fpsNumerator != 0) {
        h.fps = static_cast<double>(h.fpsDenominator) /
                static_cast<double>(h.fpsNumerator);
    } else if (h.fpsDenominator != 0) {
        warnings.push_back("MVhd fps numerator (scale) is zero; fps not computed");
    }

    h.valid = true;
    return h;
}

// ─────────────────────────────────────────────────────────────────────────────
// EaVp6Parser
// ─────────────────────────────────────────────────────────────────────────────

std::optional<EaVp6Movie> EaVp6Parser::parse(std::span<const std::uint8_t> data,
                                              std::string&                   errorOut) const
{
    if (data.empty()) {
        errorOut = "EA VP6 parse: empty input";
        return std::nullopt;
    }
    if (data.size() < 8) {
        errorOut = "EA VP6 parse: input too short (" +
                   std::to_string(data.size()) +
                   " bytes); minimum 8 bytes required for a chunk header";
        return std::nullopt;
    }

    EaVp6Movie movie;
    bool headerDecoded = false;
    std::size_t offset = 0;

    while (offset < data.size()) {
        const std::size_t remaining = data.size() - offset;

        // Need at least 8 bytes: 4 (tag) + 4 (total size field).
        if (remaining < 8) {
            errorOut = "EA VP6 parse: " + std::to_string(remaining) +
                       " trailing byte(s) at " + hexOffset(offset) +
                       " cannot form a valid chunk header (need 8)";
            return std::nullopt;
        }

        const std::uint8_t* base = data.data() + offset;

        // Read tag (4 ASCII bytes) and TOTAL chunk size (le_u32).
        // The size field includes the 8-byte header itself.
        // payload size = totalSize - 8.
        const std::string   tag(reinterpret_cast<const char*>(base), 4);
        const std::uint32_t totalSize = readU32le(base + 4);

        // totalSize must be at least 8 (the header) — a value < 8 is structurally
        // impossible and would underflow the payload size calculation.
        if (totalSize < 8) {
            errorOut = "EA VP6 parse: chunk '" + tag +
                       "' at " + hexOffset(offset) +
                       " declares total size " + std::to_string(totalSize) +
                       " which is less than the 8-byte chunk header minimum";
            return std::nullopt;
        }

        const std::uint32_t payloadSize = totalSize - 8u;

        // Validate payload fits within the remaining bytes after the header.
        // (remaining - 8) is safe because remaining >= 8 was checked above.
        if (static_cast<std::size_t>(payloadSize) > remaining - 8) {
            errorOut = "EA VP6 parse: chunk '" + tag +
                       "' at " + hexOffset(offset) +
                       " declares total size " + std::to_string(totalSize) +
                       " (payload " + std::to_string(payloadSize) +
                       ") but only " + std::to_string(remaining - 8) +
                       " byte(s) remain after the header";
            return std::nullopt;
        }

        EaVp6Chunk chunk;
        chunk.tag          = tag;
        chunk.kind         = chunkKindFromTag(tag);
        chunk.declaredSize = totalSize;   // raw field value — total chunk size
        chunk.fileOffset   = static_cast<std::uint64_t>(offset);
        chunk.payload.assign(base + 8, base + 8 + payloadSize);

        // Decode the first MVhd payload into EaVp6Header.
        if (chunk.kind == EaVp6ChunkKind::MVhd && !headerDecoded) {
            movie.header = decodeMvhd(chunk.payload, movie.warnings);
            headerDecoded = true;
        }

        movie.chunks.push_back(std::move(chunk));
        offset += static_cast<std::size_t>(totalSize);
    }

    if (movie.chunks.empty()) {
        errorOut = "EA VP6 parse: no chunks found in input";
        return std::nullopt;
    }

    return movie;
}

// ─────────────────────────────────────────────────────────────────────────────
// EaVp6Writer
// ─────────────────────────────────────────────────────────────────────────────

std::optional<std::vector<std::uint8_t>> EaVp6Writer::build(const EaVp6Movie& movie,
                                                              std::string&      errorOut) const
{
    if (movie.chunks.empty()) {
        errorOut = "EA VP6 write: movie has no chunks";
        return std::nullopt;
    }

    // Pre-flight validation: all tags must be exactly 4 chars; total chunk size
    // (8 + payload) must fit within a le_u32.
    for (std::size_t i = 0; i < movie.chunks.size(); ++i) {
        const auto& c = movie.chunks[i];
        if (c.tag.size() != 4) {
            errorOut = "EA VP6 write: chunk [" + std::to_string(i) +
                       "] has tag of length " + std::to_string(c.tag.size()) +
                       " ('" + c.tag + "'); expected exactly 4 characters";
            return std::nullopt;
        }
        // Total size = 8 + payload.size(); must fit in u32.
        if (c.payload.size() > static_cast<std::size_t>(
                std::numeric_limits<std::uint32_t>::max() - 8u)) {
            errorOut = "EA VP6 write: chunk [" + std::to_string(i) +
                       "] ('" + c.tag + "') payload size " +
                       std::to_string(c.payload.size()) +
                       " causes total chunk size to overflow u32";
            return std::nullopt;
        }
    }

    // Calculate total output size up-front to avoid repeated reallocations.
    std::size_t totalSize = 0;
    for (const auto& c : movie.chunks)
        totalSize += 8 + c.payload.size();  // 4 tag + 4 size + payload

    std::vector<std::uint8_t> out;
    out.reserve(totalSize);

    std::uint8_t hdr[8];
    for (const auto& c : movie.chunks) {
        // 4-byte tag
        std::memcpy(hdr, c.tag.data(), 4);
        // Total chunk size = 8 (header) + payload bytes.
        // This matches the real EA VP6 format where the size field includes the
        // header itself.  Using payload.size() (not declaredSize) as authoritative
        // normalises any malformed source and preserves round-trip byte equality.
        const std::uint32_t totalSize = 8u + static_cast<std::uint32_t>(c.payload.size());
        writeU32le(hdr + 4, totalSize);
        out.insert(out.end(), hdr, hdr + 8);
        out.insert(out.end(), c.payload.begin(), c.payload.end());
    }

    assert(out.size() == totalSize);
    return out;
}

// ─────────────────────────────────────────────────────────────────────────────
// Round-trip validation
// ─────────────────────────────────────────────────────────────────────────────

bool validateRoundTrip(std::span<const std::uint8_t> original, std::string& errorOut)
{
    const EaVp6Parser parser;
    const EaVp6Writer writer;

    // ── Step 1: parse original ────────────────────────────────────────────────
    std::string parseErr;
    auto movieOpt = parser.parse(original, parseErr);
    if (!movieOpt) {
        errorOut = "Round-trip [1/4]: initial parse failed: " + parseErr;
        return false;
    }
    const EaVp6Movie& movie = *movieOpt;

    // ── Step 2: rebuild bytes ─────────────────────────────────────────────────
    std::string buildErr;
    auto rebuiltOpt = writer.build(movie, buildErr);
    if (!rebuiltOpt) {
        errorOut = "Round-trip [2/4]: rebuild failed: " + buildErr;
        return false;
    }
    const std::vector<std::uint8_t>& rebuilt = *rebuiltOpt;

    // ── Step 3: re-parse rebuilt bytes ────────────────────────────────────────
    std::string reparseErr;
    auto movie2Opt = parser.parse(
        std::span<const std::uint8_t>(rebuilt.data(), rebuilt.size()), reparseErr);
    if (!movie2Opt) {
        errorOut = "Round-trip [3/4]: re-parse of rebuilt bytes failed: " + reparseErr;
        return false;
    }
    const EaVp6Movie& movie2 = *movie2Opt;

    // ── Step 4: structural verification ──────────────────────────────────────

    if (movie2.chunks.size() != movie.chunks.size()) {
        errorOut = "Round-trip [4/4]: chunk count mismatch: original=" +
                   std::to_string(movie.chunks.size()) +
                   " rebuilt=" + std::to_string(movie2.chunks.size());
        return false;
    }

    for (std::size_t i = 0; i < movie.chunks.size(); ++i) {
        const auto& a = movie.chunks[i];
        const auto& b = movie2.chunks[i];

        if (a.tag != b.tag) {
            errorOut = "Round-trip [4/4]: chunk [" + std::to_string(i) +
                       "] tag mismatch: '" + a.tag + "' vs '" + b.tag + "'";
            return false;
        }
        if (a.payload.size() != b.payload.size()) {
            errorOut = "Round-trip [4/4]: chunk [" + std::to_string(i) +
                       "] ('" + a.tag + "') payload size mismatch: " +
                       std::to_string(a.payload.size()) + " vs " +
                       std::to_string(b.payload.size());
            return false;
        }
        if (a.payload != b.payload) {
            errorOut = "Round-trip [4/4]: chunk [" + std::to_string(i) +
                       "] ('" + a.tag + "') payload bytes differ";
            return false;
        }
    }

    // ── MVhd header field comparison ─────────────────────────────────────────
    if (movie.header.valid && movie2.header.valid) {
        const auto& h1 = movie.header;
        const auto& h2 = movie2.header;
        if (h1.codecId         != h2.codecId         ||
            h1.width           != h2.width           ||
            h1.height          != h2.height          ||
            h1.frameCount      != h2.frameCount      ||
            h1.largestChunkSize != h2.largestChunkSize ||
            h1.fpsDenominator  != h2.fpsDenominator  ||
            h1.fpsNumerator    != h2.fpsNumerator)
        {
            errorOut = "Round-trip [4/4]: MVhd header fields differ between "
                       "original parse and re-parse of rebuilt bytes";
            return false;
        }
    }

    // ── Byte-for-byte equality ────────────────────────────────────────────────
    // For a well-formed source file this MUST hold:
    //   - Parser reads totalSize from the file and stores exactly (totalSize - 8)
    //     bytes into payload.
    //   - Writer emits (8 + payload.size()) as the size field = totalSize again.
    //   - So the rebuilt bytes are identical to the original.
    //
    // A mismatch means the source file had an impossible size field that the
    // writer corrected (e.g. size field was the payload length rather than the
    // total length).  The structural checks above confirmed payload-level
    // equivalence; treat byte mismatch as a hard error to surface unexpected data.
    if (rebuilt.size() != original.size() ||
        std::memcmp(rebuilt.data(), original.data(), original.size()) != 0)
    {
        errorOut = "Round-trip [4/4]: structural equivalence confirmed, but "
                   "byte-for-byte comparison failed (rebuilt=" +
                   std::to_string(rebuilt.size()) + " bytes, original=" +
                   std::to_string(original.size()) + " bytes); "
                   "the source file likely had mismatched chunk-size header fields";
        return false;
    }

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Diagnostics
// ─────────────────────────────────────────────────────────────────────────────

std::string describeEaVp6Movie(const EaVp6Movie& movie)
{
    std::ostringstream ss;
    ss << "EA VP6 Movie\n";
    ss << "- Chunks: " << movie.chunks.size() << "\n";
    ss << "- Header: " << (movie.header.valid ? "valid" : "not decoded") << "\n";

    if (movie.header.valid) {
        const auto& h = movie.header;
        ss << "- Codec ID: 0x" << std::hex << std::setw(8) << std::setfill('0')
           << h.codecId << std::dec << "\n";
        ss << "- Width: "          << h.width  << "\n";
        ss << "- Height: "         << h.height << "\n";
        ss << "- Frame Count: "    << h.frameCount << "\n";
        ss << "- Largest Chunk: "  << h.largestChunkSize << "\n";
        if (h.fps > 0.0)
            ss << "- FPS: " << h.fps << "\n";
        else
            ss << "- FPS: (unknown)\n";
    }

    if (!movie.warnings.empty()) {
        ss << "- Warnings:\n";
        for (const auto& w : movie.warnings)
            ss << "  ! " << w << "\n";
    }

    ss << "- Chunk Summary:\n";
    constexpr std::size_t kMaxDisplay = 20;
    const std::size_t     display     = std::min(movie.chunks.size(), kMaxDisplay);
    for (std::size_t i = 0; i < display; ++i) {
        const auto& c = movie.chunks[i];
        const std::size_t total = 8 + c.payload.size();
        ss << "  [" << i << "] " << c.tag
           << " total=" << total
           << " payload=" << c.payload.size() << "\n";
    }
    if (movie.chunks.size() > kMaxDisplay)
        ss << "  ... (" << (movie.chunks.size() - kMaxDisplay) << " more)\n";

    return ss.str();
}

} // namespace gf::media
