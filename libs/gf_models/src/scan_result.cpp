#include "gf/models/scan_result.hpp"

#include <algorithm>

namespace gf::models {

std::string to_string(container_type t) {
  switch (t) {
    case container_type::big: return "BIG";
    case container_type::terf: return "TERF";
    default: return "Unknown";
  }
}

container_type container_type_from_string(const std::string& s) {
  std::string u = s;
  std::transform(u.begin(), u.end(), u.begin(), [](unsigned char c) { return (char)std::toupper(c); });
  if (u == "BIG") return container_type::big;
  if (u == "TERF") return container_type::terf;
  return container_type::unknown;
}


nlohmann::json to_json(const scan_result& r) {
  nlohmann::json j;
  j["platform"] = r.platform;
  j["scan_root"] = r.scan_root;
  j["counts"] = {
    {"ast", r.counts.ast},
    {"big", r.counts.big},
    {"terf", r.counts.terf},
    {"unknown", r.counts.unknown},
  };
  j["primary_container"] = to_string(r.primary_container);
  j["files_examined"] = r.files_examined;
  j["folders_scanned"] = r.folders_scanned;
  j["duration_ms"] = r.duration_ms;
  j["warnings"] = r.warnings;
  return j;
}

scan_result scan_result_from_json(const nlohmann::json& j) {
  scan_result r;
  if (j.contains("platform")) r.platform = j.value("platform", "");
  if (j.contains("scan_root")) r.scan_root = j.value("scan_root", "");
  if (j.contains("counts")) {
    const auto c = j.at("counts");
    r.counts.ast = c.value("ast", 0);
    r.counts.big = c.value("big", 0);
    r.counts.terf = c.value("terf", 0);
    r.counts.unknown = c.value("unknown", 0);
  }
  r.primary_container = container_type_from_string(j.value("primary_container", "Unknown"));
  r.files_examined = j.value("files_examined", 0);
  r.folders_scanned = j.value("folders_scanned", 0);
  r.duration_ms = j.value("duration_ms", 0);
  if (j.contains("warnings") && j.at("warnings").is_array()) {
    r.warnings = j.at("warnings").get<std::vector<std::string>>();
  }
  return r;
}

} // namespace gf::models

