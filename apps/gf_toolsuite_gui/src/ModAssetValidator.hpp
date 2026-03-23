#pragma once
#include <QString>

namespace gf::gui {

// Centralised validation for mod asset files (icons, preview images).
//
// All validation is performed on absolute local paths.
// Supported image formats: PNG, JPEG/JPG, WebP.
//
// This is the single authority for "is this image usable?" across the export
// dialog, the metadata editor dialog, and any future asset-related UI.
// Never duplicate these checks in individual dialogs.
class ModAssetValidator {
public:
    // Returns true if relPath is safe for use as a relative path within a
    // package — no traversal, no NUL bytes, no Windows reserved device names.
    static bool isSafeRelPath(const QString& relPath);

    // Returns true if the file's extension is a supported image format.
    // Supported: .png, .jpg, .jpeg, .webp (case-insensitive).
    static bool isSupportedImageExtension(const QString& absPath);

    // Returns true if absPath:
    //   1. exists on disk,
    //   2. has a supported image extension, and
    //   3. can be opened and decoded by QImageReader.
    // Sets *outErr (if non-null) to a user-facing error message on failure.
    static bool validateImageFile(const QString& absPath, QString* outErr = nullptr);
};

} // namespace gf::gui
