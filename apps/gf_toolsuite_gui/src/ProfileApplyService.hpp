#pragma once
#include "ModProfile.hpp"
#include "RuntimeTargetConfig.hpp"
#include <QString>
#include <QStringList>

class QWidget;

namespace gf::gui {

// Result of a profile apply operation.
struct ProfileApplyResult {
    bool        success       = false;
    QString     message;
    int         filesCopied   = 0;
    QStringList appliedFiles;  // AST filenames written to the live directory
    QStringList errors;
    QStringList warnings;
    bool        backupCreated = false; // true if a new baseline_backup was created
};

// Applies a resolved profile to the live game AST directory.
//
// Pipeline:
//   1. Resolve the profile (baseline overlay + enabled mods)
//   2. Validate that all source files exist on disk
//   3. Optionally create <workspace>/snapshots/baseline_backup/ on first apply
//      (skipped if the directory already exists)
//   4. Copy each resolved file → runtime.astDirPath
//   5. Return the result
//
// Overwrite mechanism:
//   - Copies source → <live>/<filename>.astra_tmp
//   - Removes existing <live>/<filename>  (if present)
//   - Renames .astra_tmp → <live>/<filename>
//   This preserves the original file until the replacement is fully staged.
//
// Abort behaviour:
//   - Any failure immediately aborts the remaining copies and returns an error.
//   - Already-written files are NOT rolled back (the user can re-apply or
//     restore from the backup to recover).
//
// Safety:
//   - Never deletes files in the live directory except as part of overwriting
//     a file that is present in the resolved map.
//   - Never creates filenames that are not already resolved by the profile.
//   - Never touches any path outside runtime.astDirPath or the workspace.
class ProfileApplyService {
public:
    // Apply the profile to the live AST directory described by runtime.
    // Pass progressParent != nullptr to show a QProgressDialog.
    static ProfileApplyResult apply(const ModProfile&          profile,
                                     const RuntimeTargetConfig& runtime,
                                     bool                       createBackupIfNeeded = true,
                                     QWidget*                   progressParent       = nullptr);
};

} // namespace gf::gui
