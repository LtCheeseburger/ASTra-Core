#pragma once
#include <QString>
#include <QVector>

namespace gf::gui {

// Deployment status as persisted in deployment_state.json.
//
// Only "applied" and "partial" are written by ProfileApplyService.
// "drifted" is written by DriftDetector when a filesystem mismatch is found.
// "none" is the in-memory sentinel for "no state file exists".
enum class DeploymentStatus {
    None    = 0, // No deployment_state.json (never applied or cleared)
    Applied = 1, // Apply completed successfully; no drift detected as of last check
    Partial = 2, // Apply was cancelled before all files were written
    Drifted = 3, // Drift detected since the last successful apply
};

inline QString deploymentStatusString(DeploymentStatus s) {
    switch (s) {
    case DeploymentStatus::Applied: return QStringLiteral("applied");
    case DeploymentStatus::Partial: return QStringLiteral("partial");
    case DeploymentStatus::Drifted: return QStringLiteral("drifted");
    default:                        return QStringLiteral("none");
    }
}

inline DeploymentStatus deploymentStatusFromString(const QString& s) {
    if (s == QLatin1String("applied")) return DeploymentStatus::Applied;
    if (s == QLatin1String("partial")) return DeploymentStatus::Partial;
    if (s == QLatin1String("drifted")) return DeploymentStatus::Drifted;
    return DeploymentStatus::None;
}

// One file entry recorded at apply time.
// 'size' is the file size (in bytes) as written to the live directory.
struct DeploymentFileEntry {
    QString filename; // e.g. "qkl_boot.AST" (original case from resolver)
    qint64  size = 0; // byte count; used for drift detection
    // Phase 5A: absolute path including filename to the live copy that was written.
    // Empty = legacy single-root; DriftDetector falls back to runtime.astDirPath + filename.
    QString destPath;
};

// Full deployment snapshot stored at <workspace>/deployment_state.json.
struct DeploymentState {
    QString                      profileId; // ID of the profile that was applied
    QString                      appliedAt; // ISO-8601 UTC timestamp
    DeploymentStatus             status    = DeploymentStatus::None;
    QVector<DeploymentFileEntry> files;    // files written to the live directory

    bool isValid() const { return !profileId.isEmpty(); }
};

} // namespace gf::gui
