#include "gf/core/config.hpp"
#include "gf/core/log.hpp"

#include <filesystem>
#include <fstream>

namespace gf::core {

nlohmann::json Config::defaults() {
  return nlohmann::json{
    {"app", {
      {"name", "ASTra Core"},
      {"log_level", "info"}
    }},
    {"paths", {
      {"workspace", "./workspace"},
      {"backups", "./backups"}
    }}
  };
}

nlohmann::json Config::load_or_default(const std::string& path, bool write_if_missing) {
  namespace fs = std::filesystem;

  if (!fs::exists(path)) {
    auto cfg = defaults();
    if (write_if_missing) {
      try {
        std::ofstream out(path, std::ios::binary);
        out << cfg.dump(2);
        out.close();
        gf::core::Log::get()->info("Wrote default config: {}", path);
      } catch (...) {
        gf::core::Log::get()->warn("Failed to write default config: {}", path);
      }
    }
    return cfg;
  }

  std::ifstream in(path, std::ios::binary);
  if (!in) {
    gf::core::Log::get()->warn("Could not open config: {}, using defaults", path);
    return defaults();
  }

  try {
    nlohmann::json cfg;
    in >> cfg;
    return cfg;
  } catch (...) {
    gf::core::Log::get()->warn("Config parse error: {}, using defaults", path);
    return defaults();
  }
}

} // namespace gf::core
