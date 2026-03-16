#pragma once

#include <QString>

namespace gf::gui {

// Platform ids are stable strings persisted in the game library JSON.
// Keep them lowercase.
namespace platform_id {
inline constexpr const char* kUnknown  = "unknown";
inline constexpr const char* kPS2      = "ps2";
inline constexpr const char* kPS3      = "ps3";
inline constexpr const char* kPS4      = "ps4";
inline constexpr const char* kPSVita   = "psvita";
inline constexpr const char* kPSP      = "psp";
inline constexpr const char* kXbox360  = "xbox360";
inline constexpr const char* kXbox     = "xbox";
inline constexpr const char* kGameCube = "gamecube";
inline constexpr const char* kWii      = "wii";
inline constexpr const char* kWiiU     = "wiiu";
}

struct PlatformDetectResult {
  QString platformId;   // one of platform_id::*
  QString titleId;      // when available (e.g. BLUSxxxxx, CUSAxxxxx, SLUS_xxx.xx)
  QString displayTitle; // when available

  // Optional paths to platform-specific metadata/icon roots.
  QString primaryMetaPath; // e.g. PARAM.SFO, SYSTEM.CNF
  QString iconHintPath;    // e.g. ICON0.PNG or icon0.png if present
};

// Detects a game platform by inspecting a directory tree.
// This is intentionally format-agnostic so ASTra can grow beyond BGFA/AST.
PlatformDetectResult detect_platform_from_root(const QString& rootPath);

// Resource path for platform badge icon.
QString platform_badge_resource(const QString& platformId);

} // namespace gf::gui
