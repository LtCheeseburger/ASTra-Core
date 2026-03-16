#include "gf/core/config.hpp"
#include "gf/core/log.hpp"
#include "gf/core/scan.hpp"
#include "gf/core/AstArchive.hpp"
#include "gf/models/scan_result.hpp"
#include "gf_core/version.hpp"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

static bool has_arg(int argc, char** argv, const std::string& a) {
  for (int i = 1; i < argc; ++i) {
    if (argv[i] == a) return true;
  }
  return false;
}

static std::string get_arg_value(int argc, char** argv, const std::string& key) {
  for (int i = 1; i + 1 < argc; ++i) {
    if (argv[i] == key) return argv[i + 1];
  }
  return {};
}

static void print_usage() {
  std::cout <<
    "astra (CLI)\n"
    "Usage:\n"
    "  astra --version\n"
    "  astra scan --path <dir> [--json]\n\n"
    "  astra ast-validate --path <file_or_dir> [--recursive] [--max <N>] [--json]\n\n"
    "Notes:\n"
    "  - 'scan' is read-only and uses the same non-invasive heuristics as the GUI.\n"
    "  - 'ast-validate' is read-only and attempts to parse AST/BGFA directory indexes safely.\n"
    "  - PSVita AST/BGFA is treated as root-only (per contract).\n";
}

static int cmd_scan(int argc, char** argv) {
  namespace fs = std::filesystem;

  const std::string pathStr = get_arg_value(argc, argv, "--path");
  if (pathStr.empty()) {
    std::cerr << "error: missing --path\n";
    print_usage();
    return 2;
  }

  const fs::path selected = fs::path(pathStr);
  if (!fs::exists(selected) || !fs::is_directory(selected)) {
    std::cerr << "error: path does not exist or is not a directory\n";
    return 2;
  }

  const auto t0 = std::chrono::steady_clock::now();

  gf::models::scan_result r;
  r.platform = gf::core::detect_platform_key(selected);
  const fs::path scanRoot = gf::core::resolve_scan_root_for_platform(selected, r.platform);
  r.scan_root = fs::absolute(scanRoot).string();

  // AST/BGFA counting:
  // - Default: root-only (non-recursive) to match GUI behavior.
  // - Wii U: recursive (archives often live in subfolders).
  const bool astRecursive = (r.platform == gf::core::platform_keys::kWiiU);
  auto countAst = [&](const fs::path& root, bool recursive) -> int {
    int count = 0;
    std::error_code ec;
    if (!fs::exists(root) || !fs::is_directory(root)) return 0;
    if (recursive) {
      for (auto& e : fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec)) {
        if (ec) break;
        if (!e.is_regular_file(ec)) continue;
        auto ext = e.path().extension().string();
        for (auto& c : ext) c = (char)std::tolower((unsigned char)c);
        if (ext == ".ast" || ext == ".bgfa") count++;
      }
    } else {
      for (auto& e : fs::directory_iterator(root, fs::directory_options::skip_permission_denied, ec)) {
        if (ec) break;
        if (!e.is_regular_file(ec)) continue;
        auto ext = e.path().extension().string();
        for (auto& c : ext) c = (char)std::tolower((unsigned char)c);
        if (ext == ".ast" || ext == ".bgfa") count++;
      }
    }
    return count;
  };

  // Vita is root-only by contract (non-recursive).
  const bool vitaRootOnly = (r.platform == gf::core::platform_keys::kPsVita);
  const int astCount = countAst(scanRoot, astRecursive && !vitaRootOnly);
  r.counts.ast = astCount;

  if (astCount > 0) {
    r.primary_container = gf::models::container_type::unknown; // UI treats as AST present
  } else {
    const bool recursiveSniff = (r.platform == gf::core::platform_keys::kWiiU || r.platform == gf::core::platform_keys::kPsp);
    const auto sniff = gf::core::sniff_containers_by_header(scanRoot, recursiveSniff, 50000);
    r.counts.big = sniff.big_count;
    r.counts.terf = sniff.terf_count;
    r.counts.unknown = sniff.unknown_count;
    r.files_examined = sniff.files_examined;
    r.folders_scanned = sniff.folders_scanned;
    r.primary_container = gf::models::container_type_from_string(sniff.primary_type);
  }

  // Warnings are always based on the selected folder, not the scanRoot.
  r.warnings = gf::core::compute_soft_warnings(selected, r.platform, /*has_root_ast=*/(astCount > 0));

  const auto t1 = std::chrono::steady_clock::now();
  r.duration_ms = (std::int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

  if (has_arg(argc, argv, "--json")) {
    std::cout << gf::models::to_json(r).dump(2) << "\n";
  } else {
    std::cout << "platform: " << r.platform << "\n";
    std::cout << "scan_root: " << r.scan_root << "\n";
    std::cout << "duration_ms: " << r.duration_ms << "\n";
    if (r.counts.ast > 0) {
      std::cout << "ast_bgfa: " << r.counts.ast << "\n";
    } else {
      std::cout << "primary_format: " << gf::models::to_string(r.primary_container) << "\n";
      std::cout << "big: " << r.counts.big << "\n";
      std::cout << "terf: " << r.counts.terf << "\n";
      std::cout << "files_examined: " << r.files_examined << "\n";
      std::cout << "folders_scanned: " << r.folders_scanned << "\n";
    }
    if (!r.warnings.empty()) {
      std::cout << "warnings:\n";
      for (const auto& w : r.warnings) std::cout << "  - " << w << "\n";
    }
  }

  return 0;
}

