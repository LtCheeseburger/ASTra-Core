#pragma once
#include <QString>
#include <QStringList>

namespace gf::gui {

// Persistent record of one successfully installed mod under a profile.
// Written to <workspace>/mods/installed_registry.json after a successful install.
struct InstalledModRecord {
    QString     installId;      // UUID matching the install slot directory
    QString     profileId;      // Profile this was installed into
    QString     gameId;         // Game (profile.gameId)
    QString     modId;
    QString     modVersion;
    QString     modName;
    QString     sourcePath;     // Original source folder (reference only)
    QStringList installedFiles; // Relative paths under <slotDir>/files/ that were installed
    QString     manifestHash;   // SHA-256 hex of astra_mod.json raw bytes
    bool        enabled     = true;
    QString     installedAt;    // ISO-8601 UTC
    QString     status;         // "ok" | "partial" | "conflict"
    QStringList warnings;
};

} // namespace gf::gui
