// XPR2 (Xbox 360) -> DDS rebuild helper
//
// EA XPR2 TX2D payloads are stored with:
//   1. 16-bit word swap applied to the entire texture payload (swap16).
//   2. Xbox 360 XGAddress2DTiledOffset tiling applied to the block-compressed data.
//
// Decode pipeline (matches Noesis tex_XBox360_XPR.py exactly):
//   raw_xpr2_data → swap16 → XGAddress_untile → standard PC DXT/BC data
//
// Import pipeline (inverse):
//   PC DXT/BC data → XGAddress_retile → swap16 → raw_xpr2_data

#include <gf/textures/xpr2_rebuild.hpp>

#include <gf/textures/xbox360_xpr2_untile.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace gf::textures {

namespace {

enum class Xpr2UntileMode {
  Auto = 0,
  Linear,
  Morton,
  XGAddress,
};

static Xpr2UntileMode xpr2_untile_mode_from_env() {
  // Debug override (no GUI wiring required yet).
  // Examples:
  //   ASTRA_XPR2_UNTILE_MODE=linear
  //   ASTRA_XPR2_UNTILE_MODE=morton
  //   ASTRA_XPR2_UNTILE_MODE=xgaddress
  const char* v = std::getenv("ASTRA_XPR2_UNTILE_MODE");
  if (!v || !*v) v = std::getenv("GF_XPR2_UNTILE_MODE");
  if (!v || !*v) return Xpr2UntileMode::Auto;

  std::string s(v);
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
    return char(std::tolower(c));
  });

  if (s == "linear") return Xpr2UntileMode::Linear;
  if (s == "morton" || s == "tile" || s == "untile") return Xpr2UntileMode::Morton;
  if (s == "xgaddress" || s == "xg") return Xpr2UntileMode::XGAddress;
  return Xpr2UntileMode::Auto;
}

static std::uint32_t be32(const std::uint8_t* p) {
  return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 8) |
         std::uint32_t(p[3]);
}

static std::uint16_t be16(const std::uint8_t* p) {
  return (std::uint16_t(p[0]) << 8) | std::uint16_t(p[1]);
}

static std::uint32_t le32(const std::uint8_t* p) {
  return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) | (std::uint32_t(p[2]) << 16) |
         (std::uint32_t(p[3]) << 24);
}

// EA XPR2 payloads are stored as 16-bit words with byte order swapped on disk.
// A simple swap of every adjacent byte pair converts to standard PC DXT layout.
// Reference: Noesis plugin rapi.swapEndianArray(data, 2) + rapi.imageUntile360DXT().
// No additional per-block DXT endian fixup is needed after this swap.
static void swap16_inplace(std::vector<std::uint8_t>& v) {
  for (std::size_t i = 0; i + 1 < v.size(); i += 2) {
    std::swap(v[i], v[i + 1]);
  }
}

static std::vector<std::uint8_t> build_dds_header(std::uint32_t w, std::uint32_t h,
                                                  const char fourcc[4],
                                                  std::uint32_t mipCount = 1u) {
  // Minimal DDS header (128 bytes)
  std::vector<std::uint8_t> dds(128, 0);
  auto w32 = [&](std::size_t off, std::uint32_t v) {
    dds[off + 0] = std::uint8_t(v & 0xFF);
    dds[off + 1] = std::uint8_t((v >> 8) & 0xFF);
    dds[off + 2] = std::uint8_t((v >> 16) & 0xFF);
    dds[off + 3] = std::uint8_t((v >> 24) & 0xFF);
  };

  dds[0] = 'D';
  dds[1] = 'D';
  dds[2] = 'S';
  dds[3] = ' ';
  w32(4, 124);
  // DDSD_CAPS|DDSD_HEIGHT|DDSD_WIDTH|DDSD_PIXELFORMAT; add DDSD_MIPMAPCOUNT when mips > 1
  w32(8, 0x00001007u | (mipCount > 1u ? 0x00020000u : 0u));
  w32(12, h);
  w32(16, w);
  if (mipCount > 1u) w32(28, mipCount); // dwMipMapCount

  // Pixel format
  w32(76, 32);
  w32(80, 0x00000004); // FOURCC
  dds[84] = std::uint8_t(fourcc[0]);
  dds[85] = std::uint8_t(fourcc[1]);
  dds[86] = std::uint8_t(fourcc[2]);
  dds[87] = std::uint8_t(fourcc[3]);

  // DDSCAPS_TEXTURE; add DDSCAPS_MIPMAP|DDSCAPS_COMPLEX when mips > 1
  w32(108, 0x00001000u | (mipCount > 1u ? 0x00400008u : 0u));
  return dds;
}

struct FmtInfo {
  const char* fourcc;
  std::uint32_t bytes_per_block;
};

