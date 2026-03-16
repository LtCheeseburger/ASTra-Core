#pragma once

#include <QString>
#include <QByteArray>
#include <QMessageBox>
#include <algorithm>

class QWidget;

namespace gf::gui {

// Shared, minimal "document" lifecycle state used by editor-style windows.
// v0.6.4: centralizes dirty/path/title/prompt behavior without changing core APIs.
struct DocumentLifecycle final {
  void setDirty(bool v) { dirty = v; }
  bool isDirty() const { return dirty; }

  QString path;           // empty = untitled
  QString titleHint;      // optional, e.g. game name / module
  bool dirty = false;
  bool readOnly = false;

  // Optional: hash of last-loaded contents (text or bytes) for cheap dirty checks.
  QByteArray loadedHash;

  QString fileNameOrUntitled() const {
    if (path.isEmpty()) return QString("Untitled");
    const int slash = std::max(path.lastIndexOf('/'), path.lastIndexOf('\\'));
    return slash >= 0 ? path.mid(slash + 1) : path;
  }

  // Builds a consistent window title like:
  // "ASTra Core - Text Editor - foo.xml *"
  QString makeWindowTitle(const QString& appName, const QString& moduleName) const {
    QString t = appName;
    if (!moduleName.isEmpty()) t += QString(" - %1").arg(moduleName);
    const QString f = fileNameOrUntitled();
    if (!f.isEmpty()) t += QString(" - %1").arg(f);
    if (dirty) t += " *";
    return t;
  }

  // Prompts user if dirty. Returns true if it's ok to discard/close.
  static bool maybePromptDiscard(QWidget* parent, bool dirty) {
    if (!dirty) return true;
    const auto rc = QMessageBox::warning(parent,
                                        "ASTra Core",
                                        "You have unsaved changes. Discard them?",
                                        QMessageBox::Discard | QMessageBox::Cancel,
                                        QMessageBox::Cancel);
    return rc == QMessageBox::Discard;
  }
};

} // namespace gf::gui
