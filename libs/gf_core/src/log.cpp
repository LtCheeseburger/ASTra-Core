#include "gf/core/log.hpp"

#include <spdlog/spdlog.h>

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/basic_file_sink.h>

namespace gf::core {

static std::shared_ptr<spdlog::logger> g_logger;

static spdlog::level::level_enum to_spd(LogLevel lvl) {
  switch (lvl) {
    case LogLevel::trace: return spdlog::level::trace;
    case LogLevel::debug: return spdlog::level::debug;
    case LogLevel::info: return spdlog::level::info;
    case LogLevel::warn: return spdlog::level::warn;
    case LogLevel::err: return spdlog::level::err;
    case LogLevel::critical: return spdlog::level::critical;
    case LogLevel::off: return spdlog::level::off;
  }
  return spdlog::level::info;
}

void Log::init(const LogInit& init) {
  if (g_logger) return;

  auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
  auto file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(init.log_file, true);

  g_logger = std::make_shared<spdlog::logger>(
      init.app_name,
      spdlog::sinks_init_list{console_sink, file_sink}
  );

  spdlog::set_default_logger(g_logger);
  spdlog::set_level(to_spd(init.level));
  spdlog::flush_on(spdlog::level::info);

  g_logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v");
  g_logger->info("Logger initialized (file: {})", init.log_file);
}

std::shared_ptr<spdlog::logger> Log::get() {
  return g_logger ? g_logger : spdlog::default_logger();
}

} // namespace gf::core