static std::optional<FmtInfo> fmt_from_xpr2(std::uint8_t fmt) {
  // Noesis tex_XBox360_XPR.py format code mapping:
  //   0x52 -> DXT1       (8  bytes/block, colour)
  //   0x53 -> DXT3       (16 bytes/block, colour+alpha)
  //   0x54 -> DXT5       (16 bytes/block, colour+alpha)
  //   0x71 -> ATI2/BC5   (16 bytes/block, two BC4 channels — used for normal maps)
  //   0x7C -> DXT1 (normal-map variant, identical block layout to 0x52)
  switch (fmt) {
    case 0x52: return FmtInfo{"DXT1", 8};
    case 0x53: return FmtInfo{"DXT3", 16};
    case 0x54: return FmtInfo{"DXT5", 16};
    case 0x71: return FmtInfo{"ATI2", 16};
    case 0x7C: return FmtInfo{"DXT1", 8};
    default:   return std::nullopt;
  }
}

// Xbox 360 DXT textures are stored in a tiled layout.
// We untile in units of 4x4 blocks: 32x32 texel tiles == 8x8 blocks.
static constexpr std::uint32_t morton2_3bit(std::uint32_t x, std::uint32_t y) {
  x &= 7u;
  y &= 7u;
  std::uint32_t z = 0;
  z |= (x & 1u) << 0;
  z |= (y & 1u) << 1;
  z |= (x & 2u) << 1;
  z |= (y & 2u) << 2;
  z |= (x & 4u) << 2;
  z |= (y & 4u) << 3;
  return z;
}

static std::vector<std::uint8_t> untile_360_dxt_padded(const std::vector<std::uint8_t>& tiled,
                                                       std::uint32_t width_blocks,
                                                       std::uint32_t height_blocks,
                                                       std::uint32_t src_pitch_blocks,
                                                       std::uint32_t src_height_blocks,
                                                       std::uint32_t bytes_per_block) {
  // Xbox 360 2D tiled surfaces are laid out in 4KB macro-tiles.
  // For BC formats, tiles are 32x32 texels == 8x8 blocks.
  // Real surfaces often have padding in pitch/height; the source buffer must
  // be interpreted using the padded dimensions, while the destination is clipped
  // to the logical width/height.
  std::vector<std::uint8_t> linear(
      std::size_t(width_blocks) * std::size_t(height_blocks) * std::size_t(bytes_per_block), 0);

  const std::uint32_t tile_w = 8; // blocks
  const std::uint32_t tile_h = 8; // blocks
  const std::uint32_t tiles_x = (src_pitch_blocks + tile_w - 1) / tile_w;
  const std::uint32_t tiles_y = (src_height_blocks + tile_h - 1) / tile_h;

  const std::size_t blocks_per_tile = std::size_t(tile_w) * std::size_t(tile_h); // 64
  const std::size_t tile_bytes = blocks_per_tile * std::size_t(bytes_per_block);

  for (std::uint32_t ty = 0; ty < tiles_y; ++ty) {
    for (std::uint32_t tx = 0; tx < tiles_x; ++tx) {
      const std::size_t tile_index = std::size_t(ty) * std::size_t(tiles_x) + std::size_t(tx);
      const std::size_t tile_base = tile_index * tile_bytes;
      if (tile_base >= tiled.size()) continue;

      for (std::uint32_t by = 0; by < tile_h; ++by) {
        for (std::uint32_t bx = 0; bx < tile_w; ++bx) {
          const std::uint32_t dst_x = tx * tile_w + bx;
          const std::uint32_t dst_y = ty * tile_h + by;
          if (dst_x >= width_blocks || dst_y >= height_blocks) continue;

          const std::size_t mort = std::size_t(morton2_3bit(bx, by));
          const std::size_t src = tile_base + mort * std::size_t(bytes_per_block);
          const std::size_t dst =
              (std::size_t(dst_y) * std::size_t(width_blocks) + std::size_t(dst_x)) *
              std::size_t(bytes_per_block);

          if (src + bytes_per_block <= tiled.size() && dst + bytes_per_block <= linear.size()) {
            std::memcpy(linear.data() + dst, tiled.data() + src, bytes_per_block);
          }
        }
      }
    }
  }

  return linear;
}


