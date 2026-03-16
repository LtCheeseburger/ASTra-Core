#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace gf::textures {

/// Decoded TGA image in 32-bit RGBA.
struct TgaImage {
    std::uint32_t width  = 0;
    std::uint32_t height = 0;
    /// Row-major, top-to-bottom, 4 bytes per pixel (R,G,B,A).
    std::vector<std::uint8_t> rgba;
};

/// Parse a TGA file from raw bytes.
///
/// Supports:
///   - Type 2 (uncompressed true-color), 24-bit and 32-bit
///   - Type 3 (uncompressed grayscale), 8-bit  (returned as R=G=B=value, A=255)
///   - Type 10 (RLE true-color), 24-bit and 32-bit
///
/// On failure returns nullopt and, if err != nullptr, fills *err with an
/// actionable diagnostic message.
std::optional<TgaImage> read_tga(std::span<const std::uint8_t> bytes,
                                  std::string* err = nullptr);

} // namespace gf::textures
