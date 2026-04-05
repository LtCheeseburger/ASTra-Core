#include "gf/media/flv_vp6.hpp"

#include <algorithm>
#include <climits>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace gf::media {

// ─────────────────────────────────────────────────────────────────────────────
// Binary read helpers (big-endian — FLV is big-endian throughout)
// ─────────────────────────────────────────────────────────────────────────────

static std::uint8_t readU8(std::istream& in, bool& ok) noexcept
{
    std::uint8_t v = 0;
    ok = ok && static_cast<bool>(
        in.read(reinterpret_cast<char*>(&v), 1));
    return v;
}

// Read a 3-byte big-endian unsigned integer.
static std::uint32_t readU24BE(std::istream& in, bool& ok) noexcept
{
    std::uint8_t b[3] = {0, 0, 0};
    ok = ok && static_cast<bool>(in.read(reinterpret_cast<char*>(b), 3));
    return (std::uint32_t(b[0]) << 16) |
           (std::uint32_t(b[1]) <<  8) |
            std::uint32_t(b[2]);
}

// Read a 4-byte big-endian unsigned integer.
static std::uint32_t readU32BE(std::istream& in, bool& ok) noexcept
{
    std::uint8_t b[4] = {0, 0, 0, 0};
    ok = ok && static_cast<bool>(in.read(reinterpret_cast<char*>(b), 4));
    return (std::uint32_t(b[0]) << 24) |
           (std::uint32_t(b[1]) << 16) |
           (std::uint32_t(b[2]) <<  8) |
            std::uint32_t(b[3]);
}

// Skip exactly n bytes.  Sets ok=false if seek fails.
static void skipBytes(std::istream& in, std::uint32_t n, bool& ok) noexcept
{
    if (!ok || n == 0) return;
    in.seekg(n, std::ios::cur);
    ok = ok && in.good();
}

// ─────────────────────────────────────────────────────────────────────────────
// FlvVp6Parser::parse
// ─────────────────────────────────────────────────────────────────────────────

