#pragma once
#include "ModManifest.hpp"
#include <optional>

namespace gf::gui {

// Reads and validates an ASTra local mod manifest from a folder on disk.
//
// The folder must contain astra_mod.json and a files/ subdirectory with at
// least one file.  All validation — schema version, required fields, and path
// safety — happens here.  Callers receive a fully-validated ModManifest or a
// human-readable error string; there is no partial state.
class ModManifestReader {
public:
    // Read and validate the manifest rooted at <modFolder>/astra_mod.json.
    // Returns nullopt on any error; writes a description to *outErr.
    static std::optional<ModManifest> readFromFolder(const QString& modFolder,
                                                     QString*       outErr);

private:
    static bool validateModId(const QString& id, QString* outErr);
    static bool isPathSafe(const QString& relPath);
    static void collectFiles(const QString& baseDir,
                             const QString& relPrefix,
                             QStringList&   out,
                             QString*       outErr,
                             bool&          hadError);
};

} // namespace gf::gui
