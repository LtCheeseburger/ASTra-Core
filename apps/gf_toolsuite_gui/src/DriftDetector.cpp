#include "DriftDetector.hpp"
#include "DeploymentStateStore.hpp"

#include <gf/core/log.hpp>

#include <QDir>
#include <QFileInfo>

namespace gf::gui {

// static
DriftCheckResult DriftDetector::checkDrift(const DeploymentState&     state,
                                             const RuntimeTargetConfig& runtime) {
    DriftCheckResult result;
    const QDir liveDir(runtime.astDirPath);

    for (const DeploymentFileEntry& entry : state.files) {
        // Phase 5A: use the stored absolute path when available; fall back to
        // the legacy single-root derivation for entries written before 5A.
        const QString livePath = entry.destPath.isEmpty()
            ? liveDir.filePath(entry.filename)
            : entry.destPath;
        const QFileInfo fi(livePath);

        if (!fi.exists()) {
            result.isDrifted = true;
            result.driftedFiles.append({entry.filename, QStringLiteral("missing"),
                                        entry.size, 0});
            continue;
        }

        const qint64 actualSize = fi.size();
        if (actualSize != entry.size) {
            result.isDrifted = true;
            result.driftedFiles.append({entry.filename, QStringLiteral("size_mismatch"),
                                        entry.size, actualSize});
        }
    }

    return result;
}

// static
DeploymentStatus DriftDetector::queryStatus(const QString&             workspacePath,
                                              const RuntimeTargetConfig& runtime) {
    QString loadErr;
    const auto stateOpt = DeploymentStateStore::load(workspacePath, &loadErr);

    if (!stateOpt.has_value()) {
        // File absent = never applied.  Load error is logged by the store.
        return DeploymentStatus::None;
    }

    const DeploymentState& state = *stateOpt;

    // Status was already determined on a previous check — trust it.
    if (state.status == DeploymentStatus::Drifted) return DeploymentStatus::Drifted;
    if (state.status == DeploymentStatus::Partial) return DeploymentStatus::Partial;
    if (state.status == DeploymentStatus::None)    return DeploymentStatus::None;

    // Status is "applied" — run the live filesystem comparison.
    const DriftCheckResult drift = checkDrift(state, runtime);

    if (!drift.isDrifted) return DeploymentStatus::Applied;

    // Drift detected: persist the new status so it survives a restart.
    DeploymentState updated = state;
    updated.status = DeploymentStatus::Drifted;
    QString saveErr;
    if (!DeploymentStateStore::save(workspacePath, updated, &saveErr)) {
        // Non-fatal — the drift is still reported to the caller even if
        // we could not persist it.
        gf::core::logWarn(gf::core::LogCategory::General,
                          "DriftDetector: could not persist drifted status",
                          saveErr.toStdString());
    }

    gf::core::logWarn(gf::core::LogCategory::General,
                      "DriftDetector: drift detected",
                      (workspacePath + " | " +
                       QString::number(drift.driftedFiles.size()) +
                       " file(s)").toStdString());
    return DeploymentStatus::Drifted;
}

// static
bool DriftDetector::isDeploymentValid(const QString&             workspacePath,
                                        const RuntimeTargetConfig& runtime) {
    return queryStatus(workspacePath, runtime) == DeploymentStatus::Applied;
}

} // namespace gf::gui
