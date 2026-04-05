#include "ProfileWorkspaceBuilder.hpp"
#include "ModWorkspaceManager.hpp"

#include <gf/core/log.hpp>

#include <QApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QProgressDialog>

namespace gf::gui {

// static
QString ProfileWorkspaceBuilder::gameCopyPath(const ModProfile& profile) {
    return WorkspaceLayout::from(profile.workspacePath).gameCopyDir();
}

// static
bool ProfileWorkspaceBuilder::isGameCopyPopulated(const ModProfile& profile) {
    if (profile.workspacePath.isEmpty()) return false;
    const QString copyDir = gameCopyPath(profile);
    if (!QDir(copyDir).exists()) return false;
    return !QDir(copyDir)
                .entryList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot)
                .isEmpty();
}

// ─── Internal helpers ─────────────────────────────────────────────────────────

static int copyDirRecursive(const QString&   srcDir,
                             const QString&   destDir,
                             QStringList&     warnings,
                             QProgressDialog* progress)
{
    int copied = 0;

    if (!QDir(srcDir).exists()) return 0;
    if (!QDir().mkpath(destDir)) {
        warnings << QString("Cannot create directory: %1").arg(destDir);
        return 0;
    }

    const QFileInfoList entries = QDir(srcDir).entryInfoList(
        QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);

    for (const QFileInfo& fi : entries) {
        if (progress && progress->wasCanceled()) break;

        if (fi.isDir()) {
            const QString subDest = QDir(destDir).filePath(fi.fileName());
            copied += copyDirRecursive(fi.absoluteFilePath(), subDest, warnings, progress);
        } else {
            const QString destFile = QDir(destDir).filePath(fi.fileName());
            if (QFile::exists(destFile)) {
                // Already present from a prior copy — count it and skip.
                ++copied;
            } else if (!QFile::copy(fi.absoluteFilePath(), destFile)) {
                warnings << QString("Could not copy: %1").arg(fi.fileName());
            } else {
                ++copied;
                if (progress) {
                    progress->setValue(progress->value() + 1);
                    qApp->processEvents(QEventLoop::ExcludeUserInputEvents);
                }
            }
        }
    }
    return copied;
}

// ─── Public API ───────────────────────────────────────────────────────────────

// static
ProfileWorkspaceBuildResult ProfileWorkspaceBuilder::buildGameCopy(
    const ModProfile& profile,
    const QString&    sourcePath,
    QWidget*          progressParent)
{
    ProfileWorkspaceBuildResult result;

    if (!profile.isValid()) {
        result.message = "buildGameCopy: profile is invalid.";
        return result;
    }
    if (sourcePath.trimmed().isEmpty()) {
        result.message = "buildGameCopy: source path is empty.";
        return result;
    }
    if (!QDir(sourcePath).exists()) {
        result.message = QString("buildGameCopy: source path does not exist: %1")
                             .arg(sourcePath);
        return result;
    }

    const QString destPath = gameCopyPath(profile);

    QProgressDialog* progress = nullptr;
    if (progressParent) {
        progress = new QProgressDialog(
            QString("Copying game files into profile workspace\u2026"),
            "Cancel", 0, 0, progressParent);
        progress->setWindowTitle("Building Profile Copy");
        progress->setWindowModality(Qt::WindowModal);
        progress->setMinimumDuration(500);
        progress->setValue(0);
        progress->show();
    }

    result.filesCopied = copyDirRecursive(sourcePath, destPath, result.warnings, progress);

    if (progress) {
        const bool cancelled = progress->wasCanceled();
        progress->setValue(progress->maximum());
        delete progress;

        if (cancelled) {
            result.message = "Copy cancelled by user.";
            return result;
        }
    }

    result.success = true;
    result.message = QString("%1 file(s) in profile workspace game copy.")
                         .arg(result.filesCopied);

    gf::core::logInfo(gf::core::LogCategory::General,
                      "ProfileWorkspaceBuilder: game copy built",
                      (profile.name + " <- " + sourcePath).toStdString());
    return result;
}

} // namespace gf::gui
