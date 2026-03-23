#pragma once
#include "ModProfile.hpp"
#include "RuntimeTargetConfig.hpp"
#include <QStringList>
#include <QVector>

namespace gf::gui {

// One resolved entry in the apply-time file map.
struct ResolvedAstFile {
    QString filename;      // Original-case AST filename, e.g. "qkl_boot.AST"
    QString sourcePath;    // Absolute path of the file to deploy
    QString providedBy;    // "baseline" or the mod name that supplies this file
    // Phase 5A: live root directory to write this file to.
    // Empty = use runtime.astDirPath (legacy single-root behavior).
    QString destRootPath;
};

// Result of profile resolution.
//
// 'files' is the ordered set of (filename → source path) pairs for all AST
// files that should be written to the live game directory on apply.
//
// If 'errors' is non-empty, resolution has failed with hard conflicts and
// 'files' is empty.  The caller must not proceed with an apply in that case.
struct ProfileResolvedMap {
    QVector<ResolvedAstFile> files;
    QStringList              errors;   // non-empty → resolution failed
    QStringList              warnings; // non-fatal notices (e.g. unconfigured layer roots)
    bool isValid() const { return errors.isEmpty(); }
};

// Resolves the apply-time file map for a profile.
//
// Sources (in priority order):
//   1. Baseline AST files: <workspace>/overlay/ast/   (seeded by BaselineCaptureService)
//   2. Enabled mod files:  <workspace>/mods/installed/<modId>-<installId>/files/
//      Each enabled mod can override baseline entries.
//      Two enabled mods providing the same AST filename → HARD ERROR.
//
// Only full-file AST replacement is supported.  No partial patching or merging.
//
// Stateless — reads registry and overlay directory from disk on each call.
class ProfileResolverService {
public:
    // Phase 5A: runtime is used to determine multi-root dest paths via roots_manifest.json.
    // In legacy (single-root) mode, runtime.contentRoots is empty and all files target
    // runtime.astDirPath — identical behavior to the original single-argument overload.
    static ProfileResolvedMap resolve(const ModProfile&          profile,
                                       const RuntimeTargetConfig& runtime,
                                       QString*                   outErr = nullptr);
};

} // namespace gf::gui
