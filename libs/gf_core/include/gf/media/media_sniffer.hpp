#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// gf::media::media_sniffer
//
// Lightweight, zero-dependency media file detection and probing.
//
// Detection is purely signature-based (magic bytes).  No library parsing is
// performed.  All functions are noexcept and safe to call with arbitrary data.
// ─────────────────────────────────────────────────────────────────────────────

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace gf::media {

// ── Media kind ───────────────────────────────────────────────────────────────

enum class MediaKind {
    Unknown,          // unrecognised / not a media file
    EaVp6Container,   // EA proprietary VP6 container  (MVhd … vp60 magic)
    Mp4,              // Standard MPEG-4 container      (ftyp magic at offset 4)
};

// ── Probe result ─────────────────────────────────────────────────────────────
//
// Populated conservatively from header bytes only.  Fields that cannot be
// determined without full parsing are left at their zero-value defaults.

struct MediaProbeResult {
    MediaKind   kind                  = MediaKind::Unknown;
    std::string containerName;        // e.g. "MP4", "EA Media (VP6)"
    std::string videoCodec;           // e.g. "VP6", "H.264 (assumed)"
    std::string audioCodec;           // e.g. "AAC (assumed)" — empty if unknown
    std::uint32_t width               = 0;   // 0 = not determined
    std::uint32_t height              = 0;   // 0 = not determined
    double      fps                   = 0.0; // 0 = not determined
    bool        hasVideo              = false;
    bool        hasAudio              = false;
    bool        rawExtractSupported   = true;  // always true for known formats
    bool        transcodeMp4Supported = false; // true only for EA VP6 via FFmpeg
    std::vector<std::string> warnings;
};

// ── Detection API ─────────────────────────────────────────────────────────────

// Detect the media kind from raw bytes.
// `data` may point to any prefix of the file; passing the first 16 bytes is
// sufficient.  Returns MediaKind::Unknown when size is too small or no
// signature matches.
MediaKind detectMedia(const std::uint8_t* data, std::size_t size) noexcept;

// Probe the media and return a populated MediaProbeResult.
// Fields that require full stream parsing are left at defaults.
// Never throws.
MediaProbeResult probeMedia(const std::uint8_t* data, std::size_t size) noexcept;

// ── Extension helper ─────────────────────────────────────────────────────────

// Returns the suggested file extension (including leading dot) for a kind.
// Use this when naming extracted files — never mutate the original data.
//
//   EaVp6Container  → ".vp6ea"
//   Mp4             → ".mp4"
//   Unknown         → ".bin"
std::string suggestedExtension(MediaKind kind) noexcept;

} // namespace gf::media
