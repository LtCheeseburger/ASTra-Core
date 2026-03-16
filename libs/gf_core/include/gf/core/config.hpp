#pragma once
#include <nlohmann/json.hpp>
#include <spdlog/logger.h>
#include <string>

namespace gf::core {

class Config {
public:
  // Loads config from disk; if missing, returns defaults and optionally writes it.
  static nlohmann::json load_or_default(const std::string& path, bool write_if_missing = true);

  static nlohmann::json defaults();
};

} // namespace gf::core
