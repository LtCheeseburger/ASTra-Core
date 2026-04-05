#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// gf::media  FLV VP6 packet extraction
//
// Parses an FLV (Flash Video) file produced by FFmpeg's vp6f encoder and
// extracts the ordered sequence of VP6 bitstream packets.
//
// Scope:
//   - Handles codec_id = 4 (On2 VP6 / vp6f) only.
//   - Strips the FLV video-tag header byte (frame_type|codec_id) and the
//     VP6 adjustment byte that follows it; the remaining data is the raw VP6
//     bitstream suitable for EA VP6 MV0K/MV0F chunk payloads.
//   - Skips audio, script-data, and video-info/command (frame_type=5) tags.
//   - Does NOT parse VP6 bitstream internals or AMF0 script data.
//   - Does NOT provide width/height (not extractable without AMF0 parsing);
//     callers must supply target dimensions from EaVp6ImportTarget.
//
// Assumption on VP6 payload stripping:
//   The EA VP6 demuxer in FFmpeg reads MV0K/MV0F chunk payloads directly as
//   raw VP6 bitstream, without an FLV adjustment byte prefix.  FFmpeg's own
//   FLV demuxer skips two bytes (frame_type byte + VP6 adjustment byte) before
//   passing data to the VP6 decoder.  Therefore, stripping two bytes from the
//   FLV video tag data yields the correct payload for EA VP6 chunks.
// ─────────────────────────────────────────────────────────────────────────────

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace gf::media {

// ── FLV tag type (informational) ──────────────────────────────────────────────

enum class FlvTagType : std::uint8_t {
    Unknown    = 0,
    Audio      = 8,
    Video      = 9,
    ScriptData = 18,
};

// ── Extracted VP6 video packet ────────────────────────────────────────────────

struct FlvVideoPacket {
    std::uint32_t            timestampMs = 0;    // display time in milliseconds
    bool                     isKeyframe  = false; // true when FLV frame_type == 1
    std::vector<std::uint8_t> payload;            // raw VP6 bitstream (2-byte prefix stripped)
};

// ── Extracted stream ──────────────────────────────────────────────────────────

struct FlvVp6VideoStream {
    bool          valid      = false;
    double        fps        = 0.0;   // estimated from packet timestamps; 0 if < 2 packets
    std::uint32_t frameCount = 0;     // = packets.size()

    std::vector<FlvVideoPacket>  packets;   // all video frames in display order
    std::vector<std::string>     warnings;
};

// ── Parser ────────────────────────────────────────────────────────────────────

class FlvVp6Parser {
public:
    // Open inputPath as an FLV file, walk all tags, and extract VP6 video
    // packets.
    //
    // Returns a populated FlvVp6VideoStream on success (valid = true).
    // Returns std::nullopt when:
    //   - the file cannot be opened
    //   - the FLV signature is invalid
    //   - no VP6 video packets are found
    //
    // Non-fatal issues (truncated packets, unsupported codec variants, etc.)
    // are added to FlvVp6VideoStream::warnings instead of causing failure.
    std::optional<FlvVp6VideoStream>
    parse(const std::filesystem::path& inputPath, std::string& errorOut) const;
};

// Multi-line human-readable summary of an extracted VP6 video stream.
std::string describeFlvVp6VideoStream(const FlvVp6VideoStream& stream);

} // namespace gf::media
