#include "gf/textures/ea_dds_rebuild.hpp"
#include "gf/textures/dds_validate.hpp"
#include <algorithm>
#include <cstring>
#include <sstream>

namespace gf::textures {
namespace {

static uint32_t rd_u32(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

bool starts_with_dds(std::span<const uint8_t> bytes) {
  return bytes.size() >= 4 && std::memcmp(bytes.data(), "DDS ", 4) == 0;
}

bool starts_with_legacy_p3r(std::span<const uint8_t> bytes) {
  return bytes.size() >= 4 &&
         (std::memcmp(bytes.data(), "p3R", 3) == 0 || std::memcmp(bytes.data(), "P3R", 3) == 0) &&
         (bytes[3] == 0x02 || bytes[3] == 0x00);
}

bool looks_like_structured_p3r_tag(std::span<const uint8_t> bytes) {
  if (bytes.size() < 128) return false;
  if (starts_with_dds(bytes) || starts_with_legacy_p3r(bytes)) return false;

  const std::uint32_t headerSize = rd_u32(bytes.data() + 4);
  const std::uint32_t flags = rd_u32(bytes.data() + 8);
  const std::uint32_t height = rd_u32(bytes.data() + 12);
  const std::uint32_t width = rd_u32(bytes.data() + 16);
  const std::uint32_t pfSize = rd_u32(bytes.data() + 76);
  const std::uint32_t caps = rd_u32(bytes.data() + 108);

  const bool tagLooksP3rish = bytes[1] == '3' && bytes[2] == 'R' && (bytes[3] == 0x02 || bytes[3] == 0x00);
  const bool ddsLikeHeader = headerSize == 124 && pfSize == 32 && width > 0 && height > 0 &&
                             (flags & 0x1000u) != 0 && (caps & 0x1000u) != 0;
  return tagLooksP3rish && ddsLikeHeader;
}

bool starts_with_any_p3r(std::span<const uint8_t> bytes) {
  return starts_with_legacy_p3r(bytes) || looks_like_structured_p3r_tag(bytes);
}

std::size_t find_magic_offset(std::span<const uint8_t> bytes, const char* magic, std::size_t magicSize) {
  const std::size_t scanMax = std::min<std::size_t>(bytes.size(), 0x4000u);
  for (std::size_t off = 0; off + magicSize <= scanMax; ++off) {
    if (std::memcmp(bytes.data() + off, magic, magicSize) == 0) return off;
  }
  return bytes.size();
}

std::size_t find_p3r_offset(std::span<const uint8_t> bytes) {
  const std::size_t scanMax = std::min<std::size_t>(bytes.size(), 0x4000u);
  for (std::size_t off = 0; off + 128 <= scanMax; ++off) {
    const auto sub = bytes.subspan(off);
    if (starts_with_any_p3r(sub)) return off;
  }
  return bytes.size();
}

std::string sniff_magic(std::span<const uint8_t> bytes) {
  if (bytes.size() < 4) return "<short>";
  std::ostringstream oss;
  oss.setf(std::ios::hex, std::ios::basefield);
  oss.setf(std::ios::uppercase);
  for (int i = 0; i < 4; ++i) {
    if (i) oss << ' ';
    oss.width(2);
    oss.fill('0');
    oss << static_cast<unsigned>(bytes[i]);
  }
  return oss.str();
}

std::string format_name(EaDxtFormat fmt) {
  switch (fmt) {
    case EaDxtFormat::DXT1: return "DXT1";
    case EaDxtFormat::DXT5: return "DXT5";
    case EaDxtFormat::BC4: return "ATI1/BC4";
    case EaDxtFormat::BC5: return "ATI2/BC5";
    default: return "UNKNOWN";
  }
}

bool looks_bc_payload(std::span<const uint8_t> bytes) {
  if (bytes.size() < 0x58) return false;
  const std::size_t off = starts_with_dds(bytes) ? 0 : find_magic_offset(bytes, "DDS ", 4);
  if (off < bytes.size() && off + 0x58 <= bytes.size()) {
    const uint8_t* p = bytes.data() + off;
    const std::uint32_t fourcc = rd_u32(p + 84);
    return fourcc == 0x31545844u || fourcc == 0x35545844u || fourcc == 0x31495441u || fourcc == 0x32495441u;
  }
  const auto info = parse_p3r_texture_info(bytes);
  return info.has_value() && info->format != EaDxtFormat::UNKNOWN;
}

std::vector<uint8_t> swap_p3r_magic(std::span<const uint8_t> bytes) {
  std::vector<uint8_t> out(bytes.begin(), bytes.end());
  if (out.size() >= 4) {
    out[0] = 'D';
    out[1] = 'D';
    out[2] = 'S';
    out[3] = ' ';
  }
  return out;
}

std::size_t block_bytes(EaDxtFormat fmt) {
  switch (fmt) {
    case EaDxtFormat::DXT1:
    case EaDxtFormat::BC4: return 8u;
    case EaDxtFormat::DXT5:
    case EaDxtFormat::BC5: return 16u;
    default: return 0u;
  }
}

std::uint32_t fourcc_for_format(EaDxtFormat fmt) {
  switch (fmt) {
    case EaDxtFormat::DXT1: return 0x31545844u;
    case EaDxtFormat::DXT5: return 0x35545844u;
    case EaDxtFormat::BC4: return 0x31495441u;
    case EaDxtFormat::BC5: return 0x32495441u;
    default: return 0u;
  }
}

std::size_t expected_payload_size(std::uint32_t width, std::uint32_t height, std::uint32_t mipCount, EaDxtFormat fmt) {
  const std::size_t bpb = block_bytes(fmt);
  if (bpb == 0) return 0;
  std::size_t total = 0;
  std::uint32_t w = std::max(1u, width);
  std::uint32_t h = std::max(1u, height);
  const std::uint32_t mips = std::max(1u, mipCount);
  for (std::uint32_t i = 0; i < mips; ++i) {
    const std::size_t bw = std::max<std::size_t>(1u, (w + 3u) / 4u);
    const std::size_t bh = std::max<std::size_t>(1u, (h + 3u) / 4u);
    total += bw * bh * bpb;
    w = std::max(1u, w >> 1);
    h = std::max(1u, h >> 1);
  }
  return total;
}

std::vector<std::uint8_t> build_dds_header(std::uint32_t width,
                                           std::uint32_t height,
                                           EaDxtFormat fmt,
                                           std::uint32_t mipCount,
                                           std::uint32_t linearSize) {
  std::vector<std::uint8_t> dds(128, 0);
  dds[0] = 'D'; dds[1] = 'D'; dds[2] = 'S'; dds[3] = ' ';
  auto w32 = [&](std::size_t off, std::uint32_t v) {
    dds[off + 0] = static_cast<std::uint8_t>(v & 0xFFu);
    dds[off + 1] = static_cast<std::uint8_t>((v >> 8) & 0xFFu);
    dds[off + 2] = static_cast<std::uint8_t>((v >> 16) & 0xFFu);
    dds[off + 3] = static_cast<std::uint8_t>((v >> 24) & 0xFFu);
  };
  w32(4, 124u);
  w32(8, 0x00001007u | (mipCount > 1u ? 0x00020000u : 0u) | 0x00080000u);
  w32(12, height);
  w32(16, width);
  w32(20, linearSize);
  w32(28, std::max(1u, mipCount));
  w32(76, 32u);
  w32(80, 0x00000004u);
  w32(84, fourcc_for_format(fmt));
  w32(108, 0x00001000u | (mipCount > 1u ? 0x00400008u : 0u));
  return dds;
}

void add_attempt(P3rConversionDiagnostics& diag,
                 std::string stage,
                 bool success,
                 std::string message,
                 std::size_t offset,
                 const DdsValidationResult* validation = nullptr) {
  P3rConversionAttempt attempt{};
  attempt.stage = std::move(stage);
  attempt.success = success;
  attempt.message = std::move(message);
  attempt.offset = offset;
  if (validation) attempt.validation = *validation;
  diag.attempts.push_back(std::move(attempt));
}

void adopt_validation(P3rConversionDiagnostics& diag, const DdsValidationResult& v, const std::string& stage) {
  diag.width = v.width;
  diag.height = v.height;
  diag.mipCount = v.mipCount;
  diag.successStage = stage;
}

void adopt_parse(P3rConversionDiagnostics& diag, const P3rTextureInfo& info) {
  diag.parsedHeader = info.parsed;
  diag.parsedSignature = info.signature;
  diag.parsedFormatName = format_name(info.format);
  diag.parsedHeaderSize = info.headerSize;
  diag.parsedDataOffset = info.dataOffset;
  diag.parsedPayloadSize = info.payloadSize;
  diag.parseIssues = info.issues;
  if (info.width) diag.width = info.width;
  if (info.height) diag.height = info.height;
  if (info.mipCount) diag.mipCount = info.mipCount;
}

} // namespace

EaDxtFormat map_ast_flags_to_format(uint32_t flags) {
  switch (flags & 0x3F) {
    case 0x01: return EaDxtFormat::DXT1;
    case 0x11: return EaDxtFormat::DXT5;
    case 0x21: return EaDxtFormat::BC4;
    case 0x31: return EaDxtFormat::BC5;
    default:   return EaDxtFormat::UNKNOWN;
  }
}

std::optional<P3rTextureInfo> parse_p3r_texture_info(std::span<const uint8_t> payload,
                                                     std::uint32_t astFlags) {
  P3rTextureInfo info{};
  if (payload.size() < 128) {
    info.issues.push_back("P3R header is smaller than 128 bytes.");
    return std::nullopt;
  }

  info.signatureValue = rd_u32(payload.data());
  info.signature = sniff_magic(payload.first(4));
  info.headerSize = rd_u32(payload.data() + 4);
  info.flags = rd_u32(payload.data() + 8);
  info.height = rd_u32(payload.data() + 12);
  info.width = rd_u32(payload.data() + 16);
  info.mipCount = rd_u32(payload.data() + 28);
  info.fourcc = rd_u32(payload.data() + 84);
  info.caps = rd_u32(payload.data() + 108);
  info.caps2 = rd_u32(payload.data() + 112);
  const std::uint32_t pfSize = rd_u32(payload.data() + 76);

  if (info.headerSize != 124) info.issues.push_back("P3R header size is not 124 bytes.");
  if (pfSize != 32) info.issues.push_back("P3R pixel format size is not 32 bytes.");
  if (info.width == 0 || info.height == 0) info.issues.push_back("Parsed P3R dimensions are invalid.");
  if (info.mipCount == 0) info.mipCount = 1;

  if (info.fourcc == 0x31545844u) info.format = EaDxtFormat::DXT1;
  else if (info.fourcc == 0x35545844u) info.format = EaDxtFormat::DXT5;
  else if (info.fourcc == 0x31495441u) info.format = EaDxtFormat::BC4;
  else if (info.fourcc == 0x32495441u) info.format = EaDxtFormat::BC5;
  else info.format = map_ast_flags_to_format(astFlags);

  if (info.format == EaDxtFormat::UNKNOWN) {
    info.issues.push_back("Could not map parsed P3R format to a supported DDS FourCC.");
  }

  const bool legacyMagic = starts_with_legacy_p3r(payload);
  const bool structuredMagic = looks_like_structured_p3r_tag(payload);
  if (!legacyMagic && !structuredMagic) {
    info.issues.push_back("Source does not match a recognized P3R header signature.");
    return std::nullopt;
  }

  info.dataOffset = 128u;
  info.payloadSize = payload.size() - info.dataOffset;
  const std::size_t expected = expected_payload_size(info.width, info.height, info.mipCount, info.format);
  if (expected == 0) {
    info.issues.push_back("Parsed P3R format is unsupported for DDS rebuild.");
    return std::nullopt;
  }
  if (info.payloadSize < expected) {
    info.issues.push_back("P3R payload is smaller than the parsed mip-chain requires.");
    return std::nullopt;
  }

  info.parsed = true;
  return info;
}

std::optional<std::vector<uint8_t>>
rebuild_ea_dds(std::span<const uint8_t> payload, EaDdsInfo* outInfo) {
  if (payload.size() < 128) return std::nullopt;

  if (starts_with_dds(payload)) {
    const auto validation = inspect_dds(payload);
    if (!validation.is_valid()) return std::nullopt;
    if (outInfo) {
      outInfo->width = validation.width;
      outInfo->height = validation.height;
      outInfo->mipCount = validation.mipCount;
      switch (validation.format) {
        case DdsFormat::DXT1: outInfo->format = EaDxtFormat::DXT1; break;
        case DdsFormat::DXT5: outInfo->format = EaDxtFormat::DXT5; break;
        case DdsFormat::ATI1: outInfo->format = EaDxtFormat::BC4; break;
        case DdsFormat::ATI2: outInfo->format = EaDxtFormat::BC5; break;
        default: outInfo->format = EaDxtFormat::UNKNOWN; break;
      }
    }
    return std::vector<uint8_t>(payload.begin(), payload.end());
  }

  if (starts_with_legacy_p3r(payload)) {
    auto out = swap_p3r_magic(payload);
    const auto validation = inspect_dds(out);
    if (validation.is_valid()) {
      if (outInfo) {
        outInfo->width = validation.width;
        outInfo->height = validation.height;
        outInfo->mipCount = validation.mipCount;
        switch (validation.format) {
          case DdsFormat::DXT1: outInfo->format = EaDxtFormat::DXT1; break;
          case DdsFormat::DXT5: outInfo->format = EaDxtFormat::DXT5; break;
          case DdsFormat::ATI1: outInfo->format = EaDxtFormat::BC4; break;
          case DdsFormat::ATI2: outInfo->format = EaDxtFormat::BC5; break;
          default: outInfo->format = EaDxtFormat::UNKNOWN; break;
        }
      }
      return out;
    }
  }

  const auto parsed = parse_p3r_texture_info(payload);
  if (!parsed.has_value()) return std::nullopt;

  std::vector<std::uint8_t> rebuilt = build_dds_header(parsed->width, parsed->height, parsed->format,
                                                       parsed->mipCount,
                                                       static_cast<std::uint32_t>(expected_payload_size(parsed->width, parsed->height, parsed->mipCount, parsed->format)));
  rebuilt.insert(rebuilt.end(), payload.begin() + static_cast<std::ptrdiff_t>(parsed->dataOffset), payload.end());

  if (outInfo) {
    outInfo->format = parsed->format;
    outInfo->width = parsed->width;
    outInfo->height = parsed->height;
    outInfo->mipCount = parsed->mipCount;
  }

  const auto validation = inspect_dds(rebuilt);
  if (!validation.is_supported()) return std::nullopt;
  return rebuilt;
}

static void write_fourcc(std::vector<uint8_t>& dds, std::uint32_t fourcc) {
  if (dds.size() < 0x58) return;
  dds[0x54] = static_cast<uint8_t>(fourcc & 0xFFu);
  dds[0x55] = static_cast<uint8_t>((fourcc >> 8) & 0xFFu);
  dds[0x56] = static_cast<uint8_t>((fourcc >> 16) & 0xFFu);
  dds[0x57] = static_cast<uint8_t>((fourcc >> 24) & 0xFFu);
}

std::optional<std::vector<uint8_t>> rebuild_ea_dds(std::span<const uint8_t> payload, std::uint32_t astFlags, EaDdsInfo* outInfo) {
  EaDdsInfo tmp{};
  EaDdsInfo* infoPtr = outInfo ? outInfo : &tmp;

  auto ddsOpt = rebuild_ea_dds(payload, infoPtr);
  if (!ddsOpt.has_value()) {
    const auto parsed = parse_p3r_texture_info(payload, astFlags);
    if (!parsed.has_value()) return std::nullopt;
    infoPtr->format = parsed->format;
    infoPtr->width = parsed->width;
    infoPtr->height = parsed->height;
    infoPtr->mipCount = parsed->mipCount;
    std::vector<std::uint8_t> dds = build_dds_header(parsed->width, parsed->height, parsed->format,
                                                     parsed->mipCount,
                                                     static_cast<std::uint32_t>(expected_payload_size(parsed->width, parsed->height, parsed->mipCount, parsed->format)));
    dds.insert(dds.end(), payload.begin() + static_cast<std::ptrdiff_t>(parsed->dataOffset), payload.end());
    const auto validation = inspect_dds(dds);
    if (!validation.is_supported()) return std::nullopt;
    return dds;
  }

  const EaDxtFormat expected = map_ast_flags_to_format(astFlags);
  if (infoPtr->format == EaDxtFormat::UNKNOWN && expected != EaDxtFormat::UNKNOWN) {
    std::vector<uint8_t>& dds = *ddsOpt;
    switch (expected) {
      case EaDxtFormat::DXT1: write_fourcc(dds, 0x31545844u); break;
      case EaDxtFormat::DXT5: write_fourcc(dds, 0x35545844u); break;
      case EaDxtFormat::BC4:  write_fourcc(dds, 0x31495441u); break;
      case EaDxtFormat::BC5:  write_fourcc(dds, 0x32495441u); break;
      default: break;
    }
    infoPtr->format = expected;
    const auto validation = inspect_dds(dds);
    if (!validation.is_supported()) return std::nullopt;
  }

  return ddsOpt;
}

PreparedDdsExport prepare_texture_dds_for_export(std::span<const std::uint8_t> bytes,
                                                 bool sourceIsP3r,
                                                 std::uint32_t astFlags) {
  PreparedDdsExport out{};
  out.p3r.sourceMagic = sniff_magic(bytes);
  out.p3r.payloadLooksDdsLike = starts_with_dds(bytes) || find_magic_offset(bytes, "DDS ", 4) < bytes.size() || starts_with_any_p3r(bytes) || find_p3r_offset(bytes) < bytes.size();
  out.p3r.payloadLooksBcLike = looks_bc_payload(bytes);

  const std::size_t ddsOff = find_magic_offset(bytes, "DDS ", 4);
  out.p3r.ddsMagicAtZero = (ddsOff == 0 && bytes.size() >= 4);
  out.p3r.ddsMagicWrapped = (ddsOff > 0 && ddsOff < bytes.size());
  out.p3r.ddsMagicOffset = (ddsOff < bytes.size()) ? ddsOff : 0;

  const std::size_t p3rOff = find_p3r_offset(bytes);
  out.p3r.p3rMagicAtZero = (p3rOff == 0 && bytes.size() >= 4);
  out.p3r.p3rMagicWrapped = (p3rOff > 0 && p3rOff < bytes.size());
  out.p3r.p3rMagicOffset = (p3rOff < bytes.size()) ? p3rOff : 0;

  if (!sourceIsP3r) {
    out.validation = inspect_dds(bytes);
    if (out.validation.is_valid()) {
      out.ddsBytes.assign(bytes.begin(), bytes.end());
    } else {
      out.error = "DDS export failed: input DDS is invalid.";
    }
    return out;
  }

  if (starts_with_legacy_p3r(bytes)) {
    auto candidate = swap_p3r_magic(bytes);
    const auto validation = inspect_dds(std::span<const uint8_t>(candidate.data(), candidate.size()));
    if (validation.is_valid()) {
      out.ddsBytes = std::move(candidate);
      out.validation = validation;
      adopt_validation(out.p3r, validation, "direct magic/header swap");
      add_attempt(out.p3r, "direct magic/header swap", true, "P3R header was swapped to DDS magic and validated.", 0, &validation);
      return out;
    }
    add_attempt(out.p3r, "direct magic/header swap", false, "P3R -> DDS failed: direct magic-swap did not yield valid DDS.", 0, &validation);
  } else {
    add_attempt(out.p3r, "direct magic/header swap", false, "P3R -> DDS failed: source does not start with a direct legacy P3R header.", 0, nullptr);
  }

  if (ddsOff < bytes.size() && ddsOff > 0) {
    std::vector<uint8_t> candidate(bytes.begin() + static_cast<std::ptrdiff_t>(ddsOff), bytes.end());
    const auto validation = inspect_dds(std::span<const uint8_t>(candidate.data(), candidate.size()));
    if (validation.is_valid()) {
      out.ddsBytes = std::move(candidate);
      out.validation = validation;
      adopt_validation(out.p3r, validation, "wrapped/offset DDS recovery");
      add_attempt(out.p3r, "wrapped/offset DDS recovery", true, "Recovered DDS header at a non-zero offset.", ddsOff, &validation);
      return out;
    }
    add_attempt(out.p3r, "wrapped/offset DDS recovery", false, "P3R -> DDS failed: recovered wrapped DDS header, but the recovered DDS is invalid.", ddsOff, &validation);
  }

  if (p3rOff < bytes.size() && p3rOff > 0) {
    auto sub = bytes.subspan(p3rOff);
    std::optional<std::vector<std::uint8_t>> candidate;
    if (starts_with_legacy_p3r(sub)) candidate = swap_p3r_magic(sub);
    else if (auto parsed = parse_p3r_texture_info(sub, astFlags); parsed.has_value()) {
      adopt_parse(out.p3r, *parsed);
      candidate = rebuild_ea_dds(sub, astFlags, nullptr);
    }
    if (candidate.has_value()) {
      const auto validation = inspect_dds(std::span<const uint8_t>(candidate->data(), candidate->size()));
      if (validation.is_valid()) {
        out.ddsBytes = std::move(*candidate);
        out.validation = validation;
        adopt_validation(out.p3r, validation, "wrapped/offset DDS recovery");
        add_attempt(out.p3r, "wrapped/offset DDS recovery", true, "Recovered P3R header at a non-zero offset and converted it to DDS.", p3rOff, &validation);
        return out;
      }
      add_attempt(out.p3r, "wrapped/offset DDS recovery", false, "P3R -> DDS failed: wrapped P3R header was found, but converting that region still did not yield valid DDS.", p3rOff, &validation);
    } else {
      add_attempt(out.p3r, "wrapped/offset DDS recovery", false, "P3R -> DDS failed: wrapped P3R header was found, but parsing/rebuild failed.", p3rOff, nullptr);
    }
  } else if (!(ddsOff < bytes.size() && ddsOff > 0)) {
    add_attempt(out.p3r, "wrapped/offset DDS recovery", false, "P3R -> DDS failed: no wrapped DDS or wrapped P3R header was found.", 0, nullptr);
  }

  const auto parsed = parse_p3r_texture_info(bytes, astFlags);
  if (parsed.has_value()) {
    adopt_parse(out.p3r, *parsed);
    EaDdsInfo info{};
    if (auto rebuilt = rebuild_ea_dds(bytes, astFlags, &info); rebuilt.has_value()) {
      const auto validation = inspect_dds(std::span<const uint8_t>(rebuilt->data(), rebuilt->size()));
      if (validation.is_valid()) {
        out.ddsBytes = std::move(*rebuilt);
        out.validation = validation;
        adopt_validation(out.p3r, validation, "parsed P3R header -> DDS rebuild");
        add_attempt(out.p3r, "parsed P3R header -> DDS rebuild", true,
                    "Parsed P3R header fields and rebuilt a canonical DDS header from them.", 0, &validation);
        return out;
      }
      add_attempt(out.p3r, "parsed P3R header -> DDS rebuild", false,
                  "P3R -> DDS failed: rebuilt DDS did not validate.", 0, &validation);
    } else {
      add_attempt(out.p3r, "parsed P3R header -> DDS rebuild", false,
                  "P3R -> DDS failed: parsed header fields were found, but DDS rebuild failed.", 0, nullptr);
    }
  } else {
    add_attempt(out.p3r, "parsed P3R header -> DDS rebuild", false,
                "P3R -> DDS failed: could not parse P3R header fields.", 0, nullptr);
  }

  out.validation = inspect_dds(std::span<const uint8_t>());
  out.error = "P3R -> DDS conversion failed: no conversion stage produced a valid DDS buffer.";
  return out;
}

} // namespace gf::textures
