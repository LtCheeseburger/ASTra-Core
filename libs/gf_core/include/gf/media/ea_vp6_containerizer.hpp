#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// gf::media  EA VP6 containerization layer
//
// Converts VP6-in-FLV output (from FfmpegVp6EncoderBackend) into an EA VP6
// byte stream suitable for later replacement into an AST entry.
//
// Pipeline:
//   FLV file (VP6 encoded)
//     → FlvVp6Parser  (extract ordered VP6 bitstream packets)
//     → buildEaVp6MovieFromFlvVp6  (synthesize EaVp6Movie)
//     → EaVp6Writer  (serialize to bytes)
//     → validateEaVp6ContainerizationOutput  (structural checks)
//
// This module does NOT modify AST data.
//
// MVhd synthesis assumptions:
//   - codec_id: uses target.codecId when non-zero; falls back to 0x30367076
//     (ASCII 'vp60' in little-endian) which is the standard EA VP6 codec id.
//   - fps rational: uses target.fpsDenominator / target.fpsNumerator when both
//     are non-zero; otherwise approximates from target.fps using a lookup table
//     of common rates, falling back to round(fps*1000) / 1000.
//   - frameCount: number of extracted FLV VP6 packets (all MV0K + MV0F).
//   - largestChunkSize: max(8 + payload.size()) across all video-frame chunks
//     (i.e. the maximum total chunk size including the 8-byte header), which
//     matches how the EA VP6 format stores this field in observed files.
//   - Unknown original chunks (e.g. SCHl, SCCl) are NOT included in the new
//     movie; this produces a minimal-but-valid EA VP6 container.
// ─────────────────────────────────────────────────────────────────────────────

#include "gf/media/ea_vp6.hpp"
#include "gf/media/ea_vp6_import_prep.hpp"
#include "gf/media/flv_vp6.hpp"
#include "gf/media/avi_vp6.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace gf::media {

// ─────────────────────────────────────────────────────────────────────────────
// Validation result
// ─────────────────────────────────────────────────────────────────────────────

struct EaVp6ContainerizationValidation {
    bool passed                  = false;

    // Individual check results (false = check did not pass)
    bool mvhdValid               = false;  // MVhd parsed and valid
    bool frameCountMatches       = false;  // MVhd.frameCount == # video chunks
    bool largestChunkSizeMatches = false;  // MVhd.largestChunkSize == max total chunk size
    bool dimensionsMatch         = false;  // MVhd width/height == target
    bool hasKeyframe             = false;  // at least one MV0K chunk present
    bool firstFrameIsKeyframe    = false;  // first video chunk is MV0K
    bool roundTripParsePassed    = false;  // serialized bytes re-parsed successfully

    std::vector<std::string> failures;   // failed check descriptions
    std::vector<std::string> warnings;   // non-fatal observations
};

// ─────────────────────────────────────────────────────────────────────────────
// Full containerization result  (for diagnostics and testing)
// ─────────────────────────────────────────────────────────────────────────────

struct EaVp6ContainerizationResult {
    bool success = false;

    FlvVp6VideoStream              flvStream;
    EaVp6Movie                     movie;
    std::vector<std::uint8_t>      serializedBytes;

    EaVp6ContainerizationValidation validation;

    std::vector<std::string> warnings;
    std::vector<std::string> errors;
};

// ─────────────────────────────────────────────────────────────────────────────
// Core builder
// ─────────────────────────────────────────────────────────────────────────────

// Build a fresh EaVp6Movie from extracted FLV packets and original target info.
//
// Chunk list produced:
//   [0] MVhd  — synthesized from target + flv stream metadata
//   [1..N] MV0K / MV0F  — one per flv.packet in display order
//
// Returns std::nullopt when:
//   - flv.packets is empty
//   - target dimensions are zero (width or height == 0 when headerValid)
std::optional<EaVp6Movie>
buildEaVp6MovieFromFlvVp6(const FlvVp6VideoStream& flv,
                           const EaVp6ImportTarget& target,
                           std::string& errorOut);

// ─────────────────────────────────────────────────────────────────────────────
// Validation helper
// ─────────────────────────────────────────────────────────────────────────────

// Validate the synthesized EaVp6Movie and its serialized bytes.
//
// Checks (all hard failures unless noted):
//   1. serializedBytes can be re-parsed by EaVp6Parser           [hard]
//   2. Re-parsed MVhd is valid                                    [hard]
//   3. MVhd.frameCount == count of MV0K+MV0F chunks in movie     [hard]
//   4. MVhd.largestChunkSize == max(8+payload) of video chunks    [hard]
//   5. MVhd width/height match target (if target.headerValid)     [hard]
//   6. At least one MV0K chunk exists                             [hard]
//   7. First video chunk is MV0K                                  [warning]
EaVp6ContainerizationValidation
validateEaVp6ContainerizationOutput(const EaVp6Movie&                movie,
                                     const EaVp6ImportTarget&         target,
                                     const std::vector<std::uint8_t>& serializedBytes);

// ─────────────────────────────────────────────────────────────────────────────
// High-level orchestration
// ─────────────────────────────────────────────────────────────────────────────

// Parse FLV → build EaVp6Movie → serialize → validate → return bytes.
// Returns std::nullopt on any step failure; errorOut set to first failure reason.
std::optional<std::vector<std::uint8_t>>
containerizeVp6FlvToEa(const std::filesystem::path& flvPath,
                        const EaVp6ImportTarget&     target,
                        std::string&                 errorOut);

// Full-result version: returns all intermediate data for diagnostics/testing.
// Does not throw.  errors/warnings describe every problem found.
EaVp6ContainerizationResult
containerizeVp6FlvToEaWithResult(const std::filesystem::path& flvPath,
                                  const EaVp6ImportTarget&     target);

// ─────────────────────────────────────────────────────────────────────────────
// AVI VP6 variants
//
// Same pipeline as the FLV variants above, but the input is a VirtualDub AVI
// file containing VP6-in-AVI (VP62 FourCC).  AviVp6Parser extracts the VP6
// bitstream packets (no byte-stripping needed — AVI '00dc' chunks are raw VP6).
// ─────────────────────────────────────────────────────────────────────────────

// Parse AVI → build EaVp6Movie → serialize → validate → return bytes.
// Returns std::nullopt on any step failure; errorOut set to first failure reason.
std::optional<std::vector<std::uint8_t>>
containerizeVp6AviToEa(const std::filesystem::path& aviPath,
                        const EaVp6ImportTarget&     target,
                        std::string&                 errorOut);

// Full-result version.  The result's flvStream field holds the AVI-sourced
// packets (AviVp6VideoStream is an alias for FlvVp6VideoStream).
EaVp6ContainerizationResult
containerizeVp6AviToEaWithResult(const std::filesystem::path& aviPath,
                                  const EaVp6ImportTarget&     target);

// ─────────────────────────────────────────────────────────────────────────────
// Diagnostics
// ─────────────────────────────────────────────────────────────────────────────

std::string describeEaVp6ContainerizationResult(
    const EaVp6ContainerizationResult& result);

} // namespace gf::media
