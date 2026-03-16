#include "gf/textures/dds_decode.hpp"
#include "gf/textures/dds_validate.hpp"

#include <algorithm>
#include <bit>
#include <cstring>

namespace gf::textures {

static inline std::uint32_t rd_u32le(const std::uint8_t* p) {
  return (std::uint32_t)p[0] | ((std::uint32_t)p[1] << 8) | ((std::uint32_t)p[2] << 16) | ((std::uint32_t)p[3] << 24);
}

static inline std::uint16_t rd_u16le(const std::uint8_t* p) {
  return (std::uint16_t)p[0] | ((std::uint16_t)p[1] << 8);
}

static inline void rgb565_to_rgb8(std::uint16_t c, std::uint8_t& r, std::uint8_t& g, std::uint8_t& b) {
  r = static_cast<std::uint8_t>(((c >> 11) & 31u) * 255u / 31u);
  g = static_cast<std::uint8_t>(((c >> 5) & 63u) * 255u / 63u);
  b = static_cast<std::uint8_t>((c & 31u) * 255u / 31u);
}

static std::size_t mip_size_bytes(std::uint32_t w, std::uint32_t h, DdsFormat fmt) {
  if (fmt == DdsFormat::RGBA32) {
    return static_cast<std::size_t>(std::max(1u, w)) * static_cast<std::size_t>(std::max(1u, h)) * 4u;
  }
  const std::uint32_t bw = std::max(1u, (w + 3u) / 4u);
  const std::uint32_t bh = std::max(1u, (h + 3u) / 4u);
  // 8 bytes per 4x4 block for BC1/BC4, 16 bytes per 4x4 block for BC2/BC3/BC5.
  const std::size_t blockBytes = (fmt == DdsFormat::DXT1 || fmt == DdsFormat::ATI1) ? 8u : 16u;
  return static_cast<std::size_t>(bw) * static_cast<std::size_t>(bh) * blockBytes;
}

// BC4-style alpha block decode (used by ATI1/ATI2).
static void decode_bc4_block(const std::uint8_t* block, std::uint8_t out[16]) {
  const std::uint8_t a0 = block[0];
  const std::uint8_t a1 = block[1];
  std::uint8_t pal[8]{};
  pal[0] = a0; pal[1] = a1;
  if (a0 > a1) {
    pal[2] = static_cast<std::uint8_t>((6u * a0 + 1u * a1) / 7u);
    pal[3] = static_cast<std::uint8_t>((5u * a0 + 2u * a1) / 7u);
    pal[4] = static_cast<std::uint8_t>((4u * a0 + 3u * a1) / 7u);
    pal[5] = static_cast<std::uint8_t>((3u * a0 + 4u * a1) / 7u);
    pal[6] = static_cast<std::uint8_t>((2u * a0 + 5u * a1) / 7u);
    pal[7] = static_cast<std::uint8_t>((1u * a0 + 6u * a1) / 7u);
  } else {
    pal[2] = static_cast<std::uint8_t>((4u * a0 + 1u * a1) / 5u);
    pal[3] = static_cast<std::uint8_t>((3u * a0 + 2u * a1) / 5u);
    pal[4] = static_cast<std::uint8_t>((2u * a0 + 3u * a1) / 5u);
    pal[5] = static_cast<std::uint8_t>((1u * a0 + 4u * a1) / 5u);
    pal[6] = 0;
    pal[7] = 255;
  }

  std::uint64_t bits = 0;
  for (int i = 0; i < 6; ++i) bits |= static_cast<std::uint64_t>(block[2 + i]) << (8u * static_cast<std::uint32_t>(i));
  for (int i = 0; i < 16; ++i) {
    const std::uint32_t code = static_cast<std::uint32_t>((bits >> (3u * static_cast<std::uint32_t>(i))) & 7u);
    out[i] = pal[code];
  }
}

static void decode_bc1_block(const std::uint8_t* block, std::uint8_t outRGBA[16][4]) {
  const std::uint16_t c0 = rd_u16le(block + 0);
  const std::uint16_t c1 = rd_u16le(block + 2);
  std::uint8_t pal[4][4]{};

  rgb565_to_rgb8(c0, pal[0][0], pal[0][1], pal[0][2]); pal[0][3] = 255;
  rgb565_to_rgb8(c1, pal[1][0], pal[1][1], pal[1][2]); pal[1][3] = 255;

  if (c0 > c1) {
    pal[2][0] = static_cast<std::uint8_t>((2u * pal[0][0] + pal[1][0]) / 3u);
    pal[2][1] = static_cast<std::uint8_t>((2u * pal[0][1] + pal[1][1]) / 3u);
    pal[2][2] = static_cast<std::uint8_t>((2u * pal[0][2] + pal[1][2]) / 3u);
    pal[2][3] = 255;

    pal[3][0] = static_cast<std::uint8_t>((pal[0][0] + 2u * pal[1][0]) / 3u);
    pal[3][1] = static_cast<std::uint8_t>((pal[0][1] + 2u * pal[1][1]) / 3u);
    pal[3][2] = static_cast<std::uint8_t>((pal[0][2] + 2u * pal[1][2]) / 3u);
    pal[3][3] = 255;
  } else {
    pal[2][0] = static_cast<std::uint8_t>((pal[0][0] + pal[1][0]) / 2u);
    pal[2][1] = static_cast<std::uint8_t>((pal[0][1] + pal[1][1]) / 2u);
    pal[2][2] = static_cast<std::uint8_t>((pal[0][2] + pal[1][2]) / 2u);
    pal[2][3] = 255;
    pal[3][0] = 0; pal[3][1] = 0; pal[3][2] = 0; pal[3][3] = 0;
  }

  const std::uint32_t idx = rd_u32le(block + 4);
  for (int y = 0; y < 4; ++y) {
    for (int x = 0; x < 4; ++x) {
      const std::uint32_t code = (idx >> (2u * static_cast<std::uint32_t>(4 * y + x))) & 3u;
      outRGBA[4 * y + x][0] = pal[code][0];
      outRGBA[4 * y + x][1] = pal[code][1];
      outRGBA[4 * y + x][2] = pal[code][2];
      outRGBA[4 * y + x][3] = pal[code][3];
    }
  }
}

static void decode_dxt3_alpha(const std::uint8_t* block, std::uint8_t alphaOut[16]) {
  // 4-bit alpha values, 16 pixels packed into 8 bytes (little endian, row-major)
  for (int i = 0; i < 8; ++i) {
    const std::uint8_t b = block[i];
    const std::uint8_t a0 = b & 0x0Fu;
    const std::uint8_t a1 = (b >> 4) & 0x0Fu;
    alphaOut[2 * i + 0] = static_cast<std::uint8_t>(a0 * 17u);
    alphaOut[2 * i + 1] = static_cast<std::uint8_t>(a1 * 17u);
  }
}

static void decode_bc3_alpha(const std::uint8_t* block, std::uint8_t alphaOut[16]) {
  const std::uint8_t a0 = block[0];
  const std::uint8_t a1 = block[1];
  std::uint8_t pal[8]{};
  pal[0] = a0;
  pal[1] = a1;

  if (a0 > a1) {
    pal[2] = static_cast<std::uint8_t>((6u * a0 + 1u * a1) / 7u);
    pal[3] = static_cast<std::uint8_t>((5u * a0 + 2u * a1) / 7u);
    pal[4] = static_cast<std::uint8_t>((4u * a0 + 3u * a1) / 7u);
    pal[5] = static_cast<std::uint8_t>((3u * a0 + 4u * a1) / 7u);
    pal[6] = static_cast<std::uint8_t>((2u * a0 + 5u * a1) / 7u);
    pal[7] = static_cast<std::uint8_t>((1u * a0 + 6u * a1) / 7u);
  } else {
    pal[2] = static_cast<std::uint8_t>((4u * a0 + 1u * a1) / 5u);
    pal[3] = static_cast<std::uint8_t>((3u * a0 + 2u * a1) / 5u);
    pal[4] = static_cast<std::uint8_t>((2u * a0 + 3u * a1) / 5u);
    pal[5] = static_cast<std::uint8_t>((1u * a0 + 4u * a1) / 5u);
    pal[6] = 0;
    pal[7] = 255;
  }

  std::uint64_t bits = 0;
  for (int i = 0; i < 6; ++i) {
    bits |= (static_cast<std::uint64_t>(block[2 + i]) << (8u * static_cast<std::uint32_t>(i)));
  }

  for (int i = 0; i < 16; ++i) {
    const std::uint32_t code = static_cast<std::uint32_t>((bits >> (3u * static_cast<std::uint32_t>(i))) & 7u);
    alphaOut[i] = pal[code];
  }
}


static std::span<const std::uint8_t> unwrap_ea_dds(std::span<const std::uint8_t> in) {
  if (in.size() < 4) return in;
  if (std::memcmp(in.data(), "DDS ", 4) == 0) return in;

  // EASE-style: scan a small prefix for the real DDS header.
  // Some EA payloads prepend headers larger than 0x200 bytes.
  const std::size_t scanMax = std::min<std::size_t>(in.size(), 0x4000u);
  for (std::size_t off = 0; off + 4 <= scanMax; ++off) {
    if (std::memcmp(in.data() + off, "DDS ", 4) == 0) {
      return in.subspan(off);
    }
  }
  return in;
}

static DdsFormat classify_fourcc(std::uint32_t fourcc) {
  const std::uint32_t DXT1 = rd_u32le(reinterpret_cast<const std::uint8_t*>("DXT1"));
  const std::uint32_t DXT3 = rd_u32le(reinterpret_cast<const std::uint8_t*>("DXT3"));
  const std::uint32_t DXT5 = rd_u32le(reinterpret_cast<const std::uint8_t*>("DXT5"));
  const std::uint32_t ATI1 = rd_u32le(reinterpret_cast<const std::uint8_t*>("ATI1"));
  const std::uint32_t ATI2 = rd_u32le(reinterpret_cast<const std::uint8_t*>("ATI2"));
  const std::uint32_t BC4U = rd_u32le(reinterpret_cast<const std::uint8_t*>("BC4U"));
  const std::uint32_t BC4S = rd_u32le(reinterpret_cast<const std::uint8_t*>("BC4S"));
  const std::uint32_t BC5U = rd_u32le(reinterpret_cast<const std::uint8_t*>("BC5U"));
  const std::uint32_t BC5S = rd_u32le(reinterpret_cast<const std::uint8_t*>("BC5S"));
  if (fourcc == DXT1) return DdsFormat::DXT1;
  if (fourcc == DXT3) return DdsFormat::DXT3;
  if (fourcc == DXT5) return DdsFormat::DXT5;
  if (fourcc == ATI1 || fourcc == BC4U || fourcc == BC4S) return DdsFormat::ATI1;
  if (fourcc == ATI2 || fourcc == BC5U || fourcc == BC5S) return DdsFormat::ATI2;
  return DdsFormat::Unknown;
}

static std::uint32_t find_fourcc_fallback(const std::uint8_t* hdr, std::size_t n) {
  // Look for ASCII 'DXT1'/'DXT3'/'DXT5' inside the 128-byte header (EA variants sometimes shift fields).
  const char* keys[] = {"DXT1", "DXT3", "DXT5", "ATI1", "ATI2", "BC4U", "BC4S", "BC5U", "BC5S"};
  for (const char* k : keys) {
    const std::size_t klen = 4;
    for (std::size_t i = 0; i + klen <= n; ++i) {
      if (std::memcmp(hdr + i, k, klen) == 0) return rd_u32le(reinterpret_cast<const std::uint8_t*>(k));
    }
  }
  return 0;
}

std::optional<ImageRGBA> decode_dds_mip_rgba(std::span<const std::uint8_t> inBytes, std::uint32_t mip) {
  auto bytes = unwrap_ea_dds(inBytes);
  if (bytes.size() < 128) return std::nullopt;
  if (std::memcmp(bytes.data(), "DDS ", 4) != 0) return std::nullopt;

  const std::uint8_t* hdr = bytes.data();
  const std::uint32_t headerSize = rd_u32le(hdr + 4);
  if (headerSize < 124) return std::nullopt;

  const std::uint32_t height = rd_u32le(hdr + 12);
  const std::uint32_t width  = rd_u32le(hdr + 16);
  if (width == 0 || height == 0) return std::nullopt;

  std::uint32_t mipCount = rd_u32le(hdr + 28);
  if (mipCount == 0) mipCount = 1;

  // Pixel format struct begins at offset 76 into DDS_HEADER (after the 4-byte magic).
  const std::uint8_t* pf = hdr + 76;
  const std::uint32_t pfFlags     = rd_u32le(pf + 4);
  const std::uint32_t fourccRaw   = rd_u32le(pf + 8);
  const std::uint32_t rgbBitCount = rd_u32le(pf + 12);
  const std::uint32_t rmask       = rd_u32le(pf + 16);
  const std::uint32_t gmask       = rd_u32le(pf + 20);
  const std::uint32_t bmask       = rd_u32le(pf + 24);
  const std::uint32_t amask       = rd_u32le(pf + 28);

  std::uint32_t fourcc = fourccRaw;

  DdsFormat fmt = classify_fourcc(fourcc);
  if (fmt == DdsFormat::Unknown) {
    const std::uint32_t f2 = find_fourcc_fallback(hdr, 128);
    fmt = classify_fourcc(f2);
  }
  if (fmt == DdsFormat::Unknown) {
    // Some AST textures are stored as uncompressed 32-bit RGBA (FourCC==0, DDPF_RGB).
    // EASE can preview these; we support the common 32bpp layouts.
    constexpr std::uint32_t DDPF_RGB          = 0x40;
    if ((pfFlags & DDPF_RGB) && rgbBitCount == 32) {
      fmt = DdsFormat::RGBA32;
    } else {
      return std::nullopt;
    }
  }

  // Clamp mip
  if (mip >= mipCount) mip = 0;

  // Compute offset into the BC data assuming tight packing after the standard 128-byte header.
  // If EA uses unusual packing/padding, this still successfully previews mip0.
  std::uint32_t w = width;
  std::uint32_t h = height;
  std::size_t dataOffset = 128;
  for (std::uint32_t i = 0; i < mip; ++i) {
    dataOffset += mip_size_bytes(std::max(1u, w), std::max(1u, h), fmt);
    w = std::max(1u, w >> 1);
    h = std::max(1u, h >> 1);
  }

  const std::size_t need = mip_size_bytes(std::max(1u, w), std::max(1u, h), fmt);
  if (dataOffset + need > bytes.size()) {
    // Fallback to mip0 if the requested mip doesn't line up.
    mip = 0;
    w = width;
    h = height;
    dataOffset = 128;
    if (dataOffset + mip_size_bytes(w, h, fmt) > bytes.size()) return std::nullopt;
  }

  const std::uint8_t* src = bytes.data() + dataOffset;

  ImageRGBA img;
  img.width = w;
  img.height = h;
  img.rgba.resize(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4u);

  if (fmt == DdsFormat::RGBA32) {
    // Generic bitmask extraction (assumes channel masks do not overlap).
    auto chan_u8 = [](std::uint32_t px, std::uint32_t mask, std::uint8_t defaultVal) -> std::uint8_t {
      if (mask == 0u) return defaultVal;
      const unsigned shift = std::countr_zero(mask);
      const unsigned bits  = std::popcount(mask >> shift);
      const std::uint32_t v = (px & mask) >> shift;
      const std::uint32_t denom = (bits >= 32u) ? 0xFFFFFFFFu : ((1u << bits) - 1u);
      if (denom == 0u) return defaultVal;
      return static_cast<std::uint8_t>((v * 255u) / denom);
    };

    const std::size_t pixelCount = static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
    if (dataOffset + pixelCount * 4u > bytes.size()) return std::nullopt;

    for (std::size_t i = 0; i < pixelCount; ++i) {
      const std::uint32_t px = rd_u32le(src + i * 4u);
      const std::uint8_t r = chan_u8(px, rmask, 0);
      const std::uint8_t g = chan_u8(px, gmask, 0);
      const std::uint8_t b = chan_u8(px, bmask, 0);
      const std::uint8_t a = chan_u8(px, amask, 255);
      const std::size_t o = i * 4u;
      img.rgba[o + 0] = r;
      img.rgba[o + 1] = g;
      img.rgba[o + 2] = b;
      img.rgba[o + 3] = a;
    }
    return img;
  }

  const std::uint32_t bw = std::max(1u, (w + 3u) / 4u);
  const std::uint32_t bh = std::max(1u, (h + 3u) / 4u);
  const std::size_t blockBytes = (fmt == DdsFormat::DXT1 || fmt == DdsFormat::ATI1) ? 8u : 16u;

  for (std::uint32_t by = 0; by < bh; ++by) {
    for (std::uint32_t bx = 0; bx < bw; ++bx) {
      const std::size_t blockIndex = static_cast<std::size_t>(by) * bw + bx;
      const std::uint8_t* block = src + blockIndex * blockBytes;

      std::uint8_t px[16][4]{};
      if (fmt == DdsFormat::DXT1) {
        decode_bc1_block(block, px);
      } else if (fmt == DdsFormat::DXT3) {
        std::uint8_t a[16]{};
        decode_dxt3_alpha(block, a);
        decode_bc1_block(block + 8, px);
        for (int i = 0; i < 16; ++i) px[i][3] = a[i];
      } else if (fmt == DdsFormat::DXT5) {
        std::uint8_t a[16]{};
        decode_bc3_alpha(block, a);
        decode_bc1_block(block + 8, px);
        for (int i = 0; i < 16; ++i) px[i][3] = a[i];
      } else if (fmt == DdsFormat::ATI1) {
        std::uint8_t v[16]{};
        decode_bc4_block(block, v);
        for (int i = 0; i < 16; ++i) {
          px[i][0] = v[i];
          px[i][1] = v[i];
          px[i][2] = v[i];
          px[i][3] = 255;
        }
      } else if (fmt == DdsFormat::ATI2) {
        std::uint8_t r[16]{};
        std::uint8_t g[16]{};
        decode_bc4_block(block, r);
        decode_bc4_block(block + 8, g);
        for (int i = 0; i < 16; ++i) {
          px[i][0] = r[i];
          px[i][1] = g[i];
          px[i][2] = 0;
          px[i][3] = 255;
        }
      }

      for (std::uint32_t y = 0; y < 4; ++y) {
        for (std::uint32_t x = 0; x < 4; ++x) {
          const std::uint32_t ix = bx * 4u + x;
          const std::uint32_t iy = by * 4u + y;
          if (ix >= w || iy >= h) continue;

          const std::size_t dst = (static_cast<std::size_t>(iy) * w + ix) * 4u;
          const std::uint8_t* s = px[4 * y + x];
          img.rgba[dst + 0] = s[0];
          img.rgba[dst + 1] = s[1];
          img.rgba[dst + 2] = s[2];
          img.rgba[dst + 3] = s[3];
        }
      }
    }
  }

  return img;
}

std::optional<DdsInfo> parse_dds_info(std::span<const std::uint8_t> inBytes) {
  return dds_info_from_validation(inspect_dds(inBytes));
}

} // namespace gf::textures
