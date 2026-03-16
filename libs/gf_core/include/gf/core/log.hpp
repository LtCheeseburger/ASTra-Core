#pragma once
#include <memory>
#include <string>

// We call logger methods from multiple translation units (e.g., config.cpp),
// so we need the complete type here (forward-decl isn't enough).
#include <spdlog/logger.h>

namespace gf::core {

enum class LogLevel {
  trace,
  debug,
  info,
  warn,
  err,
  critical,
  off
};

struct LogInit {
  std::string app_name = "astra";
  std::string log_file = "astra.log";
  LogLevel level = LogLevel::info;
};

class Log {
public:
  static void init(const LogInit& init);
  static std::shared_ptr<spdlog::logger> get();
};

} // namespace gf::core
