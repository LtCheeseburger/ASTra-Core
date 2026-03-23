#include "ModInstallTransaction.hpp"

#include <gf/core/log.hpp>

#include <QDir>
#include <QFile>
#include <QFileInfo>

namespace gf::gui {

// static
InstallResult ModInstallTransaction::execute(const InstallPlan& plan) {
    InstallResult result;

    if (!plan.canProceed()) {
        result.success = false;
        result.message = "Plan has hard errors; cannot execute.";
        result.errors  = plan.hardErrors;
        return result;
    }

    // The temp dir is a sibling of dstRoot so that the rename stays on the same
    // filesystem volume (required for an atomic directory rename).
    const QString tempRoot = plan.dstRoot + ".tmp_install";

    // Remove any leftover temp from a previously-aborted attempt.
    if (QDir(tempRoot).exists()) {
        if (!QDir(tempRoot).removeRecursively()) {
            gf::core::logWarn(gf::core::LogCategory::FileIO,
                              "ModInstallTransaction: could not remove leftover temp dir",
                              tempRoot.toStdString());
            // Non-fatal: stageFiles will fail gracefully if the dir is really stuck.
        }
    }

    // Stage
    QStringList staged;
    QString stageErr;
    if (!stageFiles(plan, tempRoot, staged, stageErr)) {
        rollback(tempRoot, plan.dstRoot);
        result.success = false;
        result.message = "Staging failed: " + stageErr;
        result.errors << stageErr;
        return result;
    }

    // Commit
    QString commitErr;
    if (!commitStage(tempRoot, plan.dstRoot, commitErr)) {
        rollback(tempRoot, plan.dstRoot);
        result.success = false;
        result.message = "Commit failed: " + commitErr;
        result.errors << commitErr;
        return result;
    }

    result.success        = true;
    result.message        = QString("Installed %1 file(s).").arg(staged.size());
    result.installedFiles = staged;

    gf::core::logInfo(gf::core::LogCategory::General,
                      "ModInstallTransaction: committed",
                      (plan.manifest.modId + " (" +
                       QString::number(staged.size()) + " files)").toStdString());
    return result;
}

// static
bool ModInstallTransaction::stageFiles(const InstallPlan& plan,
                                        const QString&     tempRoot,
                                        QStringList&       outStaged,
                                        QString&           outErr) {
    for (const InstallOp& op : plan.ops) {
        // Map destination path into the temp tree
        const QString relFromDst = QDir(plan.dstRoot).relativeFilePath(op.dstAbsPath);
        const QString tempDst    = QDir(tempRoot).filePath(relFromDst);

        // Create parent directories in the staging area
        const QString parentDir = QFileInfo(tempDst).absolutePath();
        if (!QDir().mkpath(parentDir)) {
            outErr = QString("Cannot create staging directory: %1").arg(parentDir);
            return false;
        }

        // Copy source file into staging
        if (!QFile::copy(op.srcAbsPath, tempDst)) {
            outErr = QString("Failed to stage: %1").arg(op.srcAbsPath);
            return false;
        }

        outStaged << op.relPath;
    }
    return true;
}

// static
bool ModInstallTransaction::commitStage(const QString& tempRoot,
                                         const QString& dstRoot,
                                         QString&       outErr) {
    // Destination must not already exist — the planner should have caught this,
    // but guard here as a second line of defence.
    if (QDir(dstRoot).exists()) {
        outErr = QString("Destination slot already exists, refusing overwrite: %1").arg(dstRoot);
        return false;
    }

    // Rename temp dir -> destination.  Both paths share the same parent
    // (mods/installed/), so this is an atomic same-volume rename on both
    // Windows (MoveFileExW) and POSIX (::rename).
    const QFileInfo dstInfo(dstRoot);
    const QString   parentDir  = dstInfo.absolutePath();
    const QString   srcDirName = QFileInfo(tempRoot).fileName();
    const QString   dstDirName = dstInfo.fileName();

    if (!QDir(parentDir).exists()) {
        // Ensure the installed/ directory itself exists before renaming into it
        if (!QDir().mkpath(parentDir)) {
            outErr = QString("Cannot create installed directory: %1").arg(parentDir);
            return false;
        }
    }

    if (!QDir(parentDir).rename(srcDirName, dstDirName)) {
        outErr = QString("Failed to rename staging dir to destination:\n  %1\n  → %2")
                     .arg(tempRoot, dstRoot);
        return false;
    }
    return true;
}

// static
void ModInstallTransaction::rollback(const QString& tempRoot, const QString& dstRoot) {
    if (QDir(tempRoot).exists()) {
        if (!QDir(tempRoot).removeRecursively()) {
            gf::core::logWarn(gf::core::LogCategory::FileIO,
                              "ModInstallTransaction: rollback: could not remove temp",
                              tempRoot.toStdString());
        }
    }
    // If commitStage partially succeeded, remove the destination too.
    if (QDir(dstRoot).exists()) {
        if (!QDir(dstRoot).removeRecursively()) {
            gf::core::logWarn(gf::core::LogCategory::FileIO,
                              "ModInstallTransaction: rollback: could not remove partial dest",
                              dstRoot.toStdString());
        }
    }
}

} // namespace gf::gui
