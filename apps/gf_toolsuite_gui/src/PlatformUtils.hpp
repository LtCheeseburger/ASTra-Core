#pragma once

#include <QString>

namespace gf::gui {

// Human-friendly label for a "reveal" action (Explorer/Finder/File Manager).
QString revealMenuLabel();

// Reveal a file in the platform file manager, selecting the file when possible.
// If `path` is a directory, opens that directory.
void revealInFileManager(const QString& path);

} // namespace gf::gui
