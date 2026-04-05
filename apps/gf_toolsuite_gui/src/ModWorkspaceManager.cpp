#include "ModWorkspaceManager.hpp"
#include "ModProfileStore.hpp"

#include "gf/core/log.hpp"

#include <QDir>
#include <QFileInfo>

namespace gf::gui {

// ─── WorkspaceLayout ─────────────────────────────────────────────────────────

QString WorkspaceLayout::p(const char* rel) const {
    return QDir(root).filePath(QString::fromUtf8(rel));
}

WorkspaceLayout WorkspaceLayout::from(const QString& workspacePath) {
    WorkspaceLayout l;
    l.root = QDir::cleanPath(workspacePath);
    return l;
}

// ─── ModWorkspaceManager ─────────────────────────────────────────────────────

QStringList ModWorkspaceManager::requiredDirectories(const WorkspaceLayout& layout) {
    return {
        layout.modsDir(),
        layout.modsInstalledDir(),
        layout.modsImportedDir(),
        layout.modsCacheDir(),
        layout.overlayDir(),
        layout.overlayFilesDir(),
        layout.overlayAstDir(),
        layout.overlayRsfDir(),
        layout.overlayTexturesDir(),
        layout.overlayTextDir(),
        layout.gameCopyDir(),   // Phase 7: copy-based editing sandbox
        layout.buildDir(),
        layout.tempDir(),
        layout.logsDir(),
        layout.snapshotsDir(),
    };
}

bool ModWorkspaceManager::createWorkspace(const WorkspaceLayout& layout, QString* outErr) {
    if (layout.root.isEmpty()) {
        if (outErr) *outErr = "Workspace root path is empty";
        return false;
    }
    for (const QString& dir : requiredDirectories(layout)) {
        if (!QDir().mkpath(dir)) {
            if (outErr) *outErr = QString("Cannot create workspace directory: %1").arg(dir);
            gf::core::logError(gf::core::LogCategory::FileIO,
                               "ModWorkspaceManager: mkpath failed",
                               dir.toStdString());
            return false;
        }
    }
    gf::core::logInfo(gf::core::LogCategory::General,
                      "ModWorkspaceManager: workspace created",
                      layout.root.toStdString());
    return true;
}

bool ModWorkspaceManager::validateWorkspace(const WorkspaceLayout& layout, QString* outErr) {
    if (layout.root.isEmpty()) {
        if (outErr) *outErr = "Workspace root path is empty";
        return false;
    }
    if (!QDir(layout.root).exists()) {
        if (outErr) *outErr = QString("Workspace root does not exist: %1").arg(layout.root);
        return false;
    }
    if (!QFileInfo::exists(layout.profileJson())) {
        if (outErr) *outErr = "Missing profile.json in workspace root";
        return false;
    }
    for (const QString& dir : requiredDirectories(layout)) {
        if (!QDir(dir).exists()) {
            if (outErr) *outErr = QString("Missing workspace subdirectory: %1").arg(dir);
            return false;
        }
    }
    return true;
}

bool ModWorkspaceManager::deleteWorkspace(const WorkspaceLayout& layout, QString* outErr) {
    if (layout.root.isEmpty()) {
        if (outErr) *outErr = "Workspace root is empty — deletion refused";
        return false;
    }

    const QString cleanRoot    = QDir::cleanPath(layout.root);
    const QString expectedBase = QDir::cleanPath(ModProfileStore::defaultWorkspaceRoot());

    // Safety gate: must be a strict child of the workspaces base, never the base itself.
    if (cleanRoot == expectedBase || !cleanRoot.startsWith(expectedBase + "/")) {
        const QString msg =
            QString("Path is outside expected workspace base — deletion refused: %1").arg(cleanRoot);
        if (outErr) *outErr = msg;
        gf::core::logError(gf::core::LogCategory::FileIO,
                           "ModWorkspaceManager: deleteWorkspace safety check failed",
                           cleanRoot.toStdString());
        return false;
    }

    if (!QDir(cleanRoot).exists()) {
        // Already absent — treat as success (idempotent).
        return true;
    }

    if (!QDir(cleanRoot).removeRecursively()) {
        if (outErr)
            *outErr = QString("Failed to remove workspace directory: %1").arg(cleanRoot);
        gf::core::logError(gf::core::LogCategory::FileIO,
                           "ModWorkspaceManager: removeRecursively failed",
                           cleanRoot.toStdString());
        return false;
    }

    gf::core::logInfo(gf::core::LogCategory::General,
                      "ModWorkspaceManager: workspace deleted",
                      cleanRoot.toStdString());
    return true;
}

} // namespace gf::gui
