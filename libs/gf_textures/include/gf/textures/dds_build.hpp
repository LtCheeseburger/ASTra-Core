#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace gf::textures {

/// Target DDS format for the build pipeline.
enum class DdsBuildFormat {
    DXT1,   ///< BC1 — suitable for colour maps without alpha
    DXT5,   ///< BC3 — suitable for colour maps with alpha, normal maps, etc.
};

/// Parameters driving a TGA→DDS conversion.
struct DdsBuildParams {
    DdsBuildFormat format     = DdsBuildFormat::DXT1;
    std::uint32_t  target_w   = 0; ///< 0 = use source dimensions
    std::uint32_t  target_h   = 0; ///< 0 = use source dimensions
    std::uint32_t  mip_count  = 0; ///< 0 = generate full mip chain down to 1×1
};

/// Build a complete DDS blob (magic + header + pixel data + mip chain) from
/// decoded RGBA pixels.
///
/// @param rgba      Source pixels, row-major top-to-bottom, 4 bytes per pixel.
/// @param src_w     Width of the source RGBA image.
/// @param src_h     Height of the source RGBA image.
/// @param params    Conversion parameters.
/// @param err       If non-null, receives an actionable error string on failure.
/// @return          Complete DDS file bytes, or empty on failure.
std::vector<std::uint8_t> build_dds_from_rgba(std::span<const std::uint8_t> rgba,
                                               std::uint32_t                  src_w,
                                               std::uint32_t                  src_h,
                                               const DdsBuildParams&          params,
                                               std::string*                   err = nullptr);

/// Convenience: parse a TGA file and compress it directly to DDS bytes.
///
/// @param tga_bytes  Raw TGA file bytes.
/// @param params     Conversion parameters (dimensions / format / mips).
/// @param err        If non-null, receives an actionable error string on failure.
/// @return           Complete DDS file bytes, or empty on failure.
std::vector<std::uint8_t> build_dds_from_tga(std::span<const std::uint8_t> tga_bytes,
                                               const DdsBuildParams&         params,
                                               std::string*                  err = nullptr);

} // namespace gf::textures
