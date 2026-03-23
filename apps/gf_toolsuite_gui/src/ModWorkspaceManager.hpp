#pragma once
#include <QString>
#include <QStringList>

namespace gf::gui {

// Describes the canonical on-disk layout of a mod workspace.
// All path derivation lives here so callers never hard-code subdirectory names.
struct WorkspaceLayout {
    QString root;

    // Top-level files
    QString profileJson()        const { return p("profile.json"); }

    // Mods staging area
    QString modsDir()            const { return p("mods"); }
    QString modsInstalledDir()   const { return p("mods/installed"); }
    QString modsImportedDir()    const { return p("mods/imported"); }
    QString modsCacheDir()       const { return p("mods/cache"); }

    // Per-type overlay outputs (future: patch routing will write here)
    QString overlayDir()         const { return p("overlay"); }
    QString overlayFilesDir()    const { return p("overlay/files"); }
    QString overlayAstDir()      const { return p("overlay/ast"); }
    QString overlayRsfDir()      const { return p("overlay/rsf"); }
    QString overlayTexturesDir() const { return p("overlay/textures"); }
    QString overlayTextDir()     const { return p("overlay/text"); }

    // Phase 5A: per-root overlay storage for multi-root content captures
    QString overlayRootsDir()           const { return p("overlay/roots"); }
    QString overlayRootDir(int idx)     const { return overlayRootsDir() + "/" + QString::number(idx); }
    QString rootsManifestPath()         const { return overlayRootsDir() + "/roots_manifest.json"; }

    // Build / temp / logs / snapshots
    QString buildDir()           const { return p("build"); }
    QString tempDir()            const { return p("temp"); }
    QString logsDir()            const { return p("logs"); }
    QString snapshotsDir()       const { return p("snapshots"); }

    static WorkspaceLayout from(const QString& workspacePath);

private:
    QString p(const char* rel) const;
};

// Manages the physical workspace: creation, validation, and guarded deletion.
// Never touches any path outside the expected workspaces base directory.
class ModWorkspaceManager {
public:
    // Full list of required subdirectories (used for creation and validation).
    static QStringList requiredDirectories(const WorkspaceLayout& layout);

    // Creates the workspace directory tree.  profile.json is written separately
    // by ModProfileStore after this returns.
    static bool createWorkspace(const WorkspaceLayout& layout, QString* outErr);

    // Returns true if the workspace looks intact (root + profile.json + all dirs).
    static bool validateWorkspace(const WorkspaceLayout& layout, QString* outErr);

    // Removes the workspace directory tree.
    // Guard: path must be non-empty and must reside under ModProfileStore::defaultWorkspaceRoot().
    // Call only after the profile has already been removed from the index.
    static bool deleteWorkspace(const WorkspaceLayout& layout, QString* outErr);
};

} // namespace gf::gui
