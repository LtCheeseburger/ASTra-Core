#pragma once

#include <cstdint>
#include <vector>

namespace gf::textures {

// Untile an Xbox 360 XPR2 TX2D surface using an XGAddress-style 2D tiled layout.
//
// Notes:
// - For block-compressed textures, treat each 4x4 block as a "texel".
// - width_blocks/height_blocks are the dimensions in blocks.
// - bytes_per_block is typically 8 (DXT1) or 16 (DXT3/DXT5).
// - Input must contain at least ceil32(width_blocks) * height_blocks * bytes_per_block bytes
//   (where ceil32 = round up to nearest multiple of 32).
//
// Input and output buffers are block-linear (no headers), just the compressed payload.
std::vector<std::uint8_t> xbox360_xgaddress_untile_dxt(
    const std::vector<std::uint8_t>& tiled,
    std::uint32_t width_blocks,
    std::uint32_t height_blocks,
    std::uint32_t bytes_per_block);

// Inverse of xbox360_xgaddress_untile_dxt: re-tiles a linear DXT surface back into
// the XGAddress tiled layout used by Xbox 360.
//
// Used for DDS → XPR2 import to produce the on-disk tiled payload.
// Output size is ceil32(width_blocks) * height_blocks * bytes_per_block bytes.
std::vector<std::uint8_t> xbox360_xgaddress_retile_dxt(
    const std::vector<std::uint8_t>& linear,
    std::uint32_t width_blocks,
    std::uint32_t height_blocks,
    std::uint32_t bytes_per_block);

}  // namespace gf::textures
