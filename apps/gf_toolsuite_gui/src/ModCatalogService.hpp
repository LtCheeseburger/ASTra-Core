#pragma once
#include "ModCatalogEntry.hpp"
#include <QVector>

namespace gf::gui {

// Provides a read/validate/mutate view over the installed mod registry for a
// single profile workspace.
//
// ModCatalogService is stateless: every call reads directly from disk via
// ModRegistryStore and evaluates file-system state.  No in-memory cache is
// maintained — callers re-load as needed (e.g. after enable/disable).
class ModCatalogService {
public:
    // Load installed_registry.json, evaluate each record, and return catalog entries.
    // Returns an empty list (not an error) when no mods are installed.
    // Returns an empty list and writes to *outErr on I/O or parse failure.
    static QVector<ModCatalogEntry> load(const QString& workspacePath,
                                         QString*       outErr = nullptr);

    // Identical to load() — re-reads from disk and re-evaluates all entries.
    // Distinct name for caller readability (e.g. on a Refresh button).
    static QVector<ModCatalogEntry> refresh(const QString& workspacePath,
                                             QString*       outErr = nullptr);

    // Toggle the enabled flag for the install identified by installId.
    //
    // When enabling:
    //   - Checks file-level conflicts against all other currently-enabled mods.
    //   - Refuses (returns false, writes to *outErr) on hard conflict.
    //
    // Persists changes atomically via ModRegistryStore::save().
    // Returns false on I/O error or if installId is not found.
    static bool setEnabled(const QString& workspacePath,
                           const QString& installId,
                           bool           enabled,
                           QString*       outErr = nullptr);

private:
    static ModCatalogEntry evaluate(const InstalledModRecord& record,
                                    const QString&            workspacePath);

    static bool wouldConflict(const InstalledModRecord&          toEnable,
                              const QVector<InstalledModRecord>& allRecords,
                              QStringList&                       outConflicts);
};

} // namespace gf::gui
