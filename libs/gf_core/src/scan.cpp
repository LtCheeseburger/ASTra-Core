#include "gf/core/scan.hpp"

#include "gf/core/container_magic.hpp"

#include <algorithm>
#include <fstream>
#include <set>
#include <unordered_set>

namespace gf::core {

static std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c){ return (char)std::tolower(c); });
  return s;
}

container_sniff_counts sniff_containers_by_header(const std::filesystem::path& root,
                                                 bool recursive,
                                                 int max_files) {
  container_sniff_counts out;
  if (root.empty() || !std::filesystem::exists(root) || !std::filesystem::is_directory(root)) return out;

  const std::unordered_set<std::string> kSkipExt = {
    "exe","dll","xbe","xex","elf","self","sprx","prx","bin","ini","cfg","txt","log","xml","json",
    "png","jpg","jpeg","dds","tga","bmp","webp"
  };

  std::set<std::filesystem::path> folders;

  auto scanOne = [&](const std::filesystem::path& scanRoot, bool doRecursive) {
    if (scanRoot.empty() || !std::filesystem::exists(scanRoot) || !std::filesystem::is_directory(scanRoot)) return;

    folders.insert(std::filesystem::absolute(scanRoot));

    std::error_code ec;
    auto it = doRecursive
      ? std::filesystem::recursive_directory_iterator(scanRoot, std::filesystem::directory_options::skip_permission_denied, ec)
      : std::filesystem::recursive_directory_iterator(); // dummy

    if (doRecursive) {
      for (auto& entry : it) {
        if (out.files_examined >= max_files) break;
        if (ec) break;
        if (!entry.is_regular_file(ec)) continue;
        const auto p = entry.path();
        const auto ext = lower(p.extension().string());
        if (!ext.empty() && ext[0] == '.') {
          if (kSkipExt.contains(ext.substr(1))) continue;
        }

        std::ifstream f(p, std::ios::binary);
        if (!f) continue;
        char b[4]{0,0,0,0};
        f.read(b, 4);
        if (f.gcount() < 4) continue;

        out.files_examined++;

        const auto t = classify_container_magic({(char)b[0],(char)b[1],(char)b[2],(char)b[3]});
        if (t == ContainerMagicType::Big) out.big_count++;
        else if (t == ContainerMagicType::Terf) out.terf_count++;
        else out.unknown_count++;

        folders.insert(std::filesystem::absolute(p.parent_path()));
      }
    } else {
      for (auto& entry : std::filesystem::directory_iterator(scanRoot, std::filesystem::directory_options::skip_permission_denied, ec)) {
        if (out.files_examined >= max_files) break;
        if (ec) break;
        if (!entry.is_regular_file(ec)) continue;
        const auto p = entry.path();
        const auto ext = lower(p.extension().string());
        if (!ext.empty() && ext[0] == '.') {
          if (kSkipExt.contains(ext.substr(1))) continue;
        }

        std::ifstream f(p, std::ios::binary);
        if (!f) continue;
        char b[4]{0,0,0,0};
        f.read(b, 4);
        if (f.gcount() < 4) continue;

        out.files_examined++;

        const auto t = classify_container_magic({(char)b[0],(char)b[1],(char)b[2],(char)b[3]});
        if (t == ContainerMagicType::Big) out.big_count++;
        else if (t == ContainerMagicType::Terf) out.terf_count++;
        else out.unknown_count++;

        folders.insert(std::filesystem::absolute(p.parent_path()));
      }
    }
  };

  // Main scan
  scanOne(root, recursive);

  // Cheap "DATA/" peek for non-recursive scans (TERF sometimes lives there)
  if (!recursive) {
    const auto data1 = root / "DATA";
    const auto data2 = root / "data";
    if (std::filesystem::exists(data1) && std::filesystem::is_directory(data1)) scanOne(data1, true);
    else if (std::filesystem::exists(data2) && std::filesystem::is_directory(data2)) scanOne(data2, true);
  }

  out.folders_scanned = (int)folders.size();
  if (out.big_count > 0 || out.terf_count > 0) {
    out.primary_type = (out.big_count >= out.terf_count) ? "BIG" : "TERF";
  } else {
    out.primary_type = "Unknown";
  }
  return out;
}

