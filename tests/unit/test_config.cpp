#include <catch2/catch_test_macros.hpp>
#include "gf/core/config.hpp"

#include <filesystem>

TEST_CASE("Config defaults contain required keys") {
  auto d = gf::core::Config::defaults();
  REQUIRE(d.contains("app"));
  REQUIRE(d.contains("paths"));
  REQUIRE(d["paths"].contains("workspace"));
  REQUIRE(d["paths"].contains("backups"));
}

TEST_CASE("Config load_or_default writes file when missing") {
  namespace fs = std::filesystem;
  const std::string path = "test_config.json";
  if (fs::exists(path)) fs::remove(path);

  auto cfg = gf::core::Config::load_or_default(path, true);
  REQUIRE(fs::exists(path));
  REQUIRE(cfg.contains("app"));

  fs::remove(path);
}
