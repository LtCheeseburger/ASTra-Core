#include "gf/core/log.hpp"

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/rotating_file_sink.h>

#include <filesystem>
#include <string>

// ─── platform includes for AppData / XDG paths ───────────────────────────────
#if defined(_WIN32)
#  include <windows.h>
#  include <shlobj.h>
#elif defined(__APPLE__)
#  include <unistd.h>
#  include <pwd.h>
#endif

namespace gf::core {

// ─── internal state ───────────────────────────────────────────────────────────
static std::shared_ptr<spdlog::logger> g_logger;
static std::string                     g_log_file_path;

// ─── helpers ─────────────────────────────────────────────────────────────────
static spdlog::level::level_enum to_spd(LogLevel lvl) {
  switch (lvl) {
    case LogLevel::trace:    return spdlog::level::trace;
    case LogLevel::debug:    return spdlog::level::debug;
    case LogLevel::info:     return spdlog::level::info;
    case LogLevel::warn:     return spdlog::level::warn;
    case LogLevel::err:      return spdlog::level::err;
    case LogLevel::critical: return spdlog::level::critical;
    case LogLevel::off:      return spdlog::level::off;
  }
  return spdlog::level::info;
}

// Returns a writable log file path, creating the directory if needed.
// Priority: explicit path in LogInit → platform AppData/XDG → cwd fallback.
static std::string resolveLogPath(const std::string& explicit_log_file) {
  if (!explicit_log_file.empty()) {
    const std::filesystem::path p(explicit_log_file);
    if (p.has_parent_path()) {
      std::error_code ec;
      std::filesystem::create_directories(p.parent_path(), ec);
    }
    return explicit_log_file;
  }

  // Determine platform log directory.
  std::filesystem::path logDir;

#if defined(_WIN32)
  PWSTR appData = nullptr;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &appData))) {
    logDir = std::filesystem::path(appData) / "ASTra" / "logs";
    CoTaskMemFree(appData);
  }
#elif defined(__APPLE__)
  const char* home = getenv("HOME");
  if (!home) {
    if (const struct passwd* pw = getpwuid(getuid())) home = pw->pw_dir;
  }
  if (home) logDir = std::filesystem::path(home) / "Library" / "Logs" / "ASTra";
#else
  // Linux / POSIX — XDG_STATE_HOME preferred, then ~/.local/state
  const char* xdgState = getenv("XDG_STATE_HOME");
  if (xdgState && *xdgState) {
    logDir = std::filesystem::path(xdgState) / "astra" / "logs";
  } else {
    const char* home = getenv("HOME");
    if (home) logDir = std::filesystem::path(home) / ".local" / "state" / "astra" / "logs";
  }
#endif

  if (logDir.empty()) logDir = std::filesystem::current_path() / "logs";

  std::error_code ec;
  std::filesystem::create_directories(logDir, ec);
  if (ec) {
    // Last resort: write next to executable / cwd
    logDir = std::filesystem::current_path();
  }

  return (logDir / "astra.log").string();
}

// ─── Log::init ───────────────────────────────────────────────────────────────
void Log::init(const LogInit& init) {
  if (g_logger) return;

  g_log_file_path = resolveLogPath(init.log_file);

  const std::size_t maxBytes = init.max_file_size_mb * 1024 * 1024;

  try {
    auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    auto file_sink    = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
        g_log_file_path,
        maxBytes,
        static_cast<std::size_t>(init.max_files),
        /*rotate_on_open=*/false);

    g_logger = std::make_shared<spdlog::logger>(
        init.app_name,
        spdlog::sinks_init_list{console_sink, file_sink});

    // Pattern: timestamp · level · message
    g_logger->set_pattern("[%Y-%m-%d %H:%M:%S] [%^%8l%$] %v");

    spdlog::set_default_logger(g_logger);
    spdlog::set_level(to_spd(init.level));

    // Flush on warn and above so crashes still leave useful info behind.
    spdlog::flush_on(spdlog::level::warn);

    g_logger->info("[General] ASTra Core logger initialised  |  log -> {}", g_log_file_path);

  } catch (const std::exception& ex) {
    // Can't write to file; fall back to console-only.
    auto fallback = spdlog::stdout_color_mt("astra_fallback");
    fallback->warn("[General] Failed to open log file '{}': {}  — console-only logging active.",
                   g_log_file_path, ex.what());
    g_logger = fallback;
    spdlog::set_default_logger(g_logger);
  }
}

const std::string& Log::logFilePath() {
  return g_log_file_path;
}

std::shared_ptr<spdlog::logger> Log::get() {
  return g_logger ? g_logger : spdlog::default_logger();
}

// ─── category tag table ──────────────────────────────────────────────────────
std::string_view logCategoryTag(LogCategory cat) {
  switch (cat) {
    case LogCategory::General:        return "[General]";
    case LogCategory::AstParsing:     return "[AST parsing]";
    case LogCategory::AstExtraction:  return "[AST extraction]";
    case LogCategory::FileIO:         return "[File IO]";
    case LogCategory::DdsConversion:  return "[DDS/P3R conversion]";
    case LogCategory::TextureReplace: return "[Texture replace]";
    case LogCategory::TexturePreview: return "[Texture preview]";
    case LogCategory::Validation:     return "[Validation]";
    case LogCategory::UI:             return "[UI]";
    case LogCategory::Unknown:        return "[Unknown]";
  }
  return "[Unknown]";
}

// ─── structured helpers ───────────────────────────────────────────────────────
static std::string buildMsg(LogCategory cat, std::string_view msg) {
  std::string s;
  s.reserve(logCategoryTag(cat).size() + 1 + msg.size());
  s += logCategoryTag(cat);
  s += ' ';
  s += msg;
  return s;
}

static std::string buildMsg(LogCategory cat, std::string_view msg, std::string_view detail) {
  std::string s = buildMsg(cat, msg);
  if (!detail.empty()) { s += " | "; s += detail; }
  return s;
}

void logInfo  (LogCategory cat, std::string_view msg) { if (auto lg = Log::get()) lg->info (buildMsg(cat, msg)); }
void logWarn  (LogCategory cat, std::string_view msg) { if (auto lg = Log::get()) lg->warn (buildMsg(cat, msg)); }
void logError (LogCategory cat, std::string_view msg) { if (auto lg = Log::get()) lg->error(buildMsg(cat, msg)); }
void logFatal (LogCategory cat, std::string_view msg) { if (auto lg = Log::get()) { lg->critical(buildMsg(cat, msg)); lg->flush(); } }

void logInfo  (LogCategory cat, std::string_view msg, std::string_view d) { if (auto lg = Log::get()) lg->info (buildMsg(cat, msg, d)); }
void logWarn  (LogCategory cat, std::string_view msg, std::string_view d) { if (auto lg = Log::get()) lg->warn (buildMsg(cat, msg, d)); }
void logError (LogCategory cat, std::string_view msg, std::string_view d) { if (auto lg = Log::get()) lg->error(buildMsg(cat, msg, d)); }
void logFatal (LogCategory cat, std::string_view msg, std::string_view d) { if (auto lg = Log::get()) { lg->critical(buildMsg(cat, msg, d)); lg->flush(); } }

void logBreadcrumb(LogCategory cat, std::string_view stage) {
  if (auto lg = Log::get()) lg->info("{} >>> {}", logCategoryTag(cat), stage);
}

} // namespace gf::core