std::optional<FlvVp6VideoStream>
FlvVp6Parser::parse(const std::filesystem::path& inputPath,
                    std::string& errorOut) const
{
    std::ifstream in(inputPath, std::ios::binary);
    if (!in.is_open()) {
        errorOut = "FlvVp6Parser: cannot open file: " + inputPath.string();
        return std::nullopt;
    }

    // ── FLV file header (9 bytes minimum) ────────────────────────────────────
    {
        char sig[3] = {};
        if (!in.read(sig, 3) || sig[0] != 'F' || sig[1] != 'L' || sig[2] != 'V') {
            errorOut = "FlvVp6Parser: not an FLV file (invalid signature in " +
                       inputPath.string() + ")";
            return std::nullopt;
        }
    }

    bool ok = true;
    /* uint8_t version = */ readU8(in, ok);   // typically 1
    /* uint8_t flags   = */ readU8(in, ok);   // bit0=audio, bit2=video
    const std::uint32_t dataOffset = readU32BE(in, ok);

    if (!ok) {
        errorOut = "FlvVp6Parser: truncated FLV header";
        return std::nullopt;
    }

    // Seek to the first tag (dataOffset bytes from file start).
    // The header is 9 bytes; if dataOffset > 9 there are extra header bytes.
    if (dataOffset > 9u) {
        in.seekg(static_cast<std::streamoff>(dataOffset), std::ios::beg);
        if (!in.good()) {
            errorOut = "FlvVp6Parser: cannot seek to dataOffset " +
                       std::to_string(dataOffset);
            return std::nullopt;
        }
    }

    // ── PreviousTagSize0 (always 0) ───────────────────────────────────────────
    ok = true;
    /* uint32_t pts0 = */ readU32BE(in, ok);
    if (!ok) {
        errorOut = "FlvVp6Parser: truncated FLV (missing PreviousTagSize0)";
        return std::nullopt;
    }

    // ── Tag loop ──────────────────────────────────────────────────────────────
    FlvVp6VideoStream stream;
    bool foundAnyVideoTag = false;

    while (true) {
        // Attempt to read the one-byte tag type.  EOF here is normal.
        std::uint8_t tagType = 0;
        {
            const std::streampos posBefore = in.tellg();
            (void)posBefore;
            ok = true;
            tagType = readU8(in, ok);
            if (!ok) break;   // EOF or read error — end of file
        }

        // Read remainder of the 11-byte tag header.
        ok = true;
        const std::uint32_t dataSize    = readU24BE(in, ok);
        const std::uint32_t timestamp24 = readU24BE(in, ok);
        const std::uint8_t  tsExt       = readU8(in, ok);
        /* uint32_t streamId  = */ readU24BE(in, ok);   // always 0, discard

        if (!ok) break;   // truncated tag header — treat as end of file

        const std::uint32_t timestampMs =
            (std::uint32_t(tsExt) << 24) | timestamp24;

        // ── Video tag ─────────────────────────────────────────────────────────
        if (tagType == 9 /* video */) {
            foundAnyVideoTag = true;

            if (dataSize < 2u) {
                // A video tag with < 2 bytes of data is malformed; skip it.
                stream.warnings.push_back(
                    "FlvVp6Parser: video tag at ts=" + std::to_string(timestampMs) +
                    "ms has dataSize=" + std::to_string(dataSize) +
                    " (< 2 bytes); skipped");
                ok = true;
                skipBytes(in, dataSize, ok);
                if (!ok) break;
            } else {
                // First byte: (frame_type << 4) | codec_id.
                ok = true;
                const std::uint8_t codecByte   = readU8(in, ok);
                const std::uint8_t adjustByte  = readU8(in, ok);   // VP6 adj, discard
                (void)adjustByte;

                if (!ok) break;

                const std::uint8_t frameType = (codecByte >> 4) & 0x0Fu;
                const std::uint8_t codecId   =  codecByte       & 0x0Fu;

                const std::uint32_t remaining = dataSize - 2u;

                if (codecId == 4u /* On2 VP6 / vp6f */) {
                    if (frameType == 5u) {
                        // Video info/command frame — not a real picture; skip.
                        ok = true;
                        skipBytes(in, remaining, ok);
                        if (!ok) break;
                    } else {
                        // Extract VP6 bitstream payload.
                        FlvVideoPacket pkt;
                        pkt.timestampMs = timestampMs;
                        pkt.isKeyframe  = (frameType == 1u);
                        pkt.payload.resize(remaining);

                        if (!in.read(reinterpret_cast<char*>(pkt.payload.data()),
                                     static_cast<std::streamsize>(remaining)))
                        {
                            // Truncated payload — skip this packet.
                            stream.warnings.push_back(
                                "FlvVp6Parser: truncated VP6 payload at ts=" +
                                std::to_string(timestampMs) + "ms; packet skipped");
                            break;
                        }
                        stream.packets.push_back(std::move(pkt));
                    }
                } else if (codecId == 5u /* VP6 with alpha */) {
                    stream.warnings.push_back(
                        "FlvVp6Parser: VP6-alpha (codec_id=5) at ts=" +
                        std::to_string(timestampMs) +
                        "ms is not supported; skipping frame");
                    ok = true;
                    skipBytes(in, remaining, ok);
                    if (!ok) break;
                } else {
                    // Non-VP6 codec in this video tag — skip.
                    ok = true;
                    skipBytes(in, remaining, ok);
                    if (!ok) break;
                }
            }
        } else {
            // Audio (8), ScriptData (18), or unknown — skip data entirely.
            ok = true;
            skipBytes(in, dataSize, ok);
            if (!ok) break;
        }

        // PreviousTagSize after the tag (11 + dataSize bytes).
        // We don't validate it — some encoders emit incorrect values.
        ok = true;
        /* uint32_t pts = */ readU32BE(in, ok);
        if (!ok) break;
    }

    // ── Post-loop validation ──────────────────────────────────────────────────
    if (!foundAnyVideoTag) {
        errorOut = "FlvVp6Parser: no video tags found in FLV";
        return std::nullopt;
    }
    if (stream.packets.empty()) {
        errorOut = "FlvVp6Parser: no VP6 video packets extracted from FLV "
                   "(file may contain a non-VP6 video codec, or all packets were skipped)";
        return std::nullopt;
    }

    stream.frameCount = static_cast<std::uint32_t>(stream.packets.size());

    // Estimate FPS from packet timestamps.
    // Uses the interval between the first and last packet so the estimate is
    // stable even if the first packet is at t=0.
    if (stream.packets.size() >= 2u) {
        const double durationMs =
            static_cast<double>(stream.packets.back().timestampMs) -
            static_cast<double>(stream.packets.front().timestampMs);
        if (durationMs > 0.0) {
            stream.fps = (static_cast<double>(stream.frameCount) - 1.0)
                         * 1000.0 / durationMs;
        }
    }

    // Sanity checks that produce warnings but do not fail.
    if (!stream.packets.front().isKeyframe) {
        stream.warnings.push_back(
            "FlvVp6Parser: first VP6 packet is not a keyframe; "
            "EA VP6 files are expected to start with an MV0K (keyframe) chunk");
    }

    const bool hasAnyKeyframe = std::any_of(
        stream.packets.begin(), stream.packets.end(),
        [](const FlvVideoPacket& p){ return p.isKeyframe; });
    if (!hasAnyKeyframe) {
        stream.warnings.push_back(
            "FlvVp6Parser: no keyframe found among " +
            std::to_string(stream.frameCount) + " extracted packets");
    }

    stream.valid = true;
    return stream;
}

