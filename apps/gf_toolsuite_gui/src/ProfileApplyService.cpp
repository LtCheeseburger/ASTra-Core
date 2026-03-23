#include "ProfileApplyService.hpp"
#include "ProfileResolverService.hpp"
#include "DeploymentStateStore.hpp"
#include "ModWorkspaceManager.hpp"

#include <gf/core/log.hpp>

#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QProgressDialog>
#include <QSet>

namespace gf::gui {

// static
ProfileApplyResult ProfileApplyService::apply(const ModProfile&          profile,
                                                const RuntimeTargetConfig& runtime,
                                                bool                       createBackupIfNeeded,
                                                QWidget*                   progressParent) {
    ProfileApplyResult result;

    // ── 1. Resolve profile ─────────────────────────────────────────────────────
    QString resolveErr;
    const ProfileResolvedMap resolved = ProfileResolverService::resolve(profile, runtime, &resolveErr);

    if (!resolved.isValid()) {
        result.errors  = resolved.errors;
        result.message = "Profile resolution failed:\n" + resolved.errors.join('\n');
        gf::core::logError(gf::core::LogCategory::General,
                           "ProfileApplyService: resolution failed",
                           resolveErr.toStdString());
        return result;
    }

    if (resolved.files.isEmpty()) {
        const QString e = "Nothing to apply: the resolved profile contains no AST files.";
        result.errors << e;
        result.message = e;
        return result;
    }

    // Phase 5E: surface resolver warnings (e.g. unconfigured layer roots).
    result.warnings << resolved.warnings;

    // ── 2. Validate source files exist ─────────────────────────────────────────
    QStringList missing;
    for (const ResolvedAstFile& entry : resolved.files) {
        if (!QFile::exists(entry.sourcePath))
            missing << QString("\"%1\" → %2").arg(entry.filename, entry.sourcePath);
    }
    if (!missing.isEmpty()) {
        result.errors << QString("Apply aborted: %1 source file(s) are missing:").arg(missing.size());
        result.errors << missing;
        result.message = result.errors.first();
        gf::core::logError(gf::core::LogCategory::FileIO,
                           "ProfileApplyService: missing source files",
                           QString::number(missing.size()).toStdString());
        return result;
    }

    // ── 3. Verify all destination root directories are accessible ──────────────
    // Phase 5A: entries carry destRootPath (empty = use runtime.astDirPath).
    // Validate all unique destination roots before touching any file.
    {
        QSet<QString> seenRoots;
        for (const ResolvedAstFile& entry : resolved.files) {
            const QString rootDir = entry.destRootPath.isEmpty()
                ? runtime.astDirPath
                : entry.destRootPath;
            seenRoots.insert(rootDir);
        }
        for (const QString& rootDir : seenRoots) {
            if (!QDir(rootDir).exists()) {
                const QString e =
                    QString("Destination root does not exist: %1").arg(rootDir);
                result.errors << e;
                result.message = e;
                gf::core::logError(gf::core::LogCategory::FileIO,
                                   "ProfileApplyService: missing dest root",
                                   rootDir.toStdString());
                return result;
            }
        }
    }

    // ── 4. Optional first-time backup ─────────────────────────────────────────
    // Creates <workspace>/snapshots/baseline_backup/ the very first time an
    // apply is run, so the user can always restore to the pre-first-apply state.
    if (createBackupIfNeeded) {
        const WorkspaceLayout layout    = WorkspaceLayout::from(profile.workspacePath);
        const QString         backupDir =
            QDir(layout.snapshotsDir()).filePath("baseline_backup");

        if (!QDir(backupDir).exists()) {
            if (QDir().mkpath(backupDir)) {
                int backupCount = 0;
                for (const ResolvedAstFile& entry : resolved.files) {
                    const QString destRoot = entry.destRootPath.isEmpty()
                        ? runtime.astDirPath
                        : entry.destRootPath;
                    const QString liveFile   = QDir(destRoot).filePath(entry.filename);
                    const QString backupFile = QDir(backupDir).filePath(entry.filename);
                    if (QFile::exists(liveFile) && !QFile::exists(backupFile)) {
                        if (QFile::copy(liveFile, backupFile))
                            ++backupCount;
                        else
                            result.warnings <<
                                QString("Backup: could not copy \"%1\" (apply will proceed)")
                                    .arg(entry.filename);
                    }
                }
                result.backupCreated = (backupCount > 0);
                gf::core::logInfo(gf::core::LogCategory::FileIO,
                                  "ProfileApplyService: baseline_backup created",
                                  (backupDir + " (" +
                                   QString::number(backupCount) + " files)").toStdString());
            } else {
                result.warnings <<
                    "Could not create baseline_backup directory — applying without backup.";
            }
        }
    }

    // ── 5. Copy resolved files to live directory ───────────────────────────────
    QProgressDialog* progress = nullptr;
    if (progressParent) {
        progress = new QProgressDialog(
            QString("Applying profile \u201c%1\u201d\u2026").arg(profile.name),
            "Cancel", 0, resolved.files.size(), progressParent);
        progress->setWindowModality(Qt::WindowModal);
        progress->setMinimumDuration(300);
        progress->show();
    }

    int  step      = 0;
    bool cancelled = false;
    QVector<DeploymentFileEntry> deployFiles; // collected per-file for state record
    deployFiles.reserve(resolved.files.size());

    for (const ResolvedAstFile& entry : resolved.files) {
        if (progress) {
            if (progress->wasCanceled()) { cancelled = true; break; }
            progress->setValue(step);
            progress->setLabelText(
                QString("Writing %1 (%2 / %3)\u2026")
                    .arg(entry.filename)
                    .arg(step + 1)
                    .arg(resolved.files.size()));
            qApp->processEvents(QEventLoop::ExcludeUserInputEvents);
        }

        // Phase 5A: route to the correct live root directory per entry.
        const QString destRootDir = entry.destRootPath.isEmpty()
            ? runtime.astDirPath
            : entry.destRootPath;
        const QString destPath = QDir(destRootDir).filePath(entry.filename);
        const QString tmpPath  = destPath + ".astra_tmp";

        // Clean up any leftover temp from a previously-aborted apply.
        if (QFile::exists(tmpPath)) QFile::remove(tmpPath);

        // Stage: copy source → temp (original is untouched if this fails).
        if (!QFile::copy(entry.sourcePath, tmpPath)) {
            if (progress) { progress->setValue(resolved.files.size()); delete progress; }
            result.errors << QString("Failed to stage \"%1\" from: %2")
                               .arg(entry.filename, entry.sourcePath);
            result.message = "Apply aborted: " + result.errors.last();
            gf::core::logError(gf::core::LogCategory::FileIO,
                               "ProfileApplyService: stage copy failed",
                               entry.filename.toStdString());
            return result;
        }

        // Remove original so we can rename the temp into place.
        if (QFile::exists(destPath) && !QFile::remove(destPath)) {
            QFile::remove(tmpPath);
            if (progress) { progress->setValue(resolved.files.size()); delete progress; }
            result.errors << QString("Failed to remove existing \"%1\" for replacement")
                               .arg(destPath);
            result.message = "Apply aborted: " + result.errors.last();
            gf::core::logError(gf::core::LogCategory::FileIO,
                               "ProfileApplyService: remove-for-overwrite failed",
                               destPath.toStdString());
            return result;
        }

        // Commit: rename temp → final path.
        if (!QFile::rename(tmpPath, destPath)) {
            QFile::remove(tmpPath);
            if (progress) { progress->setValue(resolved.files.size()); delete progress; }
            result.errors << QString("Failed to rename temp to \"%1\"").arg(destPath);
            result.message = "Apply aborted: " + result.errors.last();
            gf::core::logError(gf::core::LogCategory::FileIO,
                               "ProfileApplyService: rename commit failed",
                               destPath.toStdString());
            return result;
        }

        result.appliedFiles << entry.filename;
        // Phase 5A: store absolute destPath so DriftDetector can locate each file directly.
        deployFiles.append({entry.filename, QFileInfo(destPath).size(), destPath});
        ++step;
    }

    if (progress) {
        progress->setValue(resolved.files.size());
        delete progress;
    }

    if (cancelled) {
        result.warnings << QString(
            "Apply cancelled after %1 / %2 file(s). "
            "The live directory is in a partially-applied state. "
            "Re-apply or restore from baseline_backup to recover.")
            .arg(step).arg(resolved.files.size());
    }

    result.filesCopied = step;
    result.success     = !cancelled && result.errors.isEmpty();
    result.message     = result.success
        ? QString("Profile \u201c%1\u201d applied: %2 file(s) written.")
              .arg(profile.name).arg(step)
        : QString("Apply cancelled: %1 / %2 file(s) written.")
              .arg(step).arg(resolved.files.size());

    // ── 6. Persist deployment state ───────────────────────────────────────────
    if (!deployFiles.isEmpty()) {
        DeploymentState dstate;
        dstate.profileId = profile.id;
        dstate.appliedAt = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
        dstate.status    = cancelled ? DeploymentStatus::Partial : DeploymentStatus::Applied;
        dstate.files     = deployFiles;

        QString stateErr;
        if (!DeploymentStateStore::save(profile.workspacePath, dstate, &stateErr)) {
            // Non-fatal — files are applied, just the tracking record is missing.
            result.warnings << "Deployment state could not be saved: " + stateErr;
        }
    }

    gf::core::logInfo(gf::core::LogCategory::General,
                      "ProfileApplyService: apply complete",
                      (profile.name + " \u2014 " + QString::number(step) + " files").toStdString());
    return result;
}

} // namespace gf::gui
