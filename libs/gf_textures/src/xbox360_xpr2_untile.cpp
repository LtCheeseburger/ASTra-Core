#include <gf/textures/xbox360_xpr2_untile.hpp>

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <vector>

namespace gf::textures {

namespace {

static inline std::uint32_t align_up(std::uint32_t v, std::uint32_t a) {
  return (v + (a - 1u)) & ~(a - 1u);
}

// log2 for power-of-two values (8 → 3, 16 → 4).
static inline std::uint32_t log2_pow2(std::uint32_t v) {
  std::uint32_t n = 0;
  while (v > 1u) { v >>= 1u; ++n; }
  return n;
}

// Canonical XGAddress2DTiledOffset tiling math, operating in *block* units.
// Returns the block index in the tiled buffer that corresponds to block (x, y)
// in the logical (linear) surface.
static inline std::uint32_t xgaddress_tiled_block(
    std::uint32_t x, std::uint32_t y,
    std::uint32_t aligned_width_blocks,
    std::uint32_t log_bpb) {

  const std::uint32_t macro =
      ((x >> 5u) + (y >> 5u) * (aligned_width_blocks >> 5u)) << (log_bpb + 7u);

  const std::uint32_t micro =
      ((x & 7u) + ((y & 0xEu) << 2u)) << log_bpb;

  const std::uint32_t offset =
      macro + ((micro & ~0xFu) << 1u) + (micro & 0xFu) + ((y & 1u) << 4u);

  const std::uint32_t address =
      (((offset & ~0x1FFu) << 3u) +
       ((y & 16u) << 7u) +
       ((offset & 0x1C0u) << 2u) +
       (((((y & 8u) >> 2u) + (x >> 3u)) & 3u) << 6u) +
       (offset & 0x3Fu));

  return address >> log_bpb;
}

}  // namespace

// Untile an Xbox 360 XPR2 TX2D surface using an XGAddress-style *macrotile* layout.
//
// For block-compressed textures, treat each 4x4 block as a "texel" (unit).
//
// This implementation is intentionally simple and deterministic:
// - Pitch is aligned to 32 blocks (common for 360 tiled 2D textures).
// - Macro tiles are treated as 32x16 blocks for DXT (the key difference vs simple Morton/32x32 layouts).
//
// Input and output are just the compressed payload (no DDS headers).
std::vector<std::uint8_t> xbox360_xgaddress_untile_dxt(
    const std::vector<std::uint8_t>& tiled,
    std::uint32_t width_blocks,
    std::uint32_t height_blocks,
    std::uint32_t bytes_per_block) {
  // Xbox 360 uses the XGAddress 2D tiled layout.
  // This implementation matches the canonical XGAddress2DTiledOffset math
  // (see BlueRaja/UModel GetTiledOffset), operating in *block* units.
  if (width_blocks == 0 || height_blocks == 0 || bytes_per_block == 0) {
    return {};
  }

  const std::uint32_t log_bpb            = log2_pow2(bytes_per_block);
  const std::uint32_t aligned_width_blocks = (width_blocks + 31u) & ~31u;
  const std::size_t   out_size           = static_cast<std::size_t>(width_blocks) *
                                           static_cast<std::size_t>(height_blocks) *
                                           static_cast<std::size_t>(bytes_per_block);

  std::vector<std::uint8_t> out(out_size, 0);

  for (std::uint32_t y = 0; y < height_blocks; ++y) {
    for (std::uint32_t x = 0; x < width_blocks; ++x) {
      const std::uint32_t src_block =
          xgaddress_tiled_block(x, y, aligned_width_blocks, log_bpb);

      // Guard: tiled buffer must cover this block index.
      if (src_block >= aligned_width_blocks * height_blocks) continue;

      const std::size_t src_off = static_cast<std::size_t>(src_block)  * bytes_per_block;
      const std::size_t dst_off = (static_cast<std::size_t>(y) * width_blocks +
                                   static_cast<std::size_t>(x)) * bytes_per_block;

      if (src_off + bytes_per_block <= tiled.size() &&
          dst_off + bytes_per_block <= out.size()) {
        std::memcpy(out.data() + dst_off, tiled.data() + src_off, bytes_per_block);
      }
    }
  }

  return out;
}

// Inverse of xbox360_xgaddress_untile_dxt: re-tiles a linear surface back into
// the XGAddress tiled layout.  Output is aligned_width * height_blocks blocks.
std::vector<std::uint8_t> xbox360_xgaddress_retile_dxt(
    const std::vector<std::uint8_t>& linear,
    std::uint32_t width_blocks,
    std::uint32_t height_blocks,
    std::uint32_t bytes_per_block) {

  if (width_blocks == 0 || height_blocks == 0 || bytes_per_block == 0) return {};

  const std::uint32_t log_bpb             = log2_pow2(bytes_per_block);
  const std::uint32_t aligned_width_blocks = (width_blocks + 31u) & ~31u;
  const std::size_t   tiled_size          = static_cast<std::size_t>(aligned_width_blocks) *
                                            static_cast<std::size_t>(height_blocks) *
                                            static_cast<std::size_t>(bytes_per_block);

  std::vector<std::uint8_t> tiled(tiled_size, 0);

  for (std::uint32_t y = 0; y < height_blocks; ++y) {
    for (std::uint32_t x = 0; x < width_blocks; ++x) {
      const std::uint32_t dst_block =
          xgaddress_tiled_block(x, y, aligned_width_blocks, log_bpb);

      if (dst_block >= aligned_width_blocks * height_blocks) continue;

      const std::size_t src_off = (static_cast<std::size_t>(y) * width_blocks +
                                   static_cast<std::size_t>(x)) * bytes_per_block;
      const std::size_t dst_off = static_cast<std::size_t>(dst_block) * bytes_per_block;

      if (src_off + bytes_per_block <= linear.size() &&
          dst_off + bytes_per_block <= tiled.size()) {
        std::memcpy(tiled.data() + dst_off, linear.data() + src_off, bytes_per_block);
      }
    }
  }

  return tiled;
}

}  // namespace gf::textures