// ─────────────────────────────────────────────────────────────────────────────
// describeFlvVp6VideoStream
// ─────────────────────────────────────────────────────────────────────────────

std::string describeFlvVp6VideoStream(const FlvVp6VideoStream& stream)
{
    std::ostringstream ss;
    ss << "FLV VP6 Video Stream\n";
    ss << "- Valid: " << (stream.valid ? "yes" : "no") << "\n";

    if (!stream.valid && stream.packets.empty()) {
        if (!stream.warnings.empty()) {
            ss << "- Warnings:\n";
            for (const auto& w : stream.warnings)
                ss << "  ! " << w << "\n";
        }
        return ss.str();
    }

    ss << "- Packets: " << stream.frameCount << "\n";

    std::size_t kfCount = 0;
    for (const auto& p : stream.packets)
        if (p.isKeyframe) ++kfCount;
    ss << "- Keyframes: "    << kfCount                         << "\n";
    ss << "- Delta frames: " << (stream.frameCount - kfCount)   << "\n";

    if (stream.fps > 0.0)
        ss << "- FPS (from timestamps): "
           << std::fixed << std::setprecision(2) << stream.fps  << "\n";

    // Payload size stats
    if (!stream.packets.empty()) {
        std::size_t maxPayload = 0, minPayload = SIZE_MAX, totalPayload = 0;
        for (const auto& p : stream.packets) {
            maxPayload    = std::max(maxPayload,   p.payload.size());
            minPayload    = std::min(minPayload,   p.payload.size());
            totalPayload += p.payload.size();
        }
        ss << "- Largest packet payload: " << maxPayload    << " bytes\n";
        ss << "- Smallest packet payload: " << minPayload   << " bytes\n";
        ss << "- Total payload bytes: "    << totalPayload  << "\n";
    }

    if (!stream.warnings.empty()) {
        ss << "- Warnings:\n";
        for (const auto& w : stream.warnings)
            ss << "  ! " << w << "\n";
    }

    return ss.str();
}

} // namespace gf::media
