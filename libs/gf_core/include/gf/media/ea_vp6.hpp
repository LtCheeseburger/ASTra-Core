#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// gf::media  EA VP6 container parser / writer foundation
//
// Parses EA VP6 video container files as a sequence of tagged chunks.
// Rebuilds them byte-for-byte from the in-memory model.
// Does NOT parse VP6 bitstream internals, audio streams, or frame data.
//
// Format assumptions (documented from real sample files):
//   - A file is a flat sequence of chunks.
//   - Each chunk: 4-byte ASCII tag  +  le_u32 TOTAL chunk size  +  payload bytes.
//     IMPORTANT: the size field is the TOTAL chunk size including the 8-byte
//     header itself.  payload size = totalSize - 8.
//     Example: "MVhd" with size field = 0x20 (32) → 24 bytes of payload.
//   - The first chunk is always "MVhd" (movie header).
//   - MVhd payload (24 bytes, payload-relative offsets):
//       +0x00  le_32  codec id            (usually "vp60" = 0x30367076)
//       +0x04  le_16  width
//       +0x06  le_16  height
//       +0x08  le_32  frame count
//       +0x0C  le_32  largest frame chunk size
//       +0x10  le_32  fps denominator     (rate)
//       +0x14  le_32  fps numerator       (scale)
//   - "MV0K" = keyframe chunk
//   - "MV0F" = delta/inter frame chunk
//   - Unknown tags (e.g. "SCHl", "SCCl") are preserved verbatim.
// ─────────────────────────────────────────────────────────────────────────────

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace gf::media {

// ── Chunk kind ────────────────────────────────────────────────────────────────

enum class EaVp6ChunkKind {
    Unknown,  // unrecognised tag — payload preserved verbatim
    MVhd,     // movie header
    MV0K,     // keyframe video chunk
    MV0F,     // delta / inter frame video chunk
};

// ── Chunk ─────────────────────────────────────────────────────────────────────

struct EaVp6Chunk {
    EaVp6ChunkKind            kind          = EaVp6ChunkKind::Unknown;
    std::string               tag;           // literal 4-char ASCII tag
    std::uint32_t             declaredSize  = 0;  // TOTAL chunk size (header+payload) as read from file
    std::vector<std::uint8_t> payload;            // exact payload bytes (declaredSize - 8)
    std::uint64_t             fileOffset    = 0;  // byte offset of chunk header (diagnostics only)
};

// ── Header ────────────────────────────────────────────────────────────────────
//
// Decoded from the first MVhd payload when it is >= 24 bytes.
// The MVhd chunk itself is still kept in EaVp6Movie::chunks with its payload
// preserved intact — this struct is a convenience view, not the source of truth.

struct EaVp6Header {
    bool          valid           = false;
    std::uint32_t codecId         = 0;
    std::uint16_t width           = 0;
    std::uint16_t height          = 0;
    std::uint32_t frameCount      = 0;
    std::uint32_t largestChunkSize = 0;
    std::uint32_t fpsDenominator  = 0;   // "rate"  in MultimediaWiki
    std::uint32_t fpsNumerator    = 0;   // "scale" in MultimediaWiki
    double        fps             = 0.0; // fpsDenominator / fpsNumerator when both non-zero
};

// ── Movie ─────────────────────────────────────────────────────────────────────

struct EaVp6Movie {
    EaVp6Header              header;
    std::vector<EaVp6Chunk>  chunks;    // all chunks in file order, including MVhd
    std::vector<std::string> warnings;  // non-fatal parse notes
};

// ── Parser ────────────────────────────────────────────────────────────────────

class EaVp6Parser {
public:
    // Parse an EA VP6 container from raw bytes.
    //
    // Returns the populated EaVp6Movie on success.
    // Returns std::nullopt on failure; errorOut receives a human-readable reason.
    //
    // Tolerances:
    //   - Unknown chunk tags are preserved as EaVp6ChunkKind::Unknown.
    //   - MVhd with payload < 24 bytes adds a warning instead of failing.
    //   - Trailing bytes that cannot form an 8-byte chunk header are a hard error.
    std::optional<EaVp6Movie> parse(std::span<const std::uint8_t> data,
                                    std::string& errorOut) const;
};

// ── Writer ────────────────────────────────────────────────────────────────────

class EaVp6Writer {
public:
    // Rebuild EA VP6 bytes from an EaVp6Movie.
    //
    // For every chunk in chunks (in order):
    //   writes 4-byte tag
    //   writes le_u32  (8 + payload.size())  ← total chunk size; declaredSize NOT used
    //   writes payload bytes verbatim
    //
    // Writing total chunk size (not payload size) matches the real EA VP6 format
    // and produces byte-for-byte identical output for well-formed source files.
    //
    // Fails if any chunk tag is not exactly 4 characters, or if
    // (8 + payload.size()) overflows u32.
    std::optional<std::vector<std::uint8_t>> build(const EaVp6Movie& movie,
                                                    std::string& errorOut) const;
};

// ── Round-trip validation ─────────────────────────────────────────────────────
//
// Executes the full parse → build → re-parse pipeline and checks:
//   1. chunk count is identical
//   2. chunk tags are in the same order
//   3. each chunk's payload size is unchanged
//   4. each chunk's payload bytes are identical
//   5. MVhd header fields match (when valid in both parses)
//   6. rebuilt bytes are byte-for-byte identical to the original
//      (holds for all well-formed inputs; discrepancy means the source file
//       had mismatched chunk-size headers, which the writer normalises)
//
// Returns true on full success.
// Returns false on any discrepancy; errorOut describes the first failure.
bool validateRoundTrip(std::span<const std::uint8_t> original, std::string& errorOut);

// ── Diagnostics ───────────────────────────────────────────────────────────────

EaVp6ChunkKind chunkKindFromTag(const std::string& tag) noexcept;
std::string    tagFromChunkKind(EaVp6ChunkKind kind) noexcept;

// Returns a multi-line human-readable summary of the movie.
// Lists the first 20 chunks; remaining are summarised as "... (N more)".
std::string describeEaVp6Movie(const EaVp6Movie& movie);

} // namespace gf::media
