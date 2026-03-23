#pragma once
#include <QString>
#include <QStringList>

namespace gf::gui {

static constexpr int         kModManifestSchemaVersion = 1;
static constexpr const char* kModManifestFilename      = "astra_mod.json";

// Parsed and validated content of an astra_mod.json manifest.
// All fields are normalised at parse time.  Invalid manifests are rejected by
// ModManifestReader before a ModManifest value is ever produced.
struct ModManifest {
    int         schemaVersion = 0;
    QString     modId;          // URL-safe id, e.g. "ncaa14_uniform_pack"
    QString     name;           // Human-readable mod name
    QString     version;        // e.g. "1.0.0"
    QString     author;
    QString     description;
    QStringList targetGameIds;  // gameId hashes this mod is compatible with (optional)
    QStringList platforms;      // e.g. ["PS3", "Xbox 360"] (optional)
    QString     notes;

    // Phase 5B: optional metadata fields (all may be empty)
    // package_type: "full_replacement" or "" (absent = treated as full_replacement)
    QString     packageType;    // "full_replacement" for Phase 5B exports; "" for legacy
    QString     category;       // Optional mod category (e.g. "Uniforms")
    QStringList tags;           // Optional searchable tags

    // Phase 5C: optional asset references (relative paths within the package folder)
    QString     icon;           // e.g. "icon.png" — relative to sourcePath
    QStringList previews;       // e.g. ["preview_1.png", "preview_2.jpg"]

    // Populated by ModManifestReader after parsing — not stored in the JSON.
    QString     sourcePath;       // Absolute path to the folder containing astra_mod.json
    QStringList payloadFiles;     // Relative paths under files/ (sorted)
    QByteArray  manifestRawBytes; // Raw manifest bytes — used for the install manifest hash
};

} // namespace gf::gui