std::string detect_platform_key(const std::filesystem::path& selected_path) {
  namespace fs = std::filesystem;
  if (selected_path.empty()) return platform_keys::kUnknown;
  const fs::path p = fs::absolute(selected_path);

  if (fs::exists(p / "PS3_GAME") && fs::is_directory(p / "PS3_GAME")) return platform_keys::kPs3;
  if (fs::exists(p / "PSP_GAME") && fs::is_directory(p / "PSP_GAME")) return platform_keys::kPsp;

  // Wii U dumps commonly have "content" at root.
  if (fs::exists(p / "content") && fs::is_directory(p / "content")) return platform_keys::kWiiU;

  // PS4 package dumps commonly have Image0 at root.
  if (fs::exists(p / "Image0") && fs::is_directory(p / "Image0")) return platform_keys::kPs4;

  // Vita dumps often have eboot.bin at root.
  if (fs::exists(p / "eboot.bin") && fs::is_regular_file(p / "eboot.bin")) return platform_keys::kPsVita;

  // Xbox 360: default.xex at root is a decent signal.
  for (auto& entry : fs::directory_iterator(p, fs::directory_options::skip_permission_denied)) {
    if (!entry.is_regular_file()) continue;
    const auto name = lower(entry.path().filename().string());
    if (name == "default.xex" || name.ends_with(".xex")) return platform_keys::kXbox360;
  }

  return platform_keys::kUnknown;
}

std::filesystem::path resolve_scan_root_for_platform(const std::filesystem::path& game_root,
                                                    const std::string& platform_key) {
  namespace fs = std::filesystem;
  const fs::path root = fs::absolute(game_root);
  const std::string p = lower(platform_key);

  if (p == platform_keys::kPs3) {
    const auto usrdir = root / "PS3_GAME" / "USRDIR";
    if (fs::exists(usrdir) && fs::is_directory(usrdir)) return usrdir;
    return root;
  }
  if (p == platform_keys::kPs4) {
    const auto image0 = root / "Image0";
    if (fs::exists(image0) && fs::is_directory(image0)) return image0;
    return root;
  }
  if (p == platform_keys::kWiiU) {
    const auto content = root / "content";
    if (fs::exists(content) && fs::is_directory(content)) return content;
    return root;
  }
  if (p == platform_keys::kPsp) {
    const auto usrdir = root / "PSP_GAME" / "USRDIR";
    if (fs::exists(usrdir) && fs::is_directory(usrdir)) return usrdir;
    return root;
  }
  // Vita + Xbox360 + unknown default to root (Vita ASTs are root-only per contract)
  return root;
}

bool looks_like_game_root(const std::filesystem::path& selected_path) {
  namespace fs = std::filesystem;
  if (selected_path.empty() || !fs::exists(selected_path) || !fs::is_directory(selected_path)) return false;
  const auto p = fs::absolute(selected_path);

  // Common markers across supported platforms.
  if (fs::exists(p / "PS3_GAME")) return true;
  if (fs::exists(p / "PSP_GAME")) return true;
  if (fs::exists(p / "Image0")) return true;
  if (fs::exists(p / "content")) return true;
  if (fs::exists(p / "eboot.bin")) return true;

  // Param.sfo in common places.
  if (fs::exists(p / "PARAM.SFO") || fs::exists(p / "param.sfo")) return true;
  if (fs::exists(p / "PS3_GAME" / "PARAM.SFO") || fs::exists(p / "PSP_GAME" / "PARAM.SFO")) return true;
  if (fs::exists(p / "sce_sys" / "param.sfo") || fs::exists(p / "sce_sys" / "param.sfo")) return true;

  return false;
}

std::vector<std::string> compute_soft_warnings(const std::filesystem::path& selected_path,
                                               const std::string& platform_key,
                                               bool has_root_ast) {
  namespace fs = std::filesystem;
  std::vector<std::string> out;

  const auto p = fs::absolute(selected_path);
  const std::string leaf = lower(p.filename().string());
  const std::string plat = lower(platform_key);

  if (leaf == "usrdir") {
    out.push_back("You selected a USRDIR folder. For best results, select the game root (the folder containing PS3_GAME/ or the title root).");
  }

  if (plat == platform_keys::kPsp) {
    if (!fs::exists(p / "PSP_GAME") && lower(p.filename().string()) != "psp_game") {
      // If they selected something inside PSP_GAME, we don't warn.
      out.push_back("PSP dump missing PSP_GAME at the selected root. Select the game root folder that contains PSP_GAME/.");
    }
  }

  if (plat == platform_keys::kPsVita) {
    if (!has_root_ast) {
      out.push_back("PS Vita dumps typically place AST/BGFA archives at the title root (next to eboot.bin). No AST/BGFA were found at the expected root location.");
    }
  }

  if (!looks_like_game_root(p)) {
    out.push_back("Selected folder doesn't look like a game dump (missing PS3_GAME/PSP_GAME/Image0/content/eboot.bin/PARAM.SFO). If scans look wrong, choose the game root folder.");
  }

  return out;
}

} // namespace gf::core
