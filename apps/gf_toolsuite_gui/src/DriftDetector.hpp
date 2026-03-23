#pragma once
#include "DeploymentState.hpp"
#include "RuntimeTargetConfig.hpp"
#include <QString>
#include <QVector>

namespace gf::gui {

// A single file that differs from its expected state in the live directory.
struct DriftedFile {
    QString filename;
    QString reason;          // "missing" | "size_mismatch"
    qint64  expectedSize = 0;
    qint64  actualSize   = 0; // 0 when reason is "missing"
};

// Result of a single drift check pass.
struct DriftCheckResult {
    bool               isDrifted = false;
    QVector<DriftedFile> driftedFiles;
};

// Detects drift between the recorded deployment state and the live AST directory.
//
// Drift conditions (MVP):
//   - A recorded file is absent from the live directory          → "missing"
//   - A recorded file's size differs from the stored expectation → "size_mismatch"
//
// Hashing is deliberately excluded from this phase.
//
// queryStatus() is the primary entry point for the UI.  It:
//   1. Loads the deployment state (returns None if absent).
//   2. For "applied" states, runs checkDrift() against the live directory.
//   3. If drift is found, persists "drifted" to disk so the status survives
//      an app restart.
//   4. Returns the effective DeploymentStatus.
class DriftDetector {
public:
    // Pure filesystem comparison — does NOT read or write the workspace JSON.
    // Caller is responsible for ensuring state.files is non-empty and the
    // live directory path is valid.
    static DriftCheckResult checkDrift(const DeploymentState&     state,
                                        const RuntimeTargetConfig& runtime);

    // Load state, optionally run drift check, and return effective status.
    // Persists "drifted" to disk if drift is detected (non-fatal on write error).
    static DeploymentStatus queryStatus(const QString&             workspacePath,
                                         const RuntimeTargetConfig& runtime);

    // Convenience: returns true iff queryStatus == DeploymentStatus::Applied.
    // Use as a pre-launch validation hook (Phase 4D will prompt on false).
    static bool isDeploymentValid(const QString&             workspacePath,
                                   const RuntimeTargetConfig& runtime);
};

} // namespace gf::gui
