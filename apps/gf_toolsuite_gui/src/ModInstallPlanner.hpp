#pragma once
#include "ModInstallPlan.hpp"
#include "ModProfile.hpp"

namespace gf::gui {

class ModRegistryStore;

// Produces a deterministic InstallPlan from a validated manifest + target profile.
//
// Does NOT execute any file I/O.  Returns a plan with hardErrors if installation
// must be refused; warnings are non-blocking and are forwarded to the user.
class ModInstallPlanner {
public:
    // Build a plan for installing manifest into profile's workspace.
    // The plan contains all paths needed for execution and is fully self-contained.
    static InstallPlan buildPlan(const ModManifest&      manifest,
                                 const ModProfile&       profile,
                                 const ModRegistryStore& registry);

private:
    static bool isAlreadyInstalled(const QString&          workspacePath,
                                   const QString&          modId,
                                   const ModRegistryStore& registry,
                                   QString*                outDetail);

    static bool hasFileConflict(const QString&          workspacePath,
                                const QStringList&      relPaths,
                                const ModRegistryStore& registry,
                                QStringList&            outConflicts);
};

} // namespace gf::gui
