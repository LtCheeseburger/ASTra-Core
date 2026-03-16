#include "PlatformUtils.hpp"

#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QUrl>

namespace gf::gui {

QString revealMenuLabel() {
#if defined(Q_OS_MAC)
  return "Reveal in Finder";
#elif defined(Q_OS_WIN)
  return "Reveal in Explorer";
#else
  return "Reveal in File Manager";
#endif
}

void revealInFileManager(const QString& path) {
  if (path.isEmpty()) return;
  const QFileInfo info(path);

  // Directory: just open it.
  if (info.isDir()) {
    QDesktopServices::openUrl(QUrl::fromLocalFile(info.absoluteFilePath()));
    return;
  }

#if defined(Q_OS_MAC)
  // Finder can reveal/select a file.
  QProcess::startDetached("open", QStringList() << "-R" << info.absoluteFilePath());
#elif defined(Q_OS_WIN)
  // Explorer can reveal/select a file.
  const QString native = QDir::toNativeSeparators(info.absoluteFilePath());
  QProcess::startDetached("explorer", QStringList() << "/select," + native);
#else
  // Best-effort: open containing folder.
  QDesktopServices::openUrl(QUrl::fromLocalFile(info.absolutePath()));
#endif
}

} // namespace gf::gui
