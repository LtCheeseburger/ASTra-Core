#include "gf/core/AstRootResolver.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;

namespace gf::core {

bool AstRootResolver::hasAnyAstFilesRecursive(const fs::path& root, int maxDepth) {
  if (maxDepth < 0) return false;
  if (!fs::exists(root) || !fs::is_directory(root)) return false;

  try {
    for (const auto& entry : fs::directory_iterator(root)) {
      if (entry.is_regular_file()) {
        auto ext = entry.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (ext == ".ast") return true;
      } else if (entry.is_directory()) {
        if (hasAnyAstFilesRecursive(entry.path(), maxDepth - 1))
          return true;
      }
    }
  } catch (...) {
    // swallow filesystem errors
  }

  return false;
}

fs::path AstRootResolver::tryFindPs4Image0(const fs::path& gameRoot) {
  fs::path direct = gameRoot / "Image0";
  if (fs::exists(direct) && fs::is_directory(direct))
    return direct;

  // one level deep
  try {
    for (const auto& entry : fs::directory_iterator(gameRoot)) {
      if (!entry.is_directory()) continue;
      fs::path candidate = entry.path() / "Image0";
      if (fs::exists(candidate) && fs::is_directory(candidate))
        return candidate;
    }
  } catch (...) {
  }

  return {};
}

AstRootResolveResult AstRootResolver::resolveBest(const AstRootResolveInput& in) {
  AstRootResolveResult out{};
  const fs::path gameRoot = in.gameRoot;

  if (!fs::exists(gameRoot) || !fs::is_directory(gameRoot)) {
    out.astRoot = std::filesystem::path{};
    out.details = "Game root does not exist or is not a directory";
    out.usedFallback = true;
    return out;
  }

  fs::path preferred{};

  switch (in.platform) {
    case Platform::PS3: {
      preferred = gameRoot / "PS3_GAME" / "USRDIR";
      break;
    }
    case Platform::Xbox360: {
      preferred = gameRoot; // flat root
      break;
    }
    case Platform::PS4: {
      preferred = tryFindPs4Image0(gameRoot);
      break;
    }
    case Platform::WiiU: {
      // Wii U disc/installed layout typically stores game data under "content"
      preferred = gameRoot / "content";
      break;
    }
    case Platform::Unknown:
    default:
      break;
  }

  // If preferred exists and looks plausible (has .ast somewhere nearby), use it.
  if (!preferred.empty() && fs::exists(preferred) && fs::is_directory(preferred)) {
    if (hasAnyAstFilesRecursive(preferred, /*maxDepth=*/3)) {
      out.astRoot = preferred;
      out.details = "Resolved preferred platform root";
      out.usedFallback = false;
      return out;
    }
  }

  // If platform is unknown (or preferred failed), attempt lightweight heuristics in order:
  // 1) PS3 USRDIR
  fs::path ps3 = gameRoot / "PS3_GAME" / "USRDIR";
  if (fs::exists(ps3) && fs::is_directory(ps3) && hasAnyAstFilesRecursive(ps3, 3)) {
    out.astRoot = ps3;
    out.details = "Heuristic: discovered PS3 PS3_GAME/USRDIR";
    out.usedFallback = false;
    return out;
  }

  // 2) PS4 Image0 (direct or one-level deep)
  fs::path ps4 = tryFindPs4Image0(gameRoot);
  if (!ps4.empty() && fs::exists(ps4) && fs::is_directory(ps4) && hasAnyAstFilesRecursive(ps4, 3)) {
    out.astRoot = ps4;
    out.details = "Heuristic: discovered PS4 Image0";
    out.usedFallback = false;
    return out;
  }

  
  // 3) WiiU content folder
  fs::path wiiu = gameRoot / "content";
  if (fs::exists(wiiu) && fs::is_directory(wiiu) && hasAnyAstFilesRecursive(wiiu, 3)) {
    out.astRoot = wiiu;
    out.details = "Heuristic: discovered WiiU content";
    out.usedFallback = false;
    return out;
  }

// 3) If the game root itself contains ASTs, accept it.
  if (hasAnyAstFilesRecursive(gameRoot, 3)) {
    out.astRoot = gameRoot;
    out.details = "Fallback: using game root (contains ASTs)";
    out.usedFallback = true;
    return out;
  }

  // 4) Last resort: pick a direct child that contains ASTs within 2 levels.
  try {
    for (const auto& entry : fs::directory_iterator(gameRoot)) {
      if (!entry.is_directory()) continue;
      if (hasAnyAstFilesRecursive(entry.path(), 2)) {
        out.astRoot = entry.path();
        out.details = "Fallback: discovered ASTs in a child folder";
        out.usedFallback = true;
        return out;
      }
    }
  } catch (...) {
  }

  // Nothing found: fall back to game root to keep the app functional.
  out.astRoot = gameRoot;
  out.details = "Fallback: no AST root found; using game root";
  out.usedFallback = true;
  return out;
}

} // namespace gf::core
