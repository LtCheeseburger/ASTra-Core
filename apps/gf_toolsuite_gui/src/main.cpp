#include "GameSelectorWindow.hpp"
#include "gf/core/log.hpp"
#include "gf/core/features.hpp"
#include "gf_core/version.hpp"

#include <QApplication>
#include <QIcon>
#include <QStyleFactory>
#include <QPalette>
#include <QColor>
#include <QMessageBox>
#include <QStandardPaths>

#include <exception>
#include <string>

// ─── top-level exception handler ─────────────────────────────────────────────
// std::terminate is called for unhandled exceptions; we hook it to flush a
// FATAL log entry before the process dies.
static void astraTerminateHandler() noexcept {
  try {
    const auto eptr = std::current_exception();
    if (eptr) {
      try {
        std::rethrow_exception(eptr);
      } catch (const std::exception& ex) {
        gf::core::logFatal(gf::core::LogCategory::Unknown,
                           "Unhandled exception — application terminating",
                           ex.what());
      } catch (...) {
        gf::core::logFatal(gf::core::LogCategory::Unknown,
                           "Unhandled unknown exception — application terminating");
      }
    } else {
      gf::core::logFatal(gf::core::LogCategory::Unknown,
                         "std::terminate called without active exception");
    }
    if (auto lg = gf::core::Log::get()) lg->flush();
  } catch (...) {}
  std::abort();
}

int main(int argc, char** argv) {
  // ── 1. Install terminate handler before anything else ──────────────────────
  std::set_terminate(astraTerminateHandler);

  // ── 2. Initialise logger with platform log directory ───────────────────────
  // Log file is in the platform-standard location (AppData/Logs/etc.) so we
  // pass an empty log_file and let Log::init() resolve it.
  gf::core::Log::init({
    .app_name         = "ASTra Core",
    .log_file         = "",          // auto-resolved to platform AppData
    .level            = gf::core::LogLevel::info,
    .max_file_size_mb = 4,
    .max_files        = 5,
  });

  // ── 3. Log startup context ─────────────────────────────────────────────────
  gf::core::logInfo(gf::core::LogCategory::General,
                    "ASTra Core GUI startup",
                    std::string("version=") + gf::core::kVersionString
#if defined(_WIN32)
                    + "  platform=Windows"
#elif defined(__APPLE__)
                    + "  platform=macOS"
#else
                    + "  platform=Linux"
#endif
#if defined(NDEBUG)
                    + "  build=Release"
#else
                    + "  build=Debug"
#endif
  );

  // ── 4. Qt application setup ────────────────────────────────────────────────
  QApplication app(argc, argv);
  app.setApplicationName("ASTra Core");
  app.setApplicationVersion(QString::fromUtf8(gf::core::kVersionString));
  app.setOrganizationName("ASTra");

  app.setStyle(QStyleFactory::create("Fusion"));

  QPalette dark;
  dark.setColor(QPalette::Window, QColor(32, 32, 32));
  dark.setColor(QPalette::WindowText, Qt::white);
  dark.setColor(QPalette::Base, QColor(24, 24, 24));
  dark.setColor(QPalette::AlternateBase, QColor(36, 36, 36));
  dark.setColor(QPalette::ToolTipBase, Qt::white);
  dark.setColor(QPalette::ToolTipText, Qt::white);
  dark.setColor(QPalette::Text, Qt::white);
  dark.setColor(QPalette::Button, QColor(45, 45, 45));
  dark.setColor(QPalette::ButtonText, Qt::white);
  dark.setColor(QPalette::BrightText, Qt::red);
  dark.setColor(QPalette::Link, QColor(111, 66, 193));
  dark.setColor(QPalette::Highlight, QColor(111, 66, 193));
  dark.setColor(QPalette::HighlightedText, Qt::white);
  dark.setColor(QPalette::PlaceholderText, QColor(160, 160, 160));
  dark.setColor(QPalette::Disabled, QPalette::Text, QColor(127, 127, 127));
  dark.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(127, 127, 127));
  dark.setColor(QPalette::Disabled, QPalette::WindowText, QColor(127, 127, 127));
  app.setPalette(dark);
  app.setStyleSheet(
      "QToolTip { color: #ffffff; background-color: #2d2d2d; border: 1px solid #6f42c1; }"
      "QMenu::item:selected { background-color: #6f42c1; }"
      "QTabBar::tab:selected { background: #3a3a3a; }"
  );

  // Developer opt-in: enable gated modules (still non-functional shells in v0.5.2).
  for (int i = 1; i < argc; ++i) {
    if (QString::fromUtf8(argv[i]) == "--enable-beta") {
      gf::core::set_beta_enabled(true);
      gf::core::logInfo(gf::core::LogCategory::General, "Beta features enabled via --enable-beta flag");
    }
  }

  app.setWindowIcon(QIcon(":/icons/astra_icon.png"));

  // ── 5. Main window ─────────────────────────────────────────────────────────
  try {
    gf::gui::GameSelectorWindow w;
    w.show();

    const int result = app.exec();
    gf::core::logInfo(gf::core::LogCategory::General, "ASTra Core shutdown — normal exit");
    if (auto lg = gf::core::Log::get()) lg->flush();
    return result;

  } catch (const std::exception& ex) {
    gf::core::logFatal(gf::core::LogCategory::Unknown,
                       "Fatal exception in application event loop",
                       ex.what());
    if (auto lg = gf::core::Log::get()) lg->flush();
    QMessageBox::critical(
        nullptr,
        "ASTra Core — Fatal Error",
        QString("ASTra Core encountered an unrecoverable error and must close.\n\n"
                "Error details have been written to the log file.\n\n%1")
            .arg(QString::fromStdString(ex.what())));
    return 1;
  } catch (...) {
    gf::core::logFatal(gf::core::LogCategory::Unknown,
                       "Fatal unknown exception in application event loop");
    if (auto lg = gf::core::Log::get()) lg->flush();
    QMessageBox::critical(
        nullptr,
        "ASTra Core — Fatal Error",
        "ASTra Core encountered an unknown unrecoverable error and must close.\n\n"
        "Error details have been written to the log file.");
    return 1;
  }
}
