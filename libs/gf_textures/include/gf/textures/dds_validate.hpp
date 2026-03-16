#pragma once

#include "gf/textures/dds_decode.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace gf::textures {

enum class DdsIssueSeverity {
  Info = 0,
  Warning,
  Error,
};

enum class DdsKind {
  NotDds = 0,
  Dds,
  WrappedDds,
};

enum class DdsValidationStatus {
  Invalid = 0,
  UnsupportedValid,
  Valid,
};

struct DdsIssue {
  DdsIssueSeverity severity = DdsIssueSeverity::Info;
  std::string code;
  std::string message;
};

struct DdsValidationResult {
  DdsValidationStatus status = DdsValidationStatus::Invalid;
  DdsKind kind = DdsKind::NotDds;
  bool hasDdsMagic = false;
  bool wrappedHeader = false;
  bool dx10HeaderPresent = false;
  bool payloadTruncated = false;
  bool payloadHasTrailingBytes = false;
  bool headerLooksWrapped = false;
  std::size_t dataOffset = 0;
  std::size_t sourceSize = 0;
  std::size_t visibleSize = 0;
  std::size_t payloadSize = 0;
  std::size_t expectedPayloadSizeMin = 0;
  std::size_t expectedPayloadSizeMax = 0;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::uint32_t depth = 0;
  std::uint32_t mipCount = 1;
  std::uint32_t headerSize = 0;
  std::uint32_t pixelFormatSize = 0;
  std::uint32_t caps = 0;
  std::uint32_t caps2 = 0;
  std::uint32_t fourcc = 0;
  std::uint32_t dx10Format = 0;
  DdsFormat format = DdsFormat::Unknown;
  std::string formatName;
  std::string summary;
  std::vector<DdsIssue> issues;

  [[nodiscard]] bool is_valid() const noexcept { return status == DdsValidationStatus::Valid; }
  [[nodiscard]] bool is_supported() const noexcept { return status != DdsValidationStatus::Invalid; }
};

DdsValidationResult inspect_dds(std::span<const std::uint8_t> bytes);

inline DdsValidationResult validate_dds_bytes(std::span<const std::uint8_t> bytes) {
  return inspect_dds(bytes);
}

std::optional<DdsInfo> dds_info_from_validation(const DdsValidationResult& result);

const char* dds_issue_severity_name(DdsIssueSeverity severity) noexcept;
const char* dds_validation_status_name(DdsValidationStatus status) noexcept;
const char* dds_kind_name(DdsKind kind) noexcept;

} // namespace gf::textures
