#pragma once
#include "RuntimeTargetConfig.hpp"
#include <QString>

namespace gf::gui {

// Result of a launch attempt.
struct LaunchResult {
    bool    success = false;
    QString message; // Human-readable summary on success
    QString error;   // Non-empty on failure
};

// Launches a game via a configured runtime target.
//
// For RPCS3: spawns rpcs3.exe --no-gui <gamePath> as a detached process.
// The host process does not track the child — launch is fire-and-forget.
//
// Validation:
//   - rpcs3ExePath must exist and be a regular file (not a directory)
//   - gamePath must be non-empty and must exist on the filesystem
//     (either a directory or a file, e.g. EBOOT.BIN)
//
// Does NOT perform deployment state checks — that is the caller's
// responsibility (see onLaunch() in GameSelectorWindow for the drift prompt).
class LaunchService {
public:
    // Launch the game.  gamePath is typically the registered GameEntry::rootPath.
    static LaunchResult launch(const RuntimeTargetConfig& runtime,
                                const QString&             gamePath);
};

} // namespace gf::gui