// Inverse of untile_360_dxt_padded: tiles a logical (linear) DXT surface into
// the padded tiled layout used by Xbox 360.  The output buffer is zeroed first
// so any padding blocks not covered by the logical surface are left as zero.
static std::vector<std::uint8_t> tile_360_dxt_padded(const std::vector<std::uint8_t>& linear,
                                                     std::uint32_t width_blocks,
                                                     std::uint32_t height_blocks,
                                                     std::uint32_t dst_pitch_blocks,
                                                     std::uint32_t dst_height_blocks,
                                                     std::uint32_t bytes_per_block) {
  std::vector<std::uint8_t> tiled(
      std::size_t(dst_pitch_blocks) * std::size_t(dst_height_blocks) * std::size_t(bytes_per_block),
      0);

  const std::uint32_t tile_w = 8;
  const std::uint32_t tile_h = 8;
  const std::uint32_t tiles_x = (dst_pitch_blocks  + tile_w - 1) / tile_w;
  const std::uint32_t tiles_y = (dst_height_blocks + tile_h - 1) / tile_h;

  const std::size_t blocks_per_tile = std::size_t(tile_w) * std::size_t(tile_h);
  const std::size_t tile_bytes = blocks_per_tile * std::size_t(bytes_per_block);

  for (std::uint32_t ty = 0; ty < tiles_y; ++ty) {
    for (std::uint32_t tx = 0; tx < tiles_x; ++tx) {
      const std::size_t tile_index = std::size_t(ty) * std::size_t(tiles_x) + std::size_t(tx);
      const std::size_t tile_base  = tile_index * tile_bytes;
      if (tile_base >= tiled.size()) continue;

      for (std::uint32_t by = 0; by < tile_h; ++by) {
        for (std::uint32_t bx = 0; bx < tile_w; ++bx) {
          const std::uint32_t src_x = tx * tile_w + bx;
          const std::uint32_t src_y = ty * tile_h + by;
          if (src_x >= width_blocks || src_y >= height_blocks) continue;

          const std::size_t mort = std::size_t(morton2_3bit(bx, by));
          const std::size_t dst  = tile_base + mort * std::size_t(bytes_per_block);
          const std::size_t src  = (std::size_t(src_y) * std::size_t(width_blocks) +
                                    std::size_t(src_x)) * std::size_t(bytes_per_block);

          if (src + bytes_per_block <= linear.size() && dst + bytes_per_block <= tiled.size())
            std::memcpy(tiled.data() + dst, linear.data() + src, bytes_per_block);
        }
      }
    }
  }
  return tiled;
}

} // namespace