static int cmd_ast_validate(int argc, char** argv) {
  namespace fs = std::filesystem;
  using nlohmann::json;

  const std::string pathStr = get_arg_value(argc, argv, "--path");
  if (pathStr.empty()) {
    std::cerr << "error: missing --path\n";
    print_usage();
    return 2;
  }

  const fs::path p = fs::path(pathStr);
  if (!fs::exists(p)) {
    std::cerr << "error: path does not exist\n";
    return 2;
  }

  const bool recursive = has_arg(argc, argv, "--recursive") || has_arg(argc, argv, "-r");
  std::size_t maxFiles = 5000;
  {
    const std::string maxStr = get_arg_value(argc, argv, "--max");
    if (!maxStr.empty()) {
      try {
        maxFiles = (std::size_t)std::stoull(maxStr);
      } catch (...) {
        std::cerr << "error: invalid --max value\n";
        return 2;
      }
      if (maxFiles == 0) maxFiles = 1;
    }
  }

  auto is_ast_like = [](const fs::path& fp) {
    auto ext = fp.extension().string();
    for (auto& c : ext) c = (char)std::tolower((unsigned char)c);
    return (ext == ".ast" || ext == ".bgfa");
  };

  std::vector<fs::path> files;
  std::error_code ec;

  if (fs::is_regular_file(p, ec)) {
    if (!is_ast_like(p)) {
      std::cerr << "error: file is not .ast/.bgfa\n";
      return 2;
    }
    files.push_back(p);
  } else if (fs::is_directory(p, ec)) {
    if (recursive) {
      for (auto& e : fs::recursive_directory_iterator(p, fs::directory_options::skip_permission_denied, ec)) {
        if (ec) break;
        if (!e.is_regular_file(ec)) continue;
        if (is_ast_like(e.path())) {
          files.push_back(e.path());
          if (files.size() >= maxFiles) break;
        }
      }
    } else {
      for (auto& e : fs::directory_iterator(p, fs::directory_options::skip_permission_denied, ec)) {
        if (ec) break;
        if (!e.is_regular_file(ec)) continue;
        if (is_ast_like(e.path())) {
          files.push_back(e.path());
          if (files.size() >= maxFiles) break;
        }
      }
    }
  } else {
    std::cerr << "error: path is neither file nor directory\n";
    return 2;
  }

  const auto t0 = std::chrono::steady_clock::now();

  struct item {
    std::string path;
    std::string status; // ok | warning | error
    std::string message;
    std::uint32_t entries = 0;
  };

  std::vector<item> items;
  items.reserve(files.size());

  std::size_t ok = 0, warn = 0, err = 0;
  for (const auto& fp : files) {
    item it;
    it.path = fs::absolute(fp).string();
    std::string msg;
    auto idx = gf::core::AstArchive::readIndex(fp, &msg);
    if (!idx) {
      it.status = "error";
      it.message = msg.empty() ? "failed to parse AST index" : msg;
      ++err;
    } else {
      it.entries = idx->file_count;
      if (!msg.empty()) {
        it.status = "warning";
        it.message = msg;
        ++warn;
      } else {
        it.status = "ok";
        ++ok;
      }
    }
    items.push_back(std::move(it));
  }

  const auto t1 = std::chrono::steady_clock::now();
  const auto duration_ms = (std::int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

  if (has_arg(argc, argv, "--json")) {
    json out;
    out["version"] = gf::core::kVersionString;
    out["root"] = fs::absolute(p).string();
    out["recursive"] = recursive;
    out["max"] = maxFiles;
    out["duration_ms"] = duration_ms;
    out["counts"] = json{{"ok", ok}, {"warning", warn}, {"error", err}};
    out["items"] = json::array();
    for (const auto& it : items) {
      json j;
      j["path"] = it.path;
      j["status"] = it.status;
      if (!it.message.empty()) j["message"] = it.message;
      if (it.entries) j["entries"] = it.entries;
      out["items"].push_back(j);
    }
    std::cout << out.dump(2) << "\n";
  } else {
    std::cout << "root: " << fs::absolute(p).string() << "\n";
    std::cout << "files: " << items.size() << "\n";
    std::cout << "ok: " << ok << "\n";
    std::cout << "warnings: " << warn << "\n";
    std::cout << "errors: " << err << "\n";
    std::cout << "duration_ms: " << duration_ms << "\n";
    for (const auto& it : items) {
      if (it.status == "ok") continue;
      std::cout << "- [" << it.status << "] " << it.path;
      if (!it.message.empty()) std::cout << " :: " << it.message;
      std::cout << "\n";
    }
  }

  return (err > 0) ? 1 : 0;
}

int main(int argc, char** argv) {
  gf::core::Log::init({.app_name="astra", .log_file="astra_cli.log"});

  if (argc <= 1 || has_arg(argc, argv, "--help") || has_arg(argc, argv, "-h")) {
    print_usage();
    return 0;
  }

  if (has_arg(argc, argv, "--version") || has_arg(argc, argv, "-v")) {
    std::cout << "astra " << gf::core::kVersionString << "\n";
    return 0;
  }

  const std::string sub = argv[1];
  if (sub == "scan") {
    return cmd_scan(argc, argv);
  }

  if (sub == "ast-validate" || sub == "ast_validate") {
    return cmd_ast_validate(argc, argv);
  }

  // Keep legacy behavior (config load) behind an explicit flag.
  if (sub == "config") {
    const std::string cfg_path = "config.json";
    auto cfg = gf::core::Config::load_or_default(cfg_path, true);
    gf::core::Log::get()->info("Config loaded from {}", cfg_path);
    gf::core::Log::get()->info("Workspace: {}", cfg["paths"]["workspace"].get<std::string>());
    std::cout << "astra config ok\n";
    return 0;
  }

  std::cerr << "error: unknown command '" << sub << "'\n";
  print_usage();
  return 2;
}
