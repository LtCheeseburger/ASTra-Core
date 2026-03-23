#pragma once
#include "DeploymentState.hpp"
#include <optional>

namespace gf::gui {

// Reads and writes deployment_state.json for a single profile workspace.
//
// Path:  <workspacePath>/deployment_state.json
// Writes are atomic via gf::core::safe_write_text (no backup — the state
// is cheap to re-derive by re-applying the profile).
//
// All methods are stateless; every call operates directly on disk.
class DeploymentStateStore {
public:
    // Full path to the state file for a workspace.
    static QString statePath(const QString& workspacePath);

    // Load the state.  Returns std::nullopt when the file is absent
    // (= DeploymentStatus::None — never applied).
    // Returns std::nullopt on I/O or parse error and writes to *outErr.
    static std::optional<DeploymentState> load(const QString& workspacePath,
                                                QString*       outErr = nullptr);

    // Atomically save the state via safe_write_text.
    static bool save(const QString&         workspacePath,
                     const DeploymentState& state,
                     QString*               outErr = nullptr);
};

} // namespace gf::gui
