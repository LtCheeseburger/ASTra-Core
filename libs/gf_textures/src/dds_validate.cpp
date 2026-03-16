#include "gf/textures/dds_validate.hpp"

#include <algorithm>
#include <cstring>
#include <sstream>

namespace gf::textures {
namespace {

constexpr std::uint32_t DDSD_CAPS        = 0x00000001u;
constexpr std::uint32_t DDSD_HEIGHT      = 0x00000002u;
constexpr std::uint32_t DDSD_WIDTH       = 0x00000004u;
constexpr std::uint32_t DDSD_PITCH       = 0x00000008u;
constexpr std::uint32_t DDSD_PIXELFORMAT = 0x00001000u;
constexpr std::uint32_t DDSD_MIPMAPCOUNT = 0x00020000u;
constexpr std::uint32_t DDSD_LINEARSIZE  = 0x00080000u;
constexpr std::uint32_t DDSD_DEPTH       = 0x00800000u;

constexpr std::uint32_t DDPF_ALPHAPIXELS = 0x00000001u;
constexpr std::uint32_t DDPF_FOURCC      = 0x00000004u;
constexpr std::uint32_t DDPF_RGB         = 0x00000040u;
constexpr std::uint32_t DDPF_LUMINANCE   = 0x00020000u;

constexpr std::uint32_t DDSCAPS_COMPLEX  = 0x00000008u;
constexpr std::uint32_t DDSCAPS_TEXTURE  = 0x00001000u;
constexpr std::uint32_t DDSCAPS_MIPMAP   = 0x00400000u;

constexpr std::uint32_t DDSCAPS2_CUBEMAP = 0x00000200u;
constexpr std::uint32_t DDSCAPS2_VOLUME  = 0x00200000u;

constexpr std::uint32_t DX10_FOURCC = 0x30315844u; // DX10

std::uint32_t rd_u32le(const std::uint8_t* p) {
  return (std::uint32_t)p[0] | ((std::uint32_t)p[1] << 8) | ((std::uint32_t)p[2] << 16) | ((std::uint32_t)p[3] << 24);
}

void add_issue(DdsValidationResult& result, DdsIssueSeverity severity, std::string code, std::string message) {
  result.issues.push_back(DdsIssue{severity, std::move(code), std::move(message)});
}

std::span<const std::uint8_t> unwrap_dds_prefix(std::span<const std::uint8_t> bytes, DdsValidationResult& result) {
  result.sourceSize = bytes.size();
  if (bytes.size() >= 4 && std::memcmp(bytes.data(), "DDS ", 4) == 0) {
    result.hasDdsMagic = true;
    result.kind = DdsKind::Dds;
    result.dataOffset = 0;
    result.visibleSize = bytes.size();
    return bytes;
  }

  const std::size_t scanMax = std::min<std::size_t>(bytes.size(), 0x4000u);
  for (std::size_t off = 1; off + 4 <= scanMax; ++off) {
    if (std::memcmp(bytes.data() + off, "DDS ", 4) == 0) {
      result.hasDdsMagic = true;
      result.wrappedHeader = true;
      result.headerLooksWrapped = true;
      result.kind = DdsKind::WrappedDds;
      result.dataOffset = off;
      result.visibleSize = bytes.size() - off;
      add_issue(result, DdsIssueSeverity::Warning, "wrapped_header",
                "DDS header was found at a non-zero offset; input appears wrapped or prefixed.");
      return bytes.subspan(off);
    }
  }

  result.visibleSize = bytes.size();
  return bytes;
}

std::size_t block_size_for_format(DdsFormat format) {
  switch (format) {
    case DdsFormat::DXT1:
    case DdsFormat::ATI1:
      return 8u;
    case DdsFormat::DXT3:
    case DdsFormat::DXT5:
    case DdsFormat::ATI2:
      return 16u;
    case DdsFormat::RGBA32:
      return 4u;
    default:
      return 0u;
  }
}

std::size_t mip_payload_size(std::uint32_t width, std::uint32_t height, DdsFormat format, bool* supported) {
  const std::uint32_t w = std::max(1u, width);
  const std::uint32_t h = std::max(1u, height);
  *supported = true;
  if (format == DdsFormat::RGBA32) {
    return static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4u;
  }
  const std::size_t blockBytes = block_size_for_format(format);
  if (blockBytes == 0) {
    *supported = false;
    return 0;
  }
  const std::uint32_t bw = std::max(1u, (w + 3u) / 4u);
  const std::uint32_t bh = std::max(1u, (h + 3u) / 4u);
  return static_cast<std::size_t>(bw) * static_cast<std::size_t>(bh) * blockBytes;
}

std::uint32_t fourcc_of(const char (&s)[5]) {
  return (std::uint32_t)s[0] | ((std::uint32_t)s[1] << 8) | ((std::uint32_t)s[2] << 16) | ((std::uint32_t)s[3] << 24);
}

DdsFormat classify_fourcc(std::uint32_t fourcc) {
  if (fourcc == fourcc_of("DXT1")) return DdsFormat::DXT1;
  if (fourcc == fourcc_of("DXT3")) return DdsFormat::DXT3;
  if (fourcc == fourcc_of("DXT5")) return DdsFormat::DXT5;
  if (fourcc == fourcc_of("ATI1") || fourcc == fourcc_of("BC4U") || fourcc == fourcc_of("BC4S")) return DdsFormat::ATI1;
  if (fourcc == fourcc_of("ATI2") || fourcc == fourcc_of("BC5U") || fourcc == fourcc_of("BC5S")) return DdsFormat::ATI2;
  return DdsFormat::Unknown;
}

const char* format_name(DdsFormat format) {
  switch (format) {
    case DdsFormat::DXT1: return "DXT1";
    case DdsFormat::DXT3: return "DXT3";
    case DdsFormat::DXT5: return "DXT5";
    case DdsFormat::ATI1: return "ATI1/BC4";
    case DdsFormat::ATI2: return "ATI2/BC5";
    case DdsFormat::RGBA32: return "RGBA32";
    default: return "Unknown";
  }
}

bool any_error(const DdsValidationResult& result) {
  for (const auto& issue : result.issues) {
    if (issue.severity == DdsIssueSeverity::Error) return true;
  }
  return false;
}

bool supports_rgba_layout(std::uint32_t rgbBitCount,
                          std::uint32_t rmask,
                          std::uint32_t gmask,
                          std::uint32_t bmask,
                          std::uint32_t amask) {
  if (rgbBitCount != 32) return false;
  const bool argb = (rmask == 0x00ff0000u && gmask == 0x0000ff00u && bmask == 0x000000ffu);
  const bool abgr = (rmask == 0x000000ffu && gmask == 0x0000ff00u && bmask == 0x00ff0000u);
  const bool alphaOk = (amask == 0xff000000u || amask == 0u);
  return (argb || abgr) && alphaOk;
}

} // namespace

DdsValidationResult inspect_dds(std::span<const std::uint8_t> bytes) {
  DdsValidationResult result{};
  auto visible = unwrap_dds_prefix(bytes, result);

  if (!result.hasDdsMagic) {
    add_issue(result, DdsIssueSeverity::Error, "missing_magic", "Input does not begin with a DDS header and no wrapped DDS header was found.");
    result.summary = "Not a DDS";
    return result;
  }

  if (visible.size() < 128) {
    add_issue(result, DdsIssueSeverity::Error, "truncated_header", "DDS header is truncated; fewer than 128 bytes are available.");
    result.payloadTruncated = true;
    result.summary = "Truncated DDS header";
    return result;
  }

  const std::uint8_t* hdr = visible.data();
  result.headerSize = rd_u32le(hdr + 4);
  const std::uint32_t flags = rd_u32le(hdr + 8);
  result.height = rd_u32le(hdr + 12);
  result.width = rd_u32le(hdr + 16);
  const std::uint32_t pitchOrLinearSize = rd_u32le(hdr + 20);
  result.depth = rd_u32le(hdr + 24);
  result.mipCount = rd_u32le(hdr + 28);
  result.caps = rd_u32le(hdr + 108);
  result.caps2 = rd_u32le(hdr + 112);

  const std::uint8_t* pf = hdr + 76;
  result.pixelFormatSize = rd_u32le(pf + 0);
  const std::uint32_t pfFlags = rd_u32le(pf + 4);
  result.fourcc = rd_u32le(pf + 8);
  const std::uint32_t rgbBitCount = rd_u32le(pf + 12);
  const std::uint32_t rmask = rd_u32le(pf + 16);
  const std::uint32_t gmask = rd_u32le(pf + 20);
  const std::uint32_t bmask = rd_u32le(pf + 24);
  const std::uint32_t amask = rd_u32le(pf + 28);

  if (result.headerSize != 124u) {
    add_issue(result, DdsIssueSeverity::Error, "bad_header_size", "DDS header size is not 124 bytes.");
  }
  if (result.pixelFormatSize != 32u) {
    add_issue(result, DdsIssueSeverity::Error, "bad_pixel_format_size", "DDS pixel-format size is not 32 bytes.");
  }
  if ((flags & (DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PIXELFORMAT)) !=
      (DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PIXELFORMAT)) {
    add_issue(result, DdsIssueSeverity::Warning, "missing_required_flags", "DDS header flags are missing one or more required bits.");
  }
  if ((result.caps & DDSCAPS_TEXTURE) == 0u) {
    add_issue(result, DdsIssueSeverity::Warning, "missing_texture_caps", "DDSCAPS_TEXTURE is not set.");
  }
  if (result.width == 0 || result.height == 0) {
    add_issue(result, DdsIssueSeverity::Error, "zero_dimensions", "DDS width and height must be non-zero.");
  }
  if ((result.caps2 & DDSCAPS2_VOLUME) != 0u && result.depth == 0u) {
    add_issue(result, DdsIssueSeverity::Error, "volume_missing_depth", "Volume DDS is missing a non-zero depth field.");
  }
  if ((result.caps2 & DDSCAPS2_CUBEMAP) != 0u) {
    add_issue(result, DdsIssueSeverity::Warning, "unsupported_cubemap", "Cubemap DDS detected; validation is limited and decode is unsupported.");
  }

  if (result.mipCount == 0u) {
    result.mipCount = 1u;
  }
  if (result.mipCount > 1u) {
    if ((flags & DDSD_MIPMAPCOUNT) == 0u) {
      add_issue(result, DdsIssueSeverity::Warning, "mip_flag_missing", "Mip count is greater than 1 but DDSD_MIPMAPCOUNT is not set.");
    }
    if ((result.caps & DDSCAPS_MIPMAP) == 0u) {
      add_issue(result, DdsIssueSeverity::Warning, "mip_caps_missing", "Mipmapped DDS is missing DDSCAPS_MIPMAP.");
    }
    if ((result.caps & DDSCAPS_COMPLEX) == 0u) {
      add_issue(result, DdsIssueSeverity::Warning, "complex_caps_missing", "Mipmapped DDS is missing DDSCAPS_COMPLEX.");
    }
  }

  result.format = DdsFormat::Unknown;
  if ((pfFlags & DDPF_FOURCC) != 0u) {
    if (result.fourcc == DX10_FOURCC) {
      result.dx10HeaderPresent = true;
      if (visible.size() < 148u) {
        add_issue(result, DdsIssueSeverity::Error, "missing_dx10_header", "DDS uses DX10 FourCC but the DX10 extension header is missing.");
      } else {
        result.dx10Format = rd_u32le(hdr + 128);
        add_issue(result, DdsIssueSeverity::Warning, "unsupported_dx10", "DX10 DDS header detected; ASTra does not currently decode DX10 DDS variants.");
      }
    } else {
      result.format = classify_fourcc(result.fourcc);
      if (result.format == DdsFormat::Unknown) {
        add_issue(result, DdsIssueSeverity::Warning, "unsupported_fourcc", "DDS FourCC is valid but not currently supported by ASTra.");
      }
    }
  } else if ((pfFlags & DDPF_RGB) != 0u) {
    if (supports_rgba_layout(rgbBitCount, rmask, gmask, bmask, amask)) {
      result.format = DdsFormat::RGBA32;
    } else {
      add_issue(result, DdsIssueSeverity::Warning, "unsupported_rgb_layout", "DDS uses an uncompressed RGB layout that ASTra does not currently support.");
    }
  } else if ((pfFlags & DDPF_LUMINANCE) != 0u) {
    add_issue(result, DdsIssueSeverity::Warning, "unsupported_luminance", "DDS uses a luminance format that ASTra does not currently support.");
  } else {
    add_issue(result, DdsIssueSeverity::Error, "unknown_pixel_format", "DDS pixel format is malformed or unsupported.");
  }

  result.formatName = format_name(result.format);

  std::size_t headerBytes = 128u + (result.dx10HeaderPresent ? 20u : 0u);
  result.payloadSize = (visible.size() >= headerBytes) ? (visible.size() - headerBytes) : 0u;

  bool payloadSupported = false;
  std::size_t expectedPayload = 0u;
  std::uint32_t mipWidth = std::max(1u, result.width);
  std::uint32_t mipHeight = std::max(1u, result.height);
  const std::uint32_t mipLevels = std::max(1u, result.mipCount);
  for (std::uint32_t i = 0; i < mipLevels; ++i) {
    bool mipSupported = false;
    expectedPayload += mip_payload_size(mipWidth, mipHeight, result.format, &mipSupported);
    payloadSupported = payloadSupported || mipSupported;
    mipWidth = std::max(1u, mipWidth >> 1);
    mipHeight = std::max(1u, mipHeight >> 1);
  }
  result.expectedPayloadSizeMin = expectedPayload;
  result.expectedPayloadSizeMax = expectedPayload;

  if (payloadSupported && expectedPayload > 0u) {
    if (result.payloadSize < expectedPayload) {
      result.payloadTruncated = true;
      add_issue(result, DdsIssueSeverity::Error, "truncated_payload", "DDS payload is smaller than the size implied by the header.");
    } else if (result.payloadSize > expectedPayload) {
      result.payloadHasTrailingBytes = true;
      add_issue(result, DdsIssueSeverity::Warning, "trailing_payload", "DDS payload contains trailing bytes beyond the expected mip chain.");
    }

    if ((flags & DDSD_LINEARSIZE) != 0u && result.format != DdsFormat::RGBA32) {
      const std::size_t topMip = mip_payload_size(result.width, result.height, result.format, &payloadSupported);
      if (pitchOrLinearSize != 0u && static_cast<std::size_t>(pitchOrLinearSize) != topMip) {
        add_issue(result, DdsIssueSeverity::Warning, "linear_size_mismatch", "DDS dwPitchOrLinearSize does not match the top mip payload size.");
      }
    }
    if ((flags & DDSD_PITCH) != 0u && result.format == DdsFormat::RGBA32) {
      const std::size_t expectedPitch = static_cast<std::size_t>(std::max(1u, result.width)) * 4u;
      if (pitchOrLinearSize != 0u && static_cast<std::size_t>(pitchOrLinearSize) != expectedPitch) {
        add_issue(result, DdsIssueSeverity::Warning, "pitch_mismatch", "DDS pitch does not match width * 4 for RGBA32 data.");
      }
    }
  }

  if (any_error(result)) {
    result.status = DdsValidationStatus::Invalid;
  } else if (result.format == DdsFormat::Unknown || result.dx10HeaderPresent || (result.caps2 & DDSCAPS2_CUBEMAP) != 0u) {
    result.status = DdsValidationStatus::UnsupportedValid;
  } else {
    result.status = DdsValidationStatus::Valid;
  }

  std::ostringstream oss;
  oss << dds_validation_status_name(result.status) << " | "
      << (result.formatName.empty() ? "Unknown" : result.formatName) << " | "
      << result.width << "x" << result.height << " | mips " << result.mipCount;
  result.summary = oss.str();
  return result;
}

std::optional<DdsInfo> dds_info_from_validation(const DdsValidationResult& result) {
  if (!result.is_supported()) return std::nullopt;
  if (result.width == 0 || result.height == 0) return std::nullopt;
  DdsInfo info{};
  info.format = result.format;
  info.width = result.width;
  info.height = result.height;
  info.mipCount = std::max(1u, result.mipCount);
  return info;
}

const char* dds_issue_severity_name(DdsIssueSeverity severity) noexcept {
  switch (severity) {
    case DdsIssueSeverity::Info: return "info";
    case DdsIssueSeverity::Warning: return "warning";
    case DdsIssueSeverity::Error: return "error";
    default: return "unknown";
  }
}

const char* dds_validation_status_name(DdsValidationStatus status) noexcept {
  switch (status) {
    case DdsValidationStatus::Invalid: return "invalid";
    case DdsValidationStatus::UnsupportedValid: return "unsupported-valid";
    case DdsValidationStatus::Valid: return "valid";
    default: return "unknown";
  }
}

const char* dds_kind_name(DdsKind kind) noexcept {
  switch (kind) {
    case DdsKind::NotDds: return "not-dds";
    case DdsKind::Dds: return "dds";
    case DdsKind::WrappedDds: return "wrapped-dds";
    default: return "unknown";
  }
}

} // namespace gf::textures
