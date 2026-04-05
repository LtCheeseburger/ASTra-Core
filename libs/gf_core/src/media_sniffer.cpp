#include "gf/media/media_sniffer.hpp"

#include <cstring>

namespace gf::media {

// ── Internal helpers ──────────────────────────────────────────────────────────

static bool hasBytes(const std::uint8_t* data, std::size_t size,
                     std::size_t offset, const char* needle, std::size_t len) noexcept
{
    if (offset + len > size) return false;
    return std::memcmp(data + offset, needle, len) == 0;
}

// ── detectMedia ───────────────────────────────────────────────────────────────

MediaKind detectMedia(const std::uint8_t* data, std::size_t size) noexcept
{
    if (!data || size < 8) return MediaKind::Unknown;

    // EA VP6 container: "MVhd" at offset 0, "vp60" at offset 8
    if (size >= 12 &&
        hasBytes(data, size, 0, "MVhd", 4) &&
        hasBytes(data, size, 8, "vp60", 4))
    {
        return MediaKind::EaVp6Container;
    }

    // MP4 / ISO Base Media: "ftyp" at offset 4
    if (size >= 8 && hasBytes(data, size, 4, "ftyp", 4))
        return MediaKind::Mp4;

    return MediaKind::Unknown;
}

// ── probeMedia ────────────────────────────────────────────────────────────────

MediaProbeResult probeMedia(const std::uint8_t* data, std::size_t size) noexcept
{
    MediaProbeResult result;
    result.kind = detectMedia(data, size);

    switch (result.kind) {
    case MediaKind::EaVp6Container: {
        result.containerName         = "EA Media (VP6)";
        result.videoCodec            = "VP6";
        result.audioCodec            = "";          // varies; not parseable from header alone
        result.hasVideo              = true;
        result.hasAudio              = false;       // conservative default
        result.rawExtractSupported   = true;
        result.transcodeMp4Supported = true;

        // ── MVhd field parsing ────────────────────────────────────────────────
        // File layout:
        //   0x00..0x03  "MVhd"         chunk FourCC (= codec id)
        //   0x04..0x07  le_32          chunk size
        //   0x08        payload start
        //
        // Payload fields (offsets relative to payload start = file offset 0x08):
        //   +0x00  le_32  codec id   ("vp60")
        //   +0x04  le_16  width
        //   +0x06  le_16  height
        //   +0x08  le_32  frame count
        //   +0x0C  le_32  largest frame chunk size
        //   +0x10  le_32  frame rate denominator  (rate)
        //   +0x14  le_32  frame rate numerator    (scale)
        //
        // Total minimum size to read all fields: 8 (header) + 0x18 (payload) = 0x20 = 32 bytes.
        constexpr std::size_t kPayloadBase = 0x08;
        if (size >= 0x20) {
            auto readU16le = [&](std::size_t off) noexcept -> std::uint16_t {
                return static_cast<std::uint16_t>(
                    static_cast<std::uint16_t>(data[off]) |
                    (static_cast<std::uint16_t>(data[off + 1]) << 8));
            };
            auto readU32le = [&](std::size_t off) noexcept -> std::uint32_t {
                return static_cast<std::uint32_t>(data[off]) |
                       (static_cast<std::uint32_t>(data[off + 1]) <<  8) |
                       (static_cast<std::uint32_t>(data[off + 2]) << 16) |
                       (static_cast<std::uint32_t>(data[off + 3]) << 24);
            };

            result.width  = readU16le(kPayloadBase + 0x04);
            result.height = readU16le(kPayloadBase + 0x06);

            const std::uint32_t fpsDenom = readU32le(kPayloadBase + 0x10);
            const std::uint32_t fpsNum   = readU32le(kPayloadBase + 0x14);
            if (fpsNum != 0) {
                result.fps = static_cast<double>(fpsDenom) / static_cast<double>(fpsNum);
            } else if (fpsDenom != 0) {
                result.warnings.push_back("MVhd fps scale (numerator) is zero; fps unknown");
            }
        } else {
            result.warnings.push_back("EA VP6 header too short to read MVhd metadata fields");
        }
        break;
    }

    case MediaKind::Mp4:
        result.containerName         = "MP4";
        result.videoCodec            = "H.264 (assumed)";
        result.audioCodec            = "AAC (assumed)";
        result.hasVideo              = true;
        result.hasAudio              = true;
        result.rawExtractSupported   = true;
        result.transcodeMp4Supported = false;       // already MP4; no transcode needed
        break;

    case MediaKind::Unknown:
    default:
        result.containerName         = "Unknown";
        result.rawExtractSupported   = true;        // always allow raw dump
        result.transcodeMp4Supported = false;
        break;
    }

    return result;
}

// ── suggestedExtension ────────────────────────────────────────────────────────

std::string suggestedExtension(MediaKind kind) noexcept
{
    switch (kind) {
    case MediaKind::EaVp6Container: return ".vp6ea";
    case MediaKind::Mp4:            return ".mp4";
    case MediaKind::Unknown:
    default:                        return ".bin";
    }
}

} // namespace gf::media
