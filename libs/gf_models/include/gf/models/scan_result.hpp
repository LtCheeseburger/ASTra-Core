#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace gf::models {

enum class container_type {
  big,
  terf,
  unknown,
};

struct container_counts {
  int ast = 0;
  int big = 0;
  int terf = 0;
  int unknown = 0;
};

struct scan_result {
  // Normalized platform key ("ps3", "psp", "psvita", "xbox360", etc.)
  std::string platform;

  // Resolved folder actually scanned (USRDIR/Image0/content/etc.)
  std::string scan_root;

  container_counts counts;
  container_type primary_container = container_type::unknown;

  std::int64_t files_examined = 0;
  std::int64_t folders_scanned = 0;
  std::int64_t duration_ms = 0;

  // Human-readable, non-blocking warnings intended for UI display.
  std::vector<std::string> warnings;
};

// Convenience helpers (string conversions)
std::string to_string(container_type t);
container_type container_type_from_string(const std::string& s);


// JSON helpers (for caching / CLI output)
nlohmann::json to_json(const scan_result& r);
scan_result scan_result_from_json(const nlohmann::json& j);

} // namespace gf::models