std::optional<std::vector<std::uint8_t>> rebuild_xpr2_dds_first(std::span<const std::uint8_t> xpr2,
                                                                std::string* outName,
                                                                bool all_mips) {
  auto fail = [&](const std::string& msg) -> std::optional<std::vector<std::uint8_t>> {
    if (outName) *outName = msg;
    return std::nullopt;
  };

  if (xpr2.size() < 0x20) return fail("XPR2 too small");
  if (std::memcmp(xpr2.data(), "XPR2", 4) != 0) return fail("Not an XPR2 file");

  const std::uint32_t header1 = be32(xpr2.data() + 4); // base
  const std::uint32_t header2 = be32(xpr2.data() + 8); // total data bytes (aligned)
  const std::uint32_t count = be32(xpr2.data() + 12);
  if (count == 0) return fail("XPR2 has no entries");

  const std::size_t tableOff = 16;
  if (tableOff + std::size_t(count) * 16 > xpr2.size()) return fail("XPR2 table truncated");

  const std::uint32_t dataBase = header1 + 12; // Noesis plugin
  if (dataBase >= xpr2.size()) return fail("XPR2 data base out of range");

  // Find first TX2D entry.
  std::uint32_t tx2d_off_struct = 0;
  std::uint32_t tx2d_off_name = 0;
  for (std::uint32_t i = 0; i < count; ++i) {
    const std::uint8_t* e = xpr2.data() + tableOff + std::size_t(i) * 16;
    const std::uint32_t type = be32(e + 0);
    if (type == 0x54583244u /* 'TX2D' */) {
      tx2d_off_struct = be32(e + 4);
      tx2d_off_name = be32(e + 12);
      break;
    }
  }
  if (!tx2d_off_struct) return fail("No TX2D entries found");

  // Extract name (best-effort) like Noesis: seek(nameOff + 12), read C string.
  if (outName) {
    outName->clear();
    if (tx2d_off_name) {
      const std::size_t nameOff = std::size_t(tx2d_off_name) + 12;
      if (nameOff < xpr2.size()) {
        std::size_t end = nameOff;
        while (end < xpr2.size() && xpr2[end] != 0) ++end;
        *outName = std::string(reinterpret_cast<const char*>(xpr2.data() + nameOff), end - nameOff);
      }
    }
  }

  // TX2D metadata (Noesis): seek(tx2d + 12), skip 33, then read >H B H H ...
  const std::size_t metaBase = std::size_t(tx2d_off_struct) + 12;
  const std::size_t meta = metaBase + 33;
  if (meta + 7 > xpr2.size()) return fail("TX2D metadata out of range");

  const std::uint16_t dataOffBlocks = be16(xpr2.data() + meta + 0);
  const std::uint8_t fmt = xpr2[meta + 2];
  const std::uint16_t hBlocks = be16(xpr2.data() + meta + 3);
  const std::uint16_t wPacked = be16(xpr2.data() + meta + 5);

  const std::uint32_t height = (std::uint32_t(hBlocks) + 1u) * 8u;
  const std::uint32_t width = (std::uint32_t(wPacked) + 1u) & 0x1FFFu;
  if (!width || !height) return fail("Invalid TX2D dimensions");

  const auto finfo = fmt_from_xpr2(fmt);
  if (!finfo) return fail("Unsupported XPR2 format code: " + std::to_string(fmt));

  if (header2 < 0x100) return fail("XPR2 data length too small");
  const std::uint32_t totalBlocks = header2 / 0x100;
  if (dataOffBlocks >= totalBlocks) return fail("XPR2 data offset out of range");

  const std::size_t dataOff = std::size_t(dataBase) + std::size_t(dataOffBlocks) * 0x100;
  if (dataOff >= xpr2.size()) return fail("XPR2 data offset outside file");

  // Decode just the top mip for preview.
  const std::uint32_t widthBlocks = (width + 3u) / 4u;
  const std::uint32_t heightBlocks = (height + 3u) / 4u;
  const std::size_t mipBytes = std::size_t(widthBlocks) * std::size_t(heightBlocks) *
                               std::size_t(finfo->bytes_per_block);
  if (dataOff + mipBytes > xpr2.size()) return fail("XPR2 texture data truncated");

  std::vector<std::uint8_t> raw(xpr2.begin() + std::ptrdiff_t(dataOff),
                                xpr2.begin() + std::ptrdiff_t(dataOff + mipBytes));

  // Many X360 tiled surfaces include padding in pitch/height (4KB macro-tiles).
  // When untile logic reads into padding, using only logical mip bytes can produce
  // structured corruption or large black regions.
  auto align_up_u32 = [](std::uint32_t v, std::uint32_t a) {
    return (v + (a - 1u)) & ~(a - 1u);
  };
  const std::uint32_t pitchBlocks = align_up_u32(widthBlocks, 32u);
  const std::uint32_t tileHBlocks = (finfo->bytes_per_block == 8u) ? 16u : 8u;
  const std::uint32_t paddedHeightBlocks = align_up_u32(heightBlocks, tileHBlocks);
  const std::size_t surfBytes =
      std::size_t(pitchBlocks) * std::size_t(paddedHeightBlocks) * std::size_t(finfo->bytes_per_block);

  std::vector<std::uint8_t> rawSurf;
  if (dataOff + surfBytes <= xpr2.size()) {
    rawSurf.assign(xpr2.begin() + std::ptrdiff_t(dataOff),
                   xpr2.begin() + std::ptrdiff_t(dataOff + surfBytes));
  }

  // ── Decode pipeline (matches Noesis tex_XBox360_XPR.py exactly) ────────────
  // Step 1: swap16 (Noesis: rapi.swapEndianArray(data, 2))
  // Step 2: XGAddress untile (Noesis: rapi.imageUntile360DXT)
  //
  // No additional per-block DXT endian fixup — swap16 alone is sufficient.
  // The earlier xbox360_fixup_dxt_endian_inplace (dword-swap of block halves)
  // was incorrect; it corrupted both color endpoints and index bits.
  //
  // Environment override: ASTRA_XPR2_UNTILE_MODE=linear|morton|xgaddress
  //   linear    — swap16 only, no untile (rare non-tiled files)
  //   morton    — swap16 + simple Morton 8×8-block tile (debug/legacy)
  //   xgaddress — swap16 + XGAddress untile (default, correct for EA NCAA/Madden)

  const Xpr2UntileMode forced = xpr2_untile_mode_from_env();

  // Surface data with pitch-padding for the untile step.
  // rawSurf is preferred; falls back to mip-only buffer for small textures
  // where the full padded surface may not fit within the XPR2 file.
  std::vector<std::uint8_t>* pSurf = rawSurf.empty() ? &raw : &rawSurf;

  auto fmt_hex = [](std::uint8_t v) {
    char b[4]; std::snprintf(b, sizeof(b), "%02X", v); return std::string(b);
  };

  if (forced == Xpr2UntileMode::Linear) {
    std::vector<std::uint8_t> cand = raw;
    swap16_inplace(cand);
    if (outName) *outName += "\n[xpr2] mode=linear(env) fmt=0x" + fmt_hex(fmt) +
        " " + std::to_string(width) + "x" + std::to_string(height);
    auto dds = build_dds_header(width, height, finfo->fourcc);
    dds.insert(dds.end(), cand.begin(), cand.end());
    return dds;
  }

  if (forced == Xpr2UntileMode::Morton) {
    std::vector<std::uint8_t> cand = *pSurf;
    swap16_inplace(cand);
    cand = untile_360_dxt_padded(cand, widthBlocks, heightBlocks,
        rawSurf.empty() ? widthBlocks : pitchBlocks,
        rawSurf.empty() ? heightBlocks : paddedHeightBlocks,
        finfo->bytes_per_block);
    if (outName) *outName += "\n[xpr2] mode=morton(env) fmt=0x" + fmt_hex(fmt) +
        " " + std::to_string(width) + "x" + std::to_string(height);
    auto dds = build_dds_header(width, height, finfo->fourcc);
    dds.insert(dds.end(), cand.begin(), cand.end());
    return dds;
  }

  // Default: XGAddress untile — correct for EA XPR2 (NCAA Football / Madden Xbox 360).
  if (!all_mips) {
    std::vector<std::uint8_t> cand = *pSurf;
    swap16_inplace(cand);
    cand = xbox360_xgaddress_untile_dxt(cand, widthBlocks, heightBlocks, finfo->bytes_per_block);

    if (outName) *outName += "\n[xpr2] mode=xgaddress fmt=0x" + fmt_hex(fmt) +
        " " + std::to_string(width) + "x" + std::to_string(height) +
        " surfIn=" + std::to_string(pSurf->size()) + "B";

    auto dds = build_dds_header(width, height, finfo->fourcc);
    dds.insert(dds.end(), cand.begin(), cand.end());
    return dds;
  }

  // Multi-mip export: walk every mip level stored in the XPR2 payload.
  // Each mip is independently XGAddress-tiled; pitch is aligned to 32 blocks.
  struct MipRef { std::size_t off; std::uint32_t wBlocks, hBlocks; };
  std::vector<MipRef> mipRefs;
  {
    std::size_t mipOff = dataOff;
    std::uint32_t mWB = widthBlocks, mHB = heightBlocks;
    while (mWB > 0 && mHB > 0) {
      const std::uint32_t alignedW = (mWB + 31u) & ~31u;
      const std::size_t tiledBytes = std::size_t(alignedW) * std::size_t(mHB) *
                                     std::size_t(finfo->bytes_per_block);
      if (mipOff + tiledBytes > xpr2.size()) break;
      mipRefs.push_back({mipOff, mWB, mHB});
      if (mWB == 1u && mHB == 1u) break;
      mipOff += tiledBytes;
      mWB = std::max(1u, mWB >> 1u);
      mHB = std::max(1u, mHB >> 1u);
    }
  }

  if (mipRefs.empty()) return fail("No mip data found in XPR2");

  const auto mipCount = static_cast<std::uint32_t>(mipRefs.size());
  auto dds = build_dds_header(width, height, finfo->fourcc, mipCount);

  for (const auto& ref : mipRefs) {
    const std::uint32_t alignedW = (ref.wBlocks + 31u) & ~31u;
    const std::size_t tiledBytes = std::size_t(alignedW) * std::size_t(ref.hBlocks) *
                                   std::size_t(finfo->bytes_per_block);
    std::vector<std::uint8_t> mipRaw(
        xpr2.begin() + std::ptrdiff_t(ref.off),
        xpr2.begin() + std::ptrdiff_t(ref.off + tiledBytes));
    swap16_inplace(mipRaw);
    auto linear = xbox360_xgaddress_untile_dxt(mipRaw, ref.wBlocks, ref.hBlocks,
                                               finfo->bytes_per_block);
    dds.insert(dds.end(), linear.begin(), linear.end());
  }

  if (outName) *outName += "\n[xpr2] mode=xgaddress fmt=0x" + fmt_hex(fmt) +
      " " + std::to_string(width) + "x" + std::to_string(height) +
      " mips=" + std::to_string(mipCount);

  return dds;
}

