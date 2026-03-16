#include <catch2/catch_test_macros.hpp>

#include "gf/core/scan.hpp"

#include <filesystem>
#include <fstream>
#include <vector>

static void write_file(const std::filesystem::path& p, const std::vector<char>& bytes) {
  std::ofstream f(p, std::ios::binary);
  REQUIRE((bool)f);
  f.write(bytes.data(), (std::streamsize)bytes.size());
}

TEST_CASE("sniff_containers_by_header detects BIG and TERF") {
  namespace fs = std::filesystem;

  const fs::path tmp = fs::temp_directory_path() / "astra_test_sniff";
  std::error_code ec;
  fs::remove_all(tmp, ec);
  fs::create_directories(tmp / "DATA");

  write_file(tmp / "a.bin", {'B','I','G','4'});
  write_file(tmp / "b.bin", {'T','E','R','F'});
  write_file(tmp / "DATA" / "c.bin", {'T','E','R','F'});

  auto c = gf::core::sniff_containers_by_header(tmp, /*recursive=*/false, /*max_files=*/1000);
  REQUIRE(c.big_count == 1);
  REQUIRE(c.terf_count == 2);
  REQUIRE((c.primary_type == "TERF" || c.primary_type == "BIG"));

  fs::remove_all(tmp, ec);
}

TEST_CASE("compute_soft_warnings triggers common mistakes") {
  namespace fs = std::filesystem;
  std::error_code ec;

  // USRDIR selection warning
  const fs::path usrdir = fs::temp_directory_path() / "astra_test_USRDIR" / "USRDIR";
  fs::remove_all(usrdir.parent_path(), ec);
  fs::create_directories(usrdir);
  auto w1 = gf::core::compute_soft_warnings(usrdir, "ps3", /*has_root_ast=*/true);
  REQUIRE(!w1.empty());

  // PSP missing PSP_GAME warning
  const fs::path psp = fs::temp_directory_path() / "astra_test_psp_missing";
  fs::remove_all(psp, ec);
  fs::create_directories(psp);
  auto w2 = gf::core::compute_soft_warnings(psp, "psp", /*has_root_ast=*/true);
  REQUIRE(!w2.empty());

  // Vita no root AST warning
  const fs::path vita = fs::temp_directory_path() / "astra_test_vita";
  fs::remove_all(vita, ec);
  fs::create_directories(vita);
  // Add eboot marker so it looks like Vita
  write_file(vita / "eboot.bin", {'0','0','0','0'});
  auto w3 = gf::core::compute_soft_warnings(vita, "psvita", /*has_root_ast=*/false);
  REQUIRE(!w3.empty());

  fs::remove_all(usrdir.parent_path(), ec);
  fs::remove_all(psp, ec);
  fs::remove_all(vita, ec);
}
