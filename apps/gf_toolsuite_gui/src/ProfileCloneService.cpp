#include "ProfileCloneService.hpp"
#include "ModProfileManager.hpp"
#include "ModRegistryStore.hpp"
#include "ModWorkspaceManager.hpp"

#include <gf/core/log.hpp>

#include <QApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QProgressDialog>

namespace gf::gui {

// Recursively copies all files from srcDir into destDir, preserving the
// relative directory structure.  Returns the number of files copied.
static int copyDirRecursive(const QString& srcDir,
                             const QString& destDir,
                             QStringList&   warnings,
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
            if (!QFile::copy(fi.absoluteFilePath(), destFile)) {
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

// static
ProfileCloneResult ProfileCloneService::clone(const ModProfile&  sourceProfile,
                                                const QString&     newName,
                                                const QString&     gameDisplayName,
                                                ModProfileManager& mgr,
                                                QWidget*           progressParent)
{
    ProfileCloneResult result;

    if (!sourceProfile.isValid()) {
        result.message = "clone(): source profile is invalid.";
        return result;
    }
    if (newName.trimmed().isEmpty()) {
        result.message = "clone(): new profile name must not be empty.";
        return result;
    }

    // ── 1. Create the new profile ──────────────────────────────────────────────
    ModProfile newProfile;
    QString createErr;
    const QString description =
        QString("Cloned from \"%1\"").arg(sourceProfile.name);

    if (!mgr.createProfile(sourceProfile.gameId, newName.trimmed(), description,
                            &newProfile, &createErr, gameDisplayName)) {
        result.message = "Failed to create clone profile: " + createErr;
        return result;
    }

    result.newProfileId = newProfile.id;

    // ── 2. Copy overlay/ (baseline + roots) ───────────────────────────────────
    const WorkspaceLayout srcLayout  = WorkspaceLayout::from(sourceProfile.workspacePath);
    const WorkspaceLayout destLayout = WorkspaceLayout::from(newProfile.workspacePath);

    QProgressDialog* progress = nullptr;
    if (progressParent) {
        progress = new QProgressDialog(
            QString("Cloning profile \u201c%1\u201d\u2026").arg(newName.trimmed()),
            "Cancel", 0, 0, progressParent);
        progress->setWindowModality(Qt::WindowModal);
        progress->setMinimumDuration(500);
        progress->setValue(0);
        progress->show();
    }

    copyDirRecursive(srcLayout.overlayDir(), destLayout.overlayDir(),
                     result.warnings, progress);

    // ── 3. Copy installed mods (slot directories + registry) ──────────────────
    if (!progress || !progress->wasCanceled()) {
        copyDirRecursive(srcLayout.modsInstalledDir(), destLayout.modsInstalledDir(),
                         result.warnings, progress);

        // Also copy the registry file itself (sits in mods/, not mods/installed/)
        const QString srcReg  = ModRegistryStore::registryPath(sourceProfile.workspacePath);
        const QString destReg = ModRegistryStore::registryPath(newProfile.workspacePath);
        if (QFile::exists(srcReg)) {
            if (!QFile::copy(srcReg, destReg))
                result.warnings << "Could not copy installed_registry.json to clone.";
        }
    }

    if (progress) {
        progress->setValue(progress->maximum());
        delete progress;
    }

    result.success = true;
    result.message = QString("Profile \u201c%1\u201d cloned to \u201c%2\u201d.")
                         .arg(sourceProfile.name, newName.trimmed());

    gf::core::logInfo(gf::core::LogCategory::General,
                      "ProfileCloneService: clone complete",
                      (sourceProfile.name + " \u2192 " + newName.trimmed()).toStdString());
    return result;
}

} // namespace gf::gui