std::optional<Xpr2Info> parse_xpr2_info(std::span<const std::uint8_t> xpr2) {
  if (xpr2.size() < 0x20) return std::nullopt;
  if (std::memcmp(xpr2.data(), "XPR2", 4) != 0) return std::nullopt;

  const std::uint32_t header1 = be32(xpr2.data() + 4);
  const std::uint32_t header2 = be32(xpr2.data() + 8);
  const std::uint32_t count   = be32(xpr2.data() + 12);
  if (count == 0) return std::nullopt;

  const std::size_t tableOff = 16;
  if (tableOff + std::size_t(count) * 16 > xpr2.size()) return std::nullopt;

  Xpr2Info info;
  info.texture_count = count;

  std::uint32_t tx2d_off_struct = 0;
  std::uint32_t tx2d_off_name   = 0;
  for (std::uint32_t i = 0; i < count; ++i) {
    const std::uint8_t* e = xpr2.data() + tableOff + std::size_t(i) * 16;
    if (be32(e + 0) == 0x54583244u /* TX2D */) {
      tx2d_off_struct = be32(e + 4);
      tx2d_off_name   = be32(e + 12);
      break;
    }
  }
  if (!tx2d_off_struct) return std::nullopt;

  // Name (best-effort)
  if (tx2d_off_name) {
    const std::size_t nameOff = std::size_t(tx2d_off_name) + 12;
    if (nameOff < xpr2.size()) {
      std::size_t end = nameOff;
      while (end < xpr2.size() && xpr2[end] != 0) ++end;
      info.first_name = std::string(
          reinterpret_cast<const char*>(xpr2.data() + nameOff), end - nameOff);
    }
  }

  // TX2D metadata (mirrors rebuild_xpr2_dds_first layout)
  const std::size_t meta = std::size_t(tx2d_off_struct) + 12 + 33;
  if (meta + 7 > xpr2.size()) return std::nullopt;

  const std::uint16_t dataOffBlocks = be16(xpr2.data() + meta + 0);
  const std::uint16_t hBlocks       = be16(xpr2.data() + meta + 3);
  const std::uint16_t wPacked       = be16(xpr2.data() + meta + 5);
  info.height   = (std::uint32_t(hBlocks) + 1u) * 8u;
  info.width    = (std::uint32_t(wPacked) + 1u) & 0x1FFFu;
  info.fmt_code = xpr2[meta + 2];

  // Walk the mip chain to compute the actual stored mip count.
  // Uses the same logic as rebuild_xpr2_dds_first (break on truncation,
  // don't fail hard) so both functions agree on mip_count.
  const auto finfo = fmt_from_xpr2(info.fmt_code);
  if (finfo && header2 >= 0x100 && info.width > 0 && info.height > 0) {
    const std::uint32_t dataBase = header1 + 12;
    const std::uint32_t totalBlocks = header2 / 0x100;
    if (dataBase < xpr2.size() && dataOffBlocks < totalBlocks) {
      const std::size_t dataOff = std::size_t(dataBase) + std::size_t(dataOffBlocks) * 0x100;
      std::uint32_t mWB = (info.width  + 3u) / 4u;
      std::uint32_t mHB = (info.height + 3u) / 4u;
      std::size_t mipOff = dataOff;
      std::uint32_t mipCount = 0;
      while (mWB > 0 && mHB > 0) {
        const std::uint32_t alignedW = (mWB + 31u) & ~31u;
        const std::size_t tiledBytes = std::size_t(alignedW) * std::size_t(mHB) *
                                       std::size_t(finfo->bytes_per_block);
        if (mipOff + tiledBytes > xpr2.size()) break;
        ++mipCount;
        if (mWB == 1u && mHB == 1u) break;
        mipOff += tiledBytes;
        mWB = std::max(1u, mWB >> 1u);
        mHB = std::max(1u, mHB >> 1u);
      }
      if (mipCount > 0) info.mip_count = mipCount;
    }
  }

  return info;
}

