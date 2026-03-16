#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace gf::core {

// Minimal platform keys used across ASTra (GUI + CLI).
// These keys intentionally match what the GUI persists in GameEntry.platform.
namespace platform_keys {
static constexpr const char* kUnknown = "unknown";
static constexpr const char* kPs3 = "ps3";
static constexpr const char* kPs4 = "ps4";
static constexpr const char* kPsVita = "psvita";
static constexpr const char* kPsp = "psp";
static constexpr const char* kXbox360 = "xbox360";
static constexpr const char* kWiiU = "wiiu";
} // namespace platform_keys

struct container_sniff_counts {
  int big_count = 0;
  int terf_count = 0;
  int unknown_count = 0;
  int files_examined = 0;
  int folders_scanned = 0;
  std::string primary_type = "Unknown"; // "BIG" | "TERF" | "Unknown"
};

// Header-sniff (not extension-based): BIG/BIG4/BIGF/BIG  and TERF.
container_sniff_counts sniff_containers_by_header(const std::filesystem::path& root,
                                                 bool recursive,
                                                 int max_files = 50000);

// Heuristic platform detection based on common dump layouts.
// This is intentionally conservative: unknown is OK.
std::string detect_platform_key(const std::filesystem::path& selected_path);

// "Best effort" scan root resolver for container scanning (not AST root resolving).
// Returns the folder we should sniff for containers (USRDIR/Image0/content/etc.).
std::filesystem::path resolve_scan_root_for_platform(const std::filesystem::path& game_root,
                                                    const std::string& platform_key);

// True if the folder looks like a game dump root (soft signal; not a blocker).
bool looks_like_game_root(const std::filesystem::path& selected_path);

// Soft warnings for common selection mistakes (never blockers).
// - selected USRDIR instead of game root
// - PSP missing PSP_GAME
// - Vita has no root AST/BGFA
std::vector<std::string> compute_soft_warnings(const std::filesystem::path& selected_path,
                                               const std::string& platform_key,
                                               bool has_root_ast);

} // namespace gf::core
