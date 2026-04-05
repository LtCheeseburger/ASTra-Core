#pragma once
#include <QString>
#include <QStringList>

namespace gf::gui {

// Result of a mod uninstall operation.
struct ModUninstallResult {
    bool        success = false;
    QString     message;
    QStringList warnings;
    int         filesRemoved = 0;
};

// Removes an installed mod from a profile workspace.
//
// Pipeline:
//   1. Load installed_registry.json for the profile
//   2. Find the record matching installId
//   3. Delete the install slot directory (<workspace>/mods/installed/<modId>-<installId>/)
//   4. Remove the record from the registry and save
//
// Safety:
//   - Only removes the specific install slot directory — never deletes unrelated files
//   - Registry is written atomically; a failure here is reported as a warning
//     (slot was deleted but registry update failed)
//   - Returns an error if installId is not found in the registry
class ModUninstallService {
public:
    // Uninstall the mod with the given installId from the profile workspace.
    static ModUninstallResult uninstall(const QString& workspacePath,
                                        const QString& installId);
};

} // namespace gf::gui