std::optional<std::vector<std::uint8_t>> dds_to_xpr2_patch(
    std::span<const std::uint8_t> original_xpr2,
    std::span<const std::uint8_t> new_dds,
    std::string* err)
{
  auto fail = [&](const std::string& msg) -> std::optional<std::vector<std::uint8_t>> {
    if (err) *err = msg;
    return std::nullopt;
  };

  // ── Parse original XPR2 (mirrors rebuild_xpr2_dds_first) ──────────────────
  if (original_xpr2.size() < 0x20) return fail("XPR2 too small");
  if (std::memcmp(original_xpr2.data(), "XPR2", 4) != 0) return fail("Not an XPR2 file");

  const std::uint32_t header1 = be32(original_xpr2.data() + 4);
  const std::uint32_t header2 = be32(original_xpr2.data() + 8);
  const std::uint32_t count   = be32(original_xpr2.data() + 12);
  if (count == 0) return fail("XPR2 has no entries");

  const std::size_t tableOff = 16;
  if (tableOff + std::size_t(count) * 16 > original_xpr2.size())
    return fail("XPR2 resource table truncated");

  const std::uint32_t dataBase = header1 + 12;
  if (dataBase >= original_xpr2.size()) return fail("XPR2 data base out of range");

  std::uint32_t tx2d_off_struct = 0;
  for (std::uint32_t i = 0; i < count; ++i) {
    const std::uint8_t* e = original_xpr2.data() + tableOff + std::size_t(i) * 16;
    if (be32(e + 0) == 0x54583244u /* TX2D */) {
      tx2d_off_struct = be32(e + 4);
      break;
    }
  }
  if (!tx2d_off_struct) return fail("No TX2D entry found in XPR2");

  const std::size_t meta = std::size_t(tx2d_off_struct) + 12 + 33;
  if (meta + 7 > original_xpr2.size()) return fail("TX2D metadata out of range");

  const std::uint16_t dataOffBlocks = be16(original_xpr2.data() + meta + 0);
  const std::uint8_t  fmt           = original_xpr2[meta + 2];
  const std::uint16_t hBlocks       = be16(original_xpr2.data() + meta + 3);
  const std::uint16_t wPacked       = be16(original_xpr2.data() + meta + 5);

  const std::uint32_t orig_height = (std::uint32_t(hBlocks) + 1u) * 8u;
  const std::uint32_t orig_width  = (std::uint32_t(wPacked) + 1u) & 0x1FFFu;
  if (!orig_width || !orig_height) return fail("XPR2 has invalid dimensions");

  const auto finfo = fmt_from_xpr2(fmt);
  if (!finfo) return fail("Unsupported XPR2 format code 0x" + [fmt](){
      char buf[8]; std::snprintf(buf, sizeof(buf), "%02X", fmt); return std::string(buf); }());

  if (header2 < 0x100) return fail("XPR2 data length field too small");
  const std::uint32_t totalBlocks = header2 / 0x100;
  if (dataOffBlocks >= totalBlocks) return fail("XPR2 data block offset out of range");

  const std::size_t dataOff = std::size_t(dataBase) + std::size_t(dataOffBlocks) * 0x100;
  if (dataOff >= original_xpr2.size()) return fail("XPR2 texture data starts outside file");

  // Geometry + stored mip layout contract.
  auto align_up_u32 = [](std::uint32_t v, std::uint32_t a) { return (v + (a - 1u)) & ~(a - 1u); };
  const std::uint32_t widthBlocks  = (orig_width  + 3u) / 4u;
  const std::uint32_t heightBlocks = (orig_height + 3u) / 4u;
  if (widthBlocks == 0 || heightBlocks == 0) return fail("XPR2 has invalid block geometry");

  struct MipLayout {
    std::uint32_t level = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t widthBlocks = 0;
    std::uint32_t heightBlocks = 0;
    std::size_t linearBytes = 0;
    std::size_t tiledBytes = 0;
    std::size_t paddedBytes = 0;
    std::size_t offset = 0;
  };

  std::vector<MipLayout> layouts;
  {
    std::size_t mipOff = dataOff;
    std::uint32_t mW = orig_width;
    std::uint32_t mH = orig_height;
    std::uint32_t mWB = widthBlocks;
    std::uint32_t mHB = heightBlocks;
    std::uint32_t level = 0;
    while (mWB > 0 && mHB > 0) {
      const std::size_t linearBytes = std::size_t(mWB) * std::size_t(mHB) * std::size_t(finfo->bytes_per_block);
      const std::uint32_t pitchBlocks = align_up_u32(mWB, 32u);
      const std::uint32_t tileHBlocks = (finfo->bytes_per_block == 8u) ? 16u : 8u;
      const std::uint32_t paddedHeightBlocks = align_up_u32(mHB, tileHBlocks);
      const std::size_t paddedBytes = std::size_t(pitchBlocks) * std::size_t(paddedHeightBlocks) * std::size_t(finfo->bytes_per_block);
      const std::uint32_t alignedWidthBlocks = (mWB + 31u) & ~31u;
      const std::size_t tiledBytes = std::size_t(alignedWidthBlocks) * std::size_t(mHB) * std::size_t(finfo->bytes_per_block);
      if (mipOff + tiledBytes > original_xpr2.size()) {
        return fail("XPR2 mip layout is truncated at level " + std::to_string(level));
      }
      layouts.push_back(MipLayout{level, mW, mH, mWB, mHB, linearBytes, tiledBytes, paddedBytes, mipOff});
      if (mWB == 1u && mHB == 1u) break;
      mipOff += tiledBytes;
      ++level;
      mW = std::max(1u, mW >> 1u);
      mH = std::max(1u, mH >> 1u);
      mWB = std::max(1u, mWB >> 1u);
      mHB = std::max(1u, mHB >> 1u);
    }
  }
  if (layouts.empty()) return fail("XPR2 has no writable mip layout");
  const std::uint32_t originalMipCount = static_cast<std::uint32_t>(layouts.size());

  // ── Validate new DDS ───────────────────────────────────────────────────────
  if (new_dds.size() < 128) return fail("Replacement DDS is too small");
  if (std::memcmp(new_dds.data(), "DDS ", 4) != 0) return fail("Replacement is not a DDS file");

  const std::uint32_t dds_height = le32(new_dds.data() + 12);
  const std::uint32_t dds_width  = le32(new_dds.data() + 16);
  std::uint32_t dds_mip_count    = le32(new_dds.data() + 28);
  const std::uint32_t dds_fourcc = le32(new_dds.data() + 84);
  if (dds_mip_count == 0) dds_mip_count = 1;

  if (dds_width != orig_width || dds_height != orig_height) {
    return fail("DDS dimensions " + std::to_string(dds_width) + "x" + std::to_string(dds_height) +
                " do not match XPR2 " + std::to_string(orig_width) + "x" + std::to_string(orig_height));
  }

  const bool isDXT1 = (dds_fourcc == 0x31545844u);
  const bool isDXT3 = (dds_fourcc == 0x33545844u);
  const bool isDXT5 = (dds_fourcc == 0x35545844u);
  const bool isATI2 = (dds_fourcc == 0x32495441u) || (dds_fourcc == 0x55354342u) || (dds_fourcc == 0x53354342u);

  const char* ddsName = isDXT1 ? "DXT1" : isDXT3 ? "DXT3" : isDXT5 ? "DXT5" : isATI2 ? "ATI2" : nullptr;
  if (!ddsName)
    return fail("DDS pixel format is not DXT1/DXT3/DXT5/ATI2 — unsupported for XPR2 import");

  const bool formatOk = (isDXT1 && (fmt == 0x52u || fmt == 0x7Cu)) ||
                        (isDXT3 && fmt == 0x53u) ||
                        (isDXT5 && fmt == 0x54u) ||
                        (isATI2 && fmt == 0x71u);
  if (!formatOk)
    return fail(std::string("DDS format ") + ddsName +
                " is not compatible with XPR2 format code 0x" + [fmt](){
                    char buf[8]; std::snprintf(buf, sizeof(buf), "%02X", fmt); return std::string(buf); }());

  if (dds_mip_count != originalMipCount) {
    return fail("DDS mip count " + std::to_string(dds_mip_count) +
                " does not match original XPR2 mip count " + std::to_string(originalMipCount));
  }

  std::size_t requiredDdsBytes = 128;
  for (const auto& mip : layouts) requiredDdsBytes += mip.linearBytes;
  if (new_dds.size() < requiredDdsBytes) {
    return fail("DDS data is truncated for the required mip chain (need " + std::to_string(requiredDdsBytes) +
                " bytes, have " + std::to_string(new_dds.size()) + ")");
  }

  // ── Reverse-transform: linear DDS data → XPR2 encoded data ───────────────
  // Import pipeline (inverse of the decode pipeline used in rebuild_xpr2_dds_first):
  //   Decode: xpr2_raw → swap16 → XGAddress_untile → linear_DXT
  //   Import: linear_DXT → XGAddress_retile → swap16 → xpr2_raw
  //
  // Environment override: ASTRA_XPR2_UNTILE_MODE=linear|morton overrides to
  // those modes; otherwise XGAddress retile is used (matching the default decode path).

  const Xpr2UntileMode forced = xpr2_untile_mode_from_env();
  std::vector<std::uint8_t> result(original_xpr2.begin(), original_xpr2.end());

  std::size_t ddsDataOff = 128;
  for (const auto& mip : layouts) {
    if (ddsDataOff + mip.linearBytes > new_dds.size()) {
      return fail("DDS mip " + std::to_string(mip.level) + " data is truncated");
    }

    std::vector<std::uint8_t> linearData(
        new_dds.begin() + std::ptrdiff_t(ddsDataOff),
        new_dds.begin() + std::ptrdiff_t(ddsDataOff + mip.linearBytes));
    ddsDataOff += mip.linearBytes;

    std::vector<std::uint8_t> encodedData;
    std::size_t writeSize = 0;

    if (forced == Xpr2UntileMode::Linear) {
      encodedData = linearData;
      swap16_inplace(encodedData);
      writeSize = mip.linearBytes;
    } else if (forced == Xpr2UntileMode::Morton) {
      encodedData = tile_360_dxt_padded(linearData, mip.widthBlocks, mip.heightBlocks,
                                        align_up_u32(mip.widthBlocks, 32u),
                                        align_up_u32(mip.heightBlocks, (finfo->bytes_per_block == 8u) ? 16u : 8u),
                                        finfo->bytes_per_block);
      swap16_inplace(encodedData);
      writeSize = mip.paddedBytes;
    } else {
      encodedData = xbox360_xgaddress_retile_dxt(
          linearData, mip.widthBlocks, mip.heightBlocks, finfo->bytes_per_block);
      swap16_inplace(encodedData);
      writeSize = mip.tiledBytes;
    }

    if (encodedData.size() < writeSize)
      return fail("Encoded XPR2 mip " + std::to_string(mip.level) + " is smaller than expected layout");
    if (mip.offset + writeSize > result.size())
      return fail("Encoded XPR2 mip " + std::to_string(mip.level) + " would write past the end of the container");

    // Round-trip sanity check: decode the encoded mip back to linear and verify
    // it matches the input.  This catches tiling formula errors or size mismatches
    // before writing anything bad into the container.
    if (forced != Xpr2UntileMode::Linear && forced != Xpr2UntileMode::Morton) {
      // XGAddress path: encoded = retile(linear) → swap16.
      // Verify: swap16(encoded[:tiledBytes]) → untile == linearData.
      std::vector<std::uint8_t> rtCheck(encodedData.begin(),
                                        encodedData.begin() + std::ptrdiff_t(writeSize));
      swap16_inplace(rtCheck);
      auto rtLinear = xbox360_xgaddress_untile_dxt(rtCheck, mip.widthBlocks,
                                                   mip.heightBlocks,
                                                   finfo->bytes_per_block);
      if (rtLinear.size() != linearData.size() || rtLinear != linearData) {
        return fail("Round-trip verification failed for mip " + std::to_string(mip.level) +
                    " (" + std::to_string(mip.widthBlocks) + "x" + std::to_string(mip.heightBlocks) +
                    " blocks, " + std::to_string(finfo->bytes_per_block) + " bpb): " +
                    "retile→swap16→untile did not reproduce the input DDS data. " +
                    "This indicates a tiling formula error for this texture size.");
      }
    }

    std::memcpy(result.data() + mip.offset, encodedData.data(), writeSize);
  }

  return result;
}

} // namespace gf::textures