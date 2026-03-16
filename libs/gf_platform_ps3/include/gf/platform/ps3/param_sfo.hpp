#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace gf::platform::ps3 {

// Minimal, robust PARAM.SFO reader (PSF format) to extract TITLE and TITLE_ID.
// Spec refs are widely available; we implement only what's needed + safety checks.

struct ParamSfoData {
  std::unordered_map<std::string, std::string> strings;
};

struct ParamSfoResult {
  std::string title;
  std::string title_id;
};

std::optional<ParamSfoData> read_param_sfo(const std::string& path, std::string* out_error = nullptr);
std::optional<ParamSfoResult> extract_title_and_id(const ParamSfoData& sfo);

} // namespace gf::platform::ps3
