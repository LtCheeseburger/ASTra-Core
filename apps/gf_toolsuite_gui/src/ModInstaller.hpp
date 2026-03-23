#pragma once
#include "ModManifest.hpp"
#include "ModInstallPlan.hpp"
#include "ModInstallTransaction.hpp"
#include "InstalledModRecord.hpp"
#include "ModProfile.hpp"
#include <QStringList>
#include <optional>

namespace gf::gui {

class ModRegistryStore;

// Full report returned to the caller after an install attempt.
// The plan is always populated (for diagnostics) even when the install fails.
struct ModInstallReport {
    bool        success = false;
    QString     message;
    InstallPlan plan;                             // Always set
    std::optional<InstalledModRecord> record;     // Set on success
    QStringList errors;
    QStringList warnings; // Non-fatal; forwarded from plan.warnings
};

// ModInstaller orchestrates the full install pipeline:
//   1. Build plan  (ModInstallPlanner)
//   2. Validate    (plan.canProceed())
//   3. Execute transaction (ModInstallTransaction)
//   4. Write registry record (ModRegistryStore)
//
// All logic is encapsulated here; callers (e.g. InstallModDialog) interact
// only with this class and the report it returns.
class ModInstaller {
public:
    // Install a mod from a local folder into a profile workspace.
    static ModInstallReport install(const ModManifest& manifest,
                                    const ModProfile&  profile,
                                    ModRegistryStore&  registry);

private:
    static InstalledModRecord buildRecord(const InstallPlan&   plan,
                                          const InstallResult& txResult,
                                          const ModProfile&    profile);
};

} // namespace gf::gui
