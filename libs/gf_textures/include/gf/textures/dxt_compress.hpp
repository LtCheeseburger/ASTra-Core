#pragma once

#include <cstdint>
#include <vector>
#include <span>

namespace gf::textures {

/// Supported block-compression targets for this encoder.
enum class DxtEncodeFormat {
    DXT1,  ///< BC1 — opaque or 1-bit alpha, 0.5 bytes/texel
    DXT5,  ///< BC3 — full alpha, 1 byte/texel
    BC4,   ///< ATI1 — single channel (R only), 0.5 bytes/texel, same size as DXT1
    BC5,   ///< ATI2 — two channels (RG), 1 byte/texel, same size as DXT5
};

/// Compress a single RGBA mip level into a DXT block stream.
///
/// @param rgba         Input pixels, row-major top-to-bottom, 4 bytes per pixel (R,G,B,A).
/// @param width        Width in texels. Must be > 0.
/// @param height       Height in texels. Must be > 0.
/// @param format       Target compression format.
/// @return             Compressed block data. Width and height are implicitly rounded up
///                     to the next multiple of 4 before block encoding.
std::vector<std::uint8_t> dxt_compress(std::span<const std::uint8_t> rgba,
                                        std::uint32_t                  width,
                                        std::uint32_t                  height,
                                        DxtEncodeFormat                format);

/// Compute the number of compressed bytes for a DXT image of the given dimensions.
std::size_t dxt_compressed_size(std::uint32_t width,
                                 std::uint32_t height,
                                 DxtEncodeFormat format) noexcept;

} // namespace gf::textures
