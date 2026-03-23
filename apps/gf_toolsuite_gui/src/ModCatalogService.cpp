#include "ModCatalogService.hpp"
#include "ModRegistryStore.hpp"
#include "ModWorkspaceManager.hpp"

#include <gf/core/log.hpp>

#include <QDir>
#include <QFileInfo>
#include <QSet>

namespace gf::gui {

// ── static helpers ────────────────────────────────────────────────────────────

// Reconstruct the install slot root from the record.
// Convention established in ModInstallPlanner: slotName = modId + "-" + installId
static QString installRootFor(const InstalledModRecord& record,
                               const QString&            workspacePath) {
    const WorkspaceLayout layout = WorkspaceLayout::from(workspacePath);
    return QDir(layout.modsInstalledDir()).filePath(
        record.modId + "-" + record.installId);
}

// ── ModCatalogService ─────────────────────────────────────────────────────────

// static
ModCatalogEntry ModCatalogService::evaluate(const InstalledModRecord& record,
                                             const QString&            workspacePath) {
    ModCatalogEntry entry;
    entry.record = record;

    // Malformed record — nothing more to check
    if (record.installId.isEmpty() || record.modId.isEmpty()) {
        entry.status            = ModEntryStatus::Invalid;
        entry.installRootExists = false;
        return entry;
    }

    const QString installRoot = installRootFor(record, workspacePath);
    entry.installRootExists = QDir(installRoot).exists();

    if (!entry.installRootExists) {
        entry.status = ModEntryStatus::Invalid;
        return entry;
    }

    // Count missing files
    const QDir filesRoot(QDir(installRoot).filePath("files"));
    for (const QString& rel : record.installedFiles) {
        if (!QFileInfo::exists(filesRoot.filePath(rel)))
            ++entry.missingFileCount;
    }

    // Determine status: registry "partial" flag or missing files → Partial;
    // disabled → Disabled; otherwise OK.
    if (record.status == "partial" || entry.missingFileCount > 0) {
        entry.status = ModEntryStatus::Partial;
    } else if (!record.enabled) {
        entry.status = ModEntryStatus::Disabled;
    } else {
        entry.status = ModEntryStatus::Ok;
    }

    return entry;
}

// static
bool ModCatalogService::wouldConflict(const InstalledModRecord&          toEnable,
                                       const QVector<InstalledModRecord>& allRecords,
                                       QStringList&                       outConflicts) {
    // Build the set of all files owned by currently-enabled mods (excluding toEnable itself)
    QSet<QString> inUse;
    for (const auto& r : allRecords) {
        if (r.installId == toEnable.installId) continue;
        if (!r.enabled) continue;
        for (const auto& f : r.installedFiles) inUse.insert(f);
    }

    for (const auto& f : toEnable.installedFiles) {
        if (inUse.contains(f)) outConflicts << f;
    }
    return !outConflicts.isEmpty();
}

// static
QVector<ModCatalogEntry> ModCatalogService::load(const QString& workspacePath,
                                                   QString*       outErr) {
    if (workspacePath.isEmpty()) {
        if (outErr) *outErr = "Workspace path is empty.";
        return {};
    }

    ModRegistryStore store;
    QVector<InstalledModRecord> records;
    QString loadErr;
    if (!store.load(workspacePath, records, &loadErr)) {
        if (outErr) *outErr = loadErr;
        gf::core::logWarn(gf::core::LogCategory::General,
                          "ModCatalogService: failed to load registry",
                          loadErr.toStdString());
        return {};
    }

    QVector<ModCatalogEntry> catalog;
    catalog.reserve(records.size());
    for (const auto& r : records)
        catalog << evaluate(r, workspacePath);

    return catalog;
}

// static
QVector<ModCatalogEntry> ModCatalogService::refresh(const QString& workspacePath,
                                                      QString*       outErr) {
    // Semantically identical to load(); exposed as a distinct name so callers can
    // signal user intent (e.g. "Refresh" button vs initial population).
    return load(workspacePath, outErr);
}

// static
bool ModCatalogService::setEnabled(const QString& workspacePath,
                                    const QString& installId,
                                    bool           enabled,
                                    QString*       outErr) {
    auto fail = [&](const QString& msg) {
        if (outErr) *outErr = msg;
        gf::core::logWarn(gf::core::LogCategory::General,
                          "ModCatalogService::setEnabled: " + msg.toStdString());
        return false;
    };

    if (workspacePath.isEmpty() || installId.isEmpty())
        return fail("workspacePath and installId must not be empty.");

    ModRegistryStore store;
    QVector<InstalledModRecord> records;
    QString loadErr;
    if (!store.load(workspacePath, records, &loadErr))
        return fail("Failed to load registry: " + loadErr);

    // Find the target record
    int idx = -1;
    for (int i = 0; i < records.size(); ++i) {
        if (records[i].installId == installId) { idx = i; break; }
    }
    if (idx < 0)
        return fail(QString("installId not found in registry: %1").arg(installId));

    // No-op if already in the requested state
    if (records[idx].enabled == enabled) return true;

    // When enabling, check for file-level conflicts against other enabled mods
    if (enabled) {
        QStringList conflicts;
        if (wouldConflict(records[idx], records, conflicts)) {
            return fail(QString(
                "Cannot enable '%1': %2 file conflict(s) with currently-enabled mods:\n  %3")
                .arg(records[idx].modName)
                .arg(conflicts.size())
                .arg(conflicts.mid(0, 5).join("\n  ") +
                     (conflicts.size() > 5 ? "\n  …" : "")));
        }
    }

    records[idx].enabled = enabled;

    QString saveErr;
    if (!store.save(workspacePath, records, &saveErr))
        return fail("Failed to save registry: " + saveErr);

    gf::core::logInfo(gf::core::LogCategory::General,
                      "ModCatalogService: " + QString(enabled ? "enabled" : "disabled").toStdString(),
                      records[idx].modId.toStdString());
    return true;
}

} // namespace gf::gui
