#pragma once
#include <memory>
#include <string>
#include <string_view>

// We call logger methods from multiple translation units (e.g., config.cpp),
// so we need the complete type here (forward-decl isn't enough).
#include <spdlog/logger.h>

namespace gf::core {

// ──────────────────────────────────────────────────────────────────────────────
// Log level
// ──────────────────────────────────────────────────────────────────────────────
enum class LogLevel {
  trace,
  debug,
  info,
  warn,
  err,
  critical,
  off
};

// ──────────────────────────────────────────────────────────────────────────────
// Diagnostic categories — used in every structured log entry.
// ──────────────────────────────────────────────────────────────────────────────
enum class LogCategory {
  General,        // startup/shutdown and uncategorized
  AstParsing,     // AST file loading and header/entry parsing
  AstExtraction,  // entry extraction, nested AST traversal
  FileIO,         // disk read/write, safe_write, backup
  DdsConversion,  // DDS/P3R/XPR2 decode, encode, rebuild
  TextureReplace, // full texture-replace pipeline
  TexturePreview, // DDS decode for the preview pane
  Validation,     // archive / entry / DDS validation checks
  UI,             // Qt dialog / widget / event errors
  FileDetection,  // magic/extension/content-sniff detection results
  Routing,        // editor/viewer routing decisions
  Update,         // auto-update check, download, and install events
  Network,        // general network I/O (outside of update flows)
  Unknown         // catch-all for unclassified errors
};

// Human-readable tag string inserted in every log line, e.g. "[AST parsing]"
std::string_view logCategoryTag(LogCategory cat);

// ──────────────────────────────────────────────────────────────────────────────
// Initialisation options
// ──────────────────────────────────────────────────────────────────────────────
struct LogInit {
  std::string app_name  = "astra";
  // Full path to the log file.  When empty, Log::init() picks a sensible
  // platform default under the user's application-data directory.
  std::string log_file;
  LogLevel    level     = LogLevel::info;
  // Rolling file settings
  std::size_t max_file_size_mb = 4;   // rotate after this many MiB
  std::size_t max_files        = 5;   // keep at most this many rotated files
};

// ──────────────────────────────────────────────────────────────────────────────
// Core logger singleton
// ──────────────────────────────────────────────────────────────────────────────
class Log {
public:
  /// Initialise file + console sinks.  Safe to call more than once (no-op after first).
  static void init(const LogInit& init = {});

  /// The path actually used for the primary log file (set after init()).
  static const std::string& logFilePath();

  /// Raw spdlog handle — prefer the category helpers below for new code.
  static std::shared_ptr<spdlog::logger> get();
};

// ──────────────────────────────────────────────────────────────────────────────
// Structured category-aware helpers
//   logInfo   / logWarn   / logError   / logFatal
// Each prepends "[<category tag>] " to the message so every log line carries
// its origin category without callers having to remember the format string.
// ──────────────────────────────────────────────────────────────────────────────
void logInfo   (LogCategory cat, std::string_view msg);
void logWarn   (LogCategory cat, std::string_view msg);
void logError  (LogCategory cat, std::string_view msg);
void logFatal  (LogCategory cat, std::string_view msg);

// Convenience overloads that accept an extra detail/context string appended
// after the primary message separated by " | ".
void logInfo   (LogCategory cat, std::string_view msg, std::string_view detail);
void logWarn   (LogCategory cat, std::string_view msg, std::string_view detail);
void logError  (LogCategory cat, std::string_view msg, std::string_view detail);
void logFatal  (LogCategory cat, std::string_view msg, std::string_view detail);

// ──────────────────────────────────────────────────────────────────────────────
// Breadcrumb / pipeline-stage helper
// Writes an INFO entry of the form  "[<cat>] >>> <stage>"
// Use at the start of each significant phase in a multi-step operation.
// ──────────────────────────────────────────────────────────────────────────────
void logBreadcrumb(LogCategory cat, std::string_view stage);

} // namespace gf::core
