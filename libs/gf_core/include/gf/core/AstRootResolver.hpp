#pragma once

#include <filesystem>
#include <string>
#include <cstdint>

namespace gf::core {

enum class Platform {
  Unknown = 0,
  PS3,
  Xbox360,
  PS4,
  WiiU,
};

struct AstRootResolveInput {
  std::filesystem::path gameRoot;  // e.g. D:/Games/PS3/BLUS31159-[NCAA Football 14]
  Platform platform = Platform::Unknown;
};

struct AstRootResolveResult {
  std::filesystem::path astRoot;   // directory to scan for *.ast
  std::string details;             // human-readable reason
  bool usedFallback = false;       // true if we couldn't find a "best" root
};

// Clean, Qt-free resolver so the core library does not depend on Qt.
// Rules (single best root):
//  - PS3:  <gameRoot>/PS3_GAME/USRDIR
//  - Xbox360: <gameRoot>
//  - PS4:  <gameRoot>/Image0  OR  <any direct child of gameRoot>/Image0
//  - WiiU: <gameRoot>/content
//
// If the preferred root does not exist or appears to contain no *.ast files,
// we fall back to gameRoot.
class AstRootResolver {
public:
  static AstRootResolveResult resolveBest(const AstRootResolveInput& in);

private:
  static bool hasAnyAstFilesRecursive(const std::filesystem::path& root,
                                     int maxDepth);
  static std::filesystem::path tryFindPs4Image0(const std::filesystem::path& gameRoot);
};

} // namespace gf::core
