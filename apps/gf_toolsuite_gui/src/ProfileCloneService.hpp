#pragma once
#include "ModProfile.hpp"
#include <QString>
#include <QStringList>

class QWidget;

namespace gf::gui {

class ModProfileManager;

// Result of a profile clone operation.
struct ProfileCloneResult {
    bool        success = false;
    QString     message;
    QString     newProfileId;   // ID of the newly created clone profile (empty on failure)
    QStringList warnings;
};

// Clones an existing profile into a new one with a different name.
//
// Pipeline:
//   1. Create a new profile via ModProfileManager::createProfile()
//   2. Copy overlay/ from source workspace to new workspace
//   3. Copy mods/installed/ and mods/installed_registry.json
//
// The source profile is never modified.
// The new profile is NOT set as active — the caller decides activation.
class ProfileCloneService {
public:
    // Clone sourceProfile into a new profile named newName.
    // gameDisplayName is forwarded to createProfile() for slug-based workspace naming.
    // progressParent may be nullptr to suppress the progress dialog.
    static ProfileCloneResult clone(const ModProfile&  sourceProfile,
                                     const QString&     newName,
                                     const QString&     gameDisplayName,
                                     ModProfileManager& mgr,
                                     QWidget*           progressParent = nullptr);
};

} // namespace gf::gui
