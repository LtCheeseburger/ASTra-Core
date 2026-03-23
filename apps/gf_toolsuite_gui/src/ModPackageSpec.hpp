#pragma once
#include <QString>
#include <QStringList>

namespace gf::gui {

// Metadata collected from the user before exporting a mod package.
// All fields except modId / name / version are optional.
struct ModExportSpec {
    QString     modId;          // URL-safe id, e.g. "ncaa14_jersey_retro"
    QString     name;           // Human-readable name
    QString     version;        // e.g. "1.0.0"
    QString     author;
    QString     description;
    QStringList targetGameIds;  // Pre-filled from the active game's profile ID
    QStringList platforms;      // e.g. ["PS3"]
    QString     notes;
    QString     category;       // Optional, e.g. "Uniforms"
    QStringList tags;           // Optional searchable tags

    // Phase 5C: optional asset source paths (absolute, on the local machine)
    // iconSourcePath       — copied to icon.png in the package root on export
    // previewSourcePaths   — copied to preview_1.ext, preview_2.ext, etc.
    QString     iconSourcePath;
    QStringList previewSourcePaths;
};

// Result returned by ModPackageExporter::exportPackage().
struct ModExportResult {
    bool        success   = false;
    QString     message;
    QString     outputPath;   // Absolute path to the exported package folder
    int         fileCount = 0;
    QStringList errors;
    QStringList warnings;
};

} // namespace gf::gui
