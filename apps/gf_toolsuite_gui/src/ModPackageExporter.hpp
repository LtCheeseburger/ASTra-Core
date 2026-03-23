#pragma once
#include "ModPackageSpec.hpp"
#include "ProfileResolverService.hpp"
#include "RuntimeTargetConfig.hpp"
#include <QString>

class QWidget;

namespace gf::gui {

// Exports a resolved mod profile as a portable ASTra package folder.
//
// Output layout:
//   <parentDir>/<modId>/
//     astra_mod.json
//     files/
//       base/          ← files targeting runtime.astDirPath (base game)
//         qkl_boot.AST
//       update/        ← files targeting RuntimeContentKind::Update root (if any)
//         qkl_fe2ig.AST
//       dlc/           ← files targeting RuntimeContentKind::Dlc root (if any)
//         some_dlc_file.AST
//       custom_dlc/    ← files targeting RuntimeContentKind::CustomDlc root (if any)
//
// Round-trip: the output folder can be imported directly with the existing
// mod installer.  ProfileResolverService maps layer prefixes back to the
// correct live runtime root on the next apply.
//
// Safety:
//   - NEVER writes outside <parentDir>/<modId>/
//   - Does not modify any live game directory or workspace
//
// A QProgressDialog is shown if progressParent is non-null.
class ModPackageExporter {
public:
    static ModExportResult exportPackage(const ModExportSpec&       spec,
                                          const ProfileResolvedMap&  resolved,
                                          const RuntimeTargetConfig& runtime,
                                          const QString&             parentDir,
                                          QWidget*                   progressParent = nullptr);

private:
    // Returns the layer sub-directory name for a given destRootPath.
    // Empty destRootPath → "base".  Matched against runtime.contentRoots.
    static QString layerSubdir(const QString&             destRootPath,
                                const RuntimeTargetConfig& runtime);

    // Writes astra_mod.json into packageDir.
    static bool writeManifest(const ModExportSpec& spec,
                               const QStringList&   payloadFiles,
                               const QString&       packageDir,
                               QString*             outErr);
};

} // namespace gf::gui
