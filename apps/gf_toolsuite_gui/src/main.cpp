#include "GameSelectorWindow.hpp"
#include "gf/core/log.hpp"
#include "gf/core/features.hpp"
#include "gf_core/version.hpp"

#include <QApplication>
#include <QIcon>
#include <QStyleFactory>
#include <QPalette>
#include <QColor>

int main(int argc, char** argv) {
  gf::core::Log::init({.app_name="ASTra Core", .log_file="astra_gui.log"});

  QApplication app(argc, argv);
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
    }
  }
  app.setWindowIcon(QIcon(":/icons/astra_icon.png"));
  gf::core::Log::get()->info("Launching ASTra Core GUI v{}", gf::core::kVersionString);

  gf::gui::GameSelectorWindow w;
  w.show();

  return app.exec();
}
