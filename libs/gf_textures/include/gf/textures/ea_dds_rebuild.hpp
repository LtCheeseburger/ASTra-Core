#pragma once
#include "gf/textures/dds_validate.hpp"
#include <cstdint>
#include <vector>
#include <span>
#include <optional>
#include <string>

namespace gf::textures {

enum class EaDxtFormat {
  DXT1,
  DXT5,
  BC4, // ATI1
  BC5, // ATI2
  UNKNOWN
};

struct EaDdsInfo {
  EaDxtFormat format;
  uint32_t width;
  uint32_t height;
  uint32_t mipCount;
};

struct P3rTextureInfo {
  bool parsed = false;
  std::string signature;
  std::uint32_t signatureValue = 0;
  std::uint32_t headerSize = 0;
  EaDxtFormat format = EaDxtFormat::UNKNOWN;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::uint32_t mipCount = 0;
  std::size_t dataOffset = 0;
  std::size_t payloadSize = 0;
  std::uint32_t flags = 0;
  std::uint32_t caps = 0;
  std::uint32_t caps2 = 0;
  std::uint32_t fourcc = 0;
  std::vector<std::string> issues;

  [[nodiscard]] bool ok() const noexcept { return parsed && width > 0 && height > 0 && dataOffset > 0; }
};

struct P3rConversionAttempt {
  std::string stage;
  bool success = false;
  std::string message;
  std::size_t offset = 0;
  DdsValidationResult validation{};
};

struct P3rConversionDiagnostics {
  std::string sourceMagic;
  bool ddsMagicAtZero = false;
  bool ddsMagicWrapped = false;
  bool p3rMagicAtZero = false;
  bool p3rMagicWrapped = false;
  std::size_t ddsMagicOffset = 0;
  std::size_t p3rMagicOffset = 0;
  bool payloadLooksDdsLike = false;
  bool payloadLooksBcLike = false;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::uint32_t mipCount = 0;
  std::string parsedSignature;
  std::string parsedFormatName;
  std::uint32_t parsedHeaderSize = 0;
  std::size_t parsedDataOffset = 0;
  std::size_t parsedPayloadSize = 0;
  bool parsedHeader = false;
  std::vector<std::string> parseIssues;
  std::string successStage;
  std::vector<P3rConversionAttempt> attempts;
};

struct PreparedDdsExport {
  std::vector<std::uint8_t> ddsBytes;
  DdsValidationResult validation{};
  P3rConversionDiagnostics p3r;
  std::string error;

  [[nodiscard]] bool ok() const noexcept { return !ddsBytes.empty() && validation.is_valid(); }
};


PreparedDdsExport prepare_texture_dds_for_export(std::span<const std::uint8_t> bytes,
                                                 bool sourceIsP3r,
                                                 std::uint32_t astFlags = 0);

std::optional<P3rTextureInfo> parse_p3r_texture_info(std::span<const std::uint8_t> payload,
                                                     std::uint32_t astFlags = 0);

// Rebuilds a *standard* DDS blob from an EA-wrapped DDS payload
// `payload` must be the decompressed AST entry bytes
std::optional<std::vector<uint8_t>>
rebuild_ea_dds(std::span<const uint8_t> payload, EaDdsInfo* outInfo);

// Maps AST entry flags -> expected EA DXT format
EaDxtFormat map_ast_flags_to_format(uint32_t astFlags);

// Convenience overload used by the GUI preview path.
//
// EASE determines the intended DXT format primarily from the AST entry flags.
// For preview, we keep this logic here so callers have a single entry point:
//   AST payload -> rebuild DDS -> decode.
//
// Note: `rebuild_ea_dds(payload, outInfo)` will still validate/override fields
// based on the EA header when present; this overload just seeds the format.
std::optional<std::vector<uint8_t>>
rebuild_ea_dds(std::span<const uint8_t> payload, uint32_t astFlags, EaDdsInfo* outInfo);

}
