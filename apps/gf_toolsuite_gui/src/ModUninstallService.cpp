#include "ModUninstallService.hpp"
#include "ModRegistryStore.hpp"
#include "ModWorkspaceManager.hpp"

#include <gf/core/log.hpp>

#include <QDir>

namespace gf::gui {

// static
ModUninstallResult ModUninstallService::uninstall(const QString& workspacePath,
                                                    const QString& installId)
{
    ModUninstallResult result;

    if (workspacePath.isEmpty() || installId.isEmpty()) {
        result.message = "uninstall(): workspacePath and installId must not be empty.";
        return result;
    }

    // ── 1. Load registry ───────────────────────────────────────────────────────
    ModRegistryStore store;
    QVector<InstalledModRecord> records;
    QString loadErr;
    if (!store.load(workspacePath, records, &loadErr)) {
        result.message = QString("Failed to load installed registry: %1").arg(loadErr);
        return result;
    }

    // ── 2. Find record ─────────────────────────────────────────────────────────
    int idx = -1;
    for (int i = 0; i < records.size(); ++i) {
        if (records[i].installId == installId) { idx = i; break; }
    }

    if (idx < 0) {
        result.message = QString("No installed mod found with install ID: %1").arg(installId);
        return result;
    }

    const InstalledModRecord rec = records[idx];

    // ── 3. Delete slot directory ───────────────────────────────────────────────
    // Slot path: <workspace>/mods/installed/<modId>-<installId>/
    const WorkspaceLayout layout = WorkspaceLayout::from(workspacePath);
    const QString slotDir = QDir(layout.modsInstalledDir())
                                .filePath(rec.modId + "-" + rec.installId);

    if (QDir(slotDir).exists()) {
        if (!QDir(slotDir).removeRecursively()) {
            result.warnings << QString(
                "Could not fully remove install slot directory: %1\n"
                "Some files may remain on disk. The registry will still be updated.")
                .arg(slotDir);
        } else {
            result.filesRemoved = rec.installedFiles.size();
        }
    } else {
        result.warnings << QString(
            "Install slot directory not found (already deleted?): %1").arg(slotDir);
    }

    // ── 4. Remove from registry and save ──────────────────────────────────────
    records.removeAt(idx);

    QString saveErr;
    if (!store.save(workspacePath, records, &saveErr)) {
        result.warnings << QString(
            "Registry updated in memory but could not be saved: %1\n"
            "The mod slot has been removed from disk.").arg(saveErr);
    }

    result.success = true;
    result.message = QString("Mod \"%1\" (v%2) uninstalled successfully.")
                         .arg(rec.modName, rec.modVersion);

    gf::core::logInfo(gf::core::LogCategory::General,
                      "ModUninstallService: uninstalled",
                      (rec.modId + " / " + rec.installId).toStdString());
    return result;
}

} // namespace gf::gui
