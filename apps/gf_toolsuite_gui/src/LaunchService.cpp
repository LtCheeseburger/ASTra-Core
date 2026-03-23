#include "LaunchService.hpp"

#include <gf/core/log.hpp>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>

namespace gf::gui {

// static
LaunchResult LaunchService::launch(const RuntimeTargetConfig& runtime,
                                     const QString&             gamePath) {
    // ── Validate RPCS3 executable ─────────────────────────────────────────────
    if (runtime.rpcs3ExePath.trimmed().isEmpty()) {
        const QString e = "RPCS3 executable path is not configured.";
        gf::core::logError(gf::core::LogCategory::General,
                           "LaunchService: " + e.toStdString());
        return {false, {}, e};
    }

    const QFileInfo rpcs3Info(runtime.rpcs3ExePath);
    if (!rpcs3Info.exists()) {
        const QString e =
            QString("RPCS3 executable not found: %1").arg(runtime.rpcs3ExePath);
        gf::core::logError(gf::core::LogCategory::General,
                           "LaunchService: " + e.toStdString());
        return {false, {}, e};
    }
    if (rpcs3Info.isDir()) {
        const QString e =
            QString("RPCS3 path points to a directory, not an executable: %1")
                .arg(runtime.rpcs3ExePath);
        gf::core::logError(gf::core::LogCategory::General,
                           "LaunchService: " + e.toStdString());
        return {false, {}, e};
    }

    // ── Validate game path ────────────────────────────────────────────────────
    if (gamePath.trimmed().isEmpty()) {
        const QString e = "Game path is empty.";
        gf::core::logError(gf::core::LogCategory::General,
                           "LaunchService: " + e.toStdString());
        return {false, {}, e};
    }

    const bool gameExists = QFileInfo::exists(gamePath); // true for both files and dirs
    if (!gameExists) {
        const QString e =
            QString("Game path does not exist: %1").arg(gamePath);
        gf::core::logError(gf::core::LogCategory::General,
                           "LaunchService: " + e.toStdString());
        return {false, {}, e};
    }

    // ── Launch ────────────────────────────────────────────────────────────────
    // Use --no-gui to boot directly into the game without showing the RPCS3
    // game library window.  startDetached is fire-and-forget — ASTra does not
    // monitor or wait on the child process.
    const bool started = QProcess::startDetached(
        runtime.rpcs3ExePath,
        {QStringLiteral("--no-gui"), gamePath});

    if (!started) {
        const QString e =
            QString("Failed to start RPCS3 process.\n"
                    "Executable: %1\n"
                    "Game path:  %2")
                .arg(runtime.rpcs3ExePath, gamePath);
        gf::core::logError(gf::core::LogCategory::General,
                           "LaunchService: QProcess::startDetached failed",
                           runtime.rpcs3ExePath.toStdString());
        return {false, {}, e};
    }

    const QString msg =
        QString("RPCS3 launched with game: %1").arg(gamePath);
    gf::core::logInfo(gf::core::LogCategory::General,
                      "LaunchService: launched",
                      (runtime.rpcs3ExePath + " | " + gamePath).toStdString());
    return {true, msg, {}};
}

} // namespace gf::gui
