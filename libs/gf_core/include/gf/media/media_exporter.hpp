#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// gf::media::media_exporter
//
// Raw extraction and MP4 export for detected media payloads.
//
// Both functions operate on an in-memory byte buffer obtained from an AST
// entry.  They never modify AST parsing state or any live game file.
// ─────────────────────────────────────────────────────────────────────────────

#include "media_sniffer.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace gf::media {

// ── Raw extraction ────────────────────────────────────────────────────────────

// Write `data` byte-for-byte to `outPath`.
// The caller is responsible for choosing the correct path/extension
// (see suggestedExtension()).
//
// Returns false if the file could not be opened or written.
// Does not modify `data` in any way.
bool extractRaw(const std::vector<std::uint8_t>& data,
                const std::filesystem::path&     outPath);

// ── MP4 export ────────────────────────────────────────────────────────────────

// Export `data` as an MP4 file at `outPath`.
//
// Behaviour by media kind:
//
//   Mp4            — write data directly (already a valid MP4; no transcoding)
//   EaVp6Container — write a temp file, invoke FFmpeg to transcode to MP4,
//                    then delete the temp file
//   Unknown        — return false with a descriptive message in errorOut
//
// `probe` must have been obtained from probeMedia() on the same `data`.
// `outPath` should have a ".mp4" extension; the function does not rename it.
//
// Returns false on any failure; errorOut contains a human-readable description.
bool exportAsMp4(const std::vector<std::uint8_t>& data,
                 const MediaProbeResult&           probe,
                 const std::filesystem::path&      outPath,
                 std::string&                      errorOut);

} // namespace gf::media
