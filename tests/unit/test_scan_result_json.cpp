#include <catch2/catch_test_macros.hpp>

#include "gf/models/scan_result.hpp"

TEST_CASE("scan_result JSON roundtrip preserves key fields") {
  gf::models::scan_result r;
  r.platform = "psp";
  r.scan_root = "X:/PSP_GAME/USRDIR";
  r.counts.ast = 0;
  r.counts.big = 3;
  r.counts.terf = 7;
  r.files_examined = 1234;
  r.folders_scanned = 12;
  r.duration_ms = 45;
  r.primary_container = gf::models::container_type::terf;
  r.warnings = {"warning a", "warning b"};

  auto j = gf::models::to_json(r);
  auto r2 = gf::models::scan_result_from_json(j);

  REQUIRE(r2.platform == r.platform);
  REQUIRE(r2.scan_root == r.scan_root);
  REQUIRE(r2.counts.big == r.counts.big);
  REQUIRE(r2.counts.terf == r.counts.terf);
  REQUIRE(r2.files_examined == r.files_examined);
  REQUIRE(r2.folders_scanned == r.folders_scanned);
  REQUIRE(r2.duration_ms == r.duration_ms);
  REQUIRE(r2.primary_container == r.primary_container);
  REQUIRE(r2.warnings.size() == r.warnings.size());
}
