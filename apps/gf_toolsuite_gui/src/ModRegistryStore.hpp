#pragma once
#include "InstalledModRecord.hpp"
#include <QVector>
#include <optional>

namespace gf::gui {

// Reads and writes installed_registry.json for a single profile workspace.
//
// Registry path: <workspacePath>/mods/installed_registry.json
// All writes are atomic via gf::core::safe_write_text with backup enabled.
//
// ModRegistryStore is stateless; every load/save operates directly on disk.
// There is no in-memory cache — callers are responsible for loading when needed.
class ModRegistryStore {
public:
    // Full path to the registry file for the given workspace root.
    static QString registryPath(const QString& workspacePath);

    // Load all records for a workspace.  Returns true even when the file is
    // absent (no mods installed yet).  Returns false only on I/O or parse error.
    bool load(const QString&               workspacePath,
              QVector<InstalledModRecord>& outRecords,
              QString*                     outErr) const;

    // Atomically replace the full record list on disk.
    bool save(const QString&                      workspacePath,
              const QVector<InstalledModRecord>&  records,
              QString*                            outErr) const;

    // Convenience: load, append record, save.
    // If the existing registry is unreadable the record is written alone
    // (preserving the install rather than failing).
    bool appendRecord(const QString&           workspacePath,
                      const InstalledModRecord& record,
                      QString*                 outErr) const;
};

} // namespace gf::gui
