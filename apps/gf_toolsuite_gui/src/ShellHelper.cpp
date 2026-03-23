#include "ShellHelper.hpp"

#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QFileInfo>
#include <QUrl>

namespace gf::gui {

// static
void ShellHelper::openFolder(const QString& absPath) {
    if (absPath.isEmpty()) return;

    // If the path is a file, open its parent directory instead.
    // QDesktopServices::openUrl on a directory is universally supported.
    const QFileInfo fi(absPath);
    const QString target = (fi.exists() && !fi.isDir())
                           ? fi.absolutePath()
                           : absPath;

    QDesktopServices::openUrl(QUrl::fromLocalFile(target));
}

// static
void ShellHelper::copyToClipboard(const QString& text) {
    QApplication::clipboard()->setText(text);
}

} // namespace gf::gui
