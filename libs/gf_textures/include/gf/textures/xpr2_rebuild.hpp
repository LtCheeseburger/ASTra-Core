#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace gf::textures {

// Rebuild a standard DDS blob ("DDS "+header+data) from an Xbox 360 XPR2 container.
//
// Notes:
// - XPR2 commonly stores block-compressed data (DXT1/DXT3/DXT5/ATI2) in Xbox 360 tiled layout.
// - This function extracts the *first* texture entry in the container.
// - If outName is provided, it will be filled with the texture name when present.
// When all_mips is true, decodes the full mip chain and writes a multi-level DDS.
// When false (default), decodes only mip 0 (suitable for preview).
std::optional<std::vector<std::uint8_t>> rebuild_xpr2_dds_first(std::span<const std::uint8_t> payload,
                                                               std::string* outName = nullptr,
                                                               bool all_mips = false);

// Lightweight XPR2 metadata — extracted without decoding or building a DDS.
// Useful for display and validation before conversion.
struct Xpr2Info {
  std::uint32_t width          = 0;   // logical texture width in texels
  std::uint32_t height         = 0;   // logical texture height in texels
  std::uint8_t  fmt_code       = 0;   // raw EA format byte (0x52=DXT1, 0x53=DXT3, 0x54=DXT5, 0x71=ATI2, 0x7C=DXT1 normal)
  std::uint32_t mip_count      = 1;   // inferred mip count from the stored XPR2 payload layout
  std::uint32_t texture_count  = 0;   // number of resource entries in the XPR2
  std::string   first_name;           // name of the first TX2D entry (may be empty)
};

// Parse XPR2 metadata without decoding the texture data.
// Returns nullopt if the input is not a valid / supported XPR2.
std::optional<Xpr2Info> parse_xpr2_info(std::span<const std::uint8_t> payload);

// Convert a DDS file back into XPR2 format by patching the texture payload of an
// existing XPR2 container.
//
// The new_dds must be a valid DDS with the same dimensions, mip contract, and
// compatible block-compressed format family as the original XPR2 texture. The function:
//   1. Parses the original XPR2 to obtain its structure and format metadata.
//   2. Validates dimension parity, format parity, and mip-count parity.
//   3. Re-tiles and re-encodes every mip level to match the original XPR2 layout.
//   4. Returns a complete XPR2 byte vector with only the texture data replaced.
//
// Returns nullopt + fills *err on any failure (dimension mismatch, mip mismatch,
// malformed DDS, unsupported format/layout, parse error, etc.).
std::optional<std::vector<std::uint8_t>> dds_to_xpr2_patch(
    std::span<const std::uint8_t> original_xpr2,
    std::span<const std::uint8_t> new_dds,
    std::string* err = nullptr);

}
