#pragma once
#include "ModProfile.hpp"
#include <QString>
#include <QStringList>

class QWidget;

namespace gf::gui {

// Result of a profile workspace copy operation.
struct ProfileWorkspaceBuildResult {
    bool        success     = false;
    int         filesCopied = 0;
    QString     message;
    QStringList warnings;
};

// Phase 7: copy-based workspace builder.
//
// Copies game content from a source directory into a profile's game_copy/
// directory so users always edit the profile copy, never the base game files.
//
// Pipeline:
//   1. Ensure game_copy/ exists under the workspace root.
//   2. Recursively copy all files from sourcePath into game_copy/.
//      Existing files in game_copy/ are left in place (incremental — only new
//      source files are added; nothing in game_copy/ is deleted).
//   3. Return a result struct with the file count and any per-file warnings.
//
// The source game is never modified.
class ProfileWorkspaceBuilder {
public:
    // Returns the absolute path to game_copy/ within the profile workspace.
    static QString gameCopyPath(const ModProfile& profile);

    // Returns true if game_copy/ exists and contains at least one entry
    // (file or sub-directory), indicating the copy has been performed.
    static bool isGameCopyPopulated(const ModProfile& profile);

    // Copies all files from sourcePath into profile's game_copy/ directory.
    // progressParent: pass a QWidget* to show a cancellable progress dialog;
    //                 nullptr = silent (no dialog shown).
    static ProfileWorkspaceBuildResult buildGameCopy(
        const ModProfile& profile,
        const QString&    sourcePath,
        QWidget*          progressParent = nullptr);
};

} // namespace gf::gui
