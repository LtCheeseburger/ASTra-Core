#include "ModInstallPlanner.hpp"
#include "ModRegistryStore.hpp"
#include "ModWorkspaceManager.hpp"

#include <gf/core/log.hpp>

#include <QDir>
#include <QSet>
#include <QUuid>

namespace gf::gui {

// static
InstallPlan ModInstallPlanner::buildPlan(const ModManifest&      manifest,
                                          const ModProfile&       profile,
                                          const ModRegistryStore& registry) {
    InstallPlan plan;
    plan.manifest  = manifest;
    plan.installId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    plan.slotName  = manifest.modId + "-" + plan.installId;

    const WorkspaceLayout layout = WorkspaceLayout::from(profile.workspacePath);
    plan.dstRoot = QDir(layout.modsInstalledDir()).filePath(plan.slotName);

    // ── Profile validity ──────────────────────────────────────────────────────
    if (!profile.isValid()) {
        plan.hardErrors << "Target profile is invalid or incomplete.";
        return plan;
    }
    if (profile.workspacePath.isEmpty()) {
        plan.hardErrors << "Profile has no workspace path.";
        return plan;
    }

    // ── Game compatibility ────────────────────────────────────────────────────
    if (!manifest.targetGameIds.isEmpty() &&
        !manifest.targetGameIds.contains(profile.gameId)) {
        plan.hardErrors << QString(
            "This mod is not compatible with the selected game.\n"
            "  Mod targets: %1\n"
            "  Current game: %2")
            .arg(manifest.targetGameIds.join(", "), profile.gameId);
        return plan;
    }

    // ── Duplicate mod check ───────────────────────────────────────────────────
    {
        QString detail;
        if (isAlreadyInstalled(profile.workspacePath, manifest.modId, registry, &detail)) {
            plan.hardErrors << QString(
                "mod_id '%1' is already installed in this profile.\n"
                "  %2\n"
                "Remove the existing installation before installing again.")
                .arg(manifest.modId, detail);
            return plan;
        }
    }

    // ── File-level collision check ────────────────────────────────────────────
    {
        QStringList conflicts;
        if (hasFileConflict(profile.workspacePath, manifest.payloadFiles,
                            registry, conflicts)) {
            plan.hardErrors << QString(
                "Install would conflict with %1 already-installed file(s):\n  %2")
                .arg(conflicts.size())
                .arg(conflicts.mid(0, 5).join("\n  ") +
                     (conflicts.size() > 5 ? "\n  …" : ""));
        }
    }

    if (!plan.canProceed()) return plan;

    // ── Build copy operations ─────────────────────────────────────────────────
    const QDir filesDir(QDir(manifest.sourcePath).filePath("files"));
    for (const QString& rel : manifest.payloadFiles) {
        InstallOp op;
        op.relPath    = rel;
        op.srcAbsPath = filesDir.filePath(rel);
        op.dstAbsPath = QDir(plan.dstRoot).filePath("files/" + rel);
        plan.ops << op;
    }

    // ── Non-blocking warnings ─────────────────────────────────────────────────
    if (manifest.targetGameIds.isEmpty()) {
        plan.warnings << InstallWarning{
            "no_game_target",
            "Manifest has no target_game_ids — game compatibility is unverified."
        };
    }

    gf::core::logInfo(gf::core::LogCategory::General,
                      "ModInstallPlanner: plan built",
                      (manifest.modId + " — " +
                       QString::number(plan.ops.size()) + " ops").toStdString());
    return plan;
}

// static
bool ModInstallPlanner::isAlreadyInstalled(const QString&          workspacePath,
                                            const QString&          modId,
                                            const ModRegistryStore& registry,
                                            QString*                outDetail) {
    QVector<InstalledModRecord> records;
    QString err;
    // Treat an unreadable registry as "not installed" to avoid blocking
    if (!registry.load(workspacePath, records, &err)) return false;

    for (const auto& r : records) {
        if (r.modId == modId && r.enabled) {
            if (outDetail)
                *outDetail = QString("Existing install: %1 v%2 (id: %3)")
                                 .arg(r.modName, r.modVersion, r.installId);
            return true;
        }
    }
    return false;
}

// static
bool ModInstallPlanner::hasFileConflict(const QString&          workspacePath,
                                         const QStringList&      relPaths,
                                         const ModRegistryStore& registry,
                                         QStringList&            outConflicts) {
    QVector<InstalledModRecord> records;
    QString err;
    if (!registry.load(workspacePath, records, &err)) return false;

    // Collect all files owned by currently-enabled installs
    QSet<QString> installedFiles;
    for (const auto& r : records) {
        if (!r.enabled) continue;
        for (const auto& f : r.installedFiles) installedFiles.insert(f);
    }

    for (const auto& rel : relPaths) {
        if (installedFiles.contains(rel)) outConflicts << rel;
    }
    return !outConflicts.isEmpty();
}

} // namespace gf::gui
