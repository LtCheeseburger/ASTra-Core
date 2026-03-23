#pragma once
#include "ModProfile.hpp"
#include "RuntimeTargetConfig.hpp"
#include <QString>
#include <QStringList>

class QWidget;

namespace gf::gui {

class ModProfileManager;

// Result of a baseline capture attempt.
struct BaselineCaptureResult {
    bool        success      = false;
    QString     message;
    QString     profileId;   // ID of the created/updated profile (empty on failure)
    int         filesCopied  = 0;
    QStringList warnings;
};

// Captures a baseline snapshot of the live game AST files into a new profile
// workspace.
//
// Behavior:
//   1. Creates a new profile (via ModProfileManager) with the given name
//   2. Copies all qkl_*.AST files from astDirPath into
//      <workspace>/overlay/ast/
//   3. Marks the profile as isBaseline=true with the given BaselineType
//   4. Sets the new profile as the active profile for the game
//
// Safety:
//   - NEVER writes to astDirPath or any live game directory
//   - Only reads from astDirPath; writes only within the workspace
//   - If any copy fails, the partial workspace is left in place but the
//     profile is still persisted (reported as a warning, not a hard failure)
//
// A QProgressDialog is shown if progressParent is non-null.
class BaselineCaptureService {
public:
    // Single-root capture (legacy): copies qkl_*.AST files from astDirPath → overlay/ast/.
    //
    // gameDisplayName (optional): when non-empty the new profile's workspace path uses
    // the Phase 5D human-readable slug format (<gameSlug>/<profileSlug>__<shortId>/).
    // Pass an empty string to preserve the legacy <gameId>/<profileId>/ path format.
    static BaselineCaptureResult capture(const QString&      gameId,
                                         const QString&      astDirPath,
                                         const QString&      profileName,
                                         BaselineType        type,
                                         ModProfileManager&  mgr,
                                         QWidget*            progressParent = nullptr,
                                         const QString&      gameDisplayName = {});

    // Phase 5A: multi-root capture.
    // When runtime.contentRoots is empty, delegates to capture() for single-root behavior.
    // Otherwise captures:
    //   - runtime.astDirPath  → overlay/ast/              (base game root)
    //   - contentRoots[i].path → overlay/roots/<i>/       (update / DLC roots)
    // Writes overlay/roots/roots_manifest.json so ProfileResolverService can map
    // each captured file back to its live destination on apply.
    //
    // Fails clearly (before creating a profile) if the base root is missing or empty.
    // Warns when optional roots are configured but contain no qkl_*.AST files.
    //
    // gameDisplayName (optional): same slug-naming behavior as capture() above.
    static BaselineCaptureResult captureFromRuntime(const QString&             gameId,
                                                     const RuntimeTargetConfig& runtime,
                                                     const QString&             profileName,
                                                     BaselineType               type,
                                                     ModProfileManager&         mgr,
                                                     QWidget*                   progressParent = nullptr,
                                                     const QString&             gameDisplayName = {});
};

} // namespace gf::gui
