#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// gf::media  AVI VP6 packet extraction
//
// Parses an AVI (Audio Video Interleave) file produced by VirtualDub with the
// On2 VP6 (VP62) VFW codec and extracts the ordered sequence of VP6 bitstream
// packets.
//
// Scope:
//   - Handles AVI 1.0 (RIFF AVI, with idx1 index).
//   - Extracts video stream 0 chunks ('00dc' / '00db') from the movi LIST.
//   - Keyframe detection from idx1 AVIIF_KEYFRAME flag (0x10).
//   - Ignores audio stream chunks.
//   - Does NOT parse BITMAPINFOHEADER or AMF data beyond the VP6 codec check.
//
// AVI VP6 vs FLV VP6 payload format:
//   FLV video tags prepend a (frame_type|codec_id) byte + VP6 adjustment byte
//   before the raw bitstream — these must be stripped (see flv_vp6.hpp).
//   AVI '00dc' chunk data is the raw VP6 bitstream directly, with no prefix.
//   Therefore no byte stripping is needed for AVI-sourced payloads.
//
// The parser returns FlvVp6VideoStream (via the AviVp6VideoStream alias) so
// that the EA VP6 containerizer can accept both FLV and AVI input without
// additional overloads.
// ─────────────────────────────────────────────────────────────────────────────

#include "gf/media/flv_vp6.hpp"   // FlvVp6VideoStream (reused as packet container)

#include <filesystem>
#include <optional>
#include <string>

namespace gf::media {

// AVI VP6 video stream — same packet structure as the FLV parser.
// The alias makes the source format explicit at call sites while
// allowing the containerizer to accept either type directly.
using AviVp6VideoStream = FlvVp6VideoStream;

class AviVp6Parser {
public:
    // Open inputPath as an AVI file, walk the RIFF structure, and extract VP6
    // video packets in display order.
    //
    // Returns a populated AviVp6VideoStream on success (valid = true).
    // Returns std::nullopt when:
    //   - the file cannot be opened
    //   - the RIFF/AVI signature is invalid
    //   - the movi LIST is absent
    //   - no video frames are found
    //
    // Non-fatal issues (missing idx1, truncated frames, etc.) are added to
    // AviVp6VideoStream::warnings instead of causing failure.
    std::optional<AviVp6VideoStream>
    parse(const std::filesystem::path& inputPath, std::string& errorOut) const;
};

} // namespace gf::media
