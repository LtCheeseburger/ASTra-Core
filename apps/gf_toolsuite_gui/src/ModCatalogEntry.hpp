#pragma once
#include "InstalledModRecord.hpp"

namespace gf::gui {

// Runtime-evaluated status of one installed mod in a profile workspace.
// Computed by ModCatalogService by cross-referencing the registry record
// with the actual files on disk.
enum class ModEntryStatus {
    Ok,       // enabled=true, install root and all registered files present
    Disabled, // enabled=false; files may be intact
    Partial,  // registry status was "partial", or some expected files are missing
    Invalid   // install root missing, malformed record, or critical files absent
};

// Read model that pairs an InstalledModRecord with its evaluated status.
// Never mutated by the UI — all mutations go through ModCatalogService.
struct ModCatalogEntry {
    InstalledModRecord record;
    ModEntryStatus     status           = ModEntryStatus::Invalid;
    bool               installRootExists = false;
    int                missingFileCount  = 0;

    // Convenience: human-readable label for status
    QString statusLabel() const {
        switch (status) {
        case ModEntryStatus::Ok:       return "Enabled";
        case ModEntryStatus::Disabled: return "Disabled";
        case ModEntryStatus::Partial:  return "Partial";
        case ModEntryStatus::Invalid:  return "Invalid";
        }
        return "Unknown";
    }
};

} // namespace gf::gui
