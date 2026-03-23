#pragma once
#include "ModManifest.hpp"
#include <QString>
#include <QStringList>
#include <QVector>

namespace gf::gui {

// A single file-copy operation produced by ModInstallPlanner.
struct InstallOp {
    QString relPath;      // Relative path within files/ (display + registry key)
    QString srcAbsPath;   // Source: <modFolder>/files/<relPath>
    QString dstAbsPath;   // Dest:   <workspace>/mods/installed/<slotName>/files/<relPath>
};

// Non-fatal informational warning attached to a plan.
struct InstallWarning {
    QString code;    // e.g. "no_game_target"
    QString message;
};

// Result of the planning phase.  If hardErrors is non-empty the install must
// be refused; warnings are surfaced to the user but do not block installation.
struct InstallPlan {
    ModManifest             manifest;
    QString                 installId;  // UUID generated at plan time
    QString                 slotName;   // "<modId>-<installId>" — used as the install subdirectory
    QString                 dstRoot;    // <workspace>/mods/installed/<slotName>/
    QVector<InstallOp>      ops;
    QVector<InstallWarning> warnings;
    QStringList             hardErrors;

    bool canProceed() const { return hardErrors.isEmpty(); }
};

} // namespace gf::gui
