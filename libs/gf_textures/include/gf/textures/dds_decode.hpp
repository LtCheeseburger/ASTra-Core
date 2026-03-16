
#pragma once
#include <cstdint>
#include <vector>
#include <optional>
#include <span>

namespace gf::textures {

// Formats we support decoding for preview.
// Note: "DXT" names are kept for clarity with legacy tooling.
enum class DdsFormat {
  Unknown = 0,
  DXT1,
  DXT3,
  DXT5,
  ATI1, // BC4
  ATI2, // BC5
  RGBA32 // uncompressed 32-bit (common A8R8G8B8 / X8R8G8B8 / A8B8G8R8)
};

struct ImageRGBA {
  uint32_t width = 0;
  uint32_t height = 0;
  std::vector<uint8_t> rgba;
};

struct DdsInfo {
  DdsFormat format = DdsFormat::Unknown;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t mipCount = 1;
};

// Parses DDS (or EA-wrapped DDS) header information.
// Returns nullopt if the input doesn't look like a DDS.
std::optional<DdsInfo> parse_dds_info(std::span<const uint8_t> bytes);

std::optional<ImageRGBA> decode_dds_mip_rgba(std::span<const uint8_t> bytes, uint32_t mip);

}
