#pragma once
#include <QString>

namespace gf::gui {

// Centralized helpers for desktop/shell integration.
//
// All OS-specific open/reveal/clipboard logic lives here.
// UI classes must never call QDesktopServices or platform APIs directly.
class ShellHelper {
public:
    // Opens absPath in the system file manager (Explorer, Finder, Nautilus, etc.).
    // If absPath points to a directory it opens that directory.
    // If absPath points to a file it opens the containing directory.
    // No-op when absPath is empty or does not exist.
    static void openFolder(const QString& absPath);

    // Copies text to the system clipboard.
    static void copyToClipboard(const QString& text);
};

} // namespace gf::gui
