#include <catch2/catch_test_macros.hpp>
#include "gf_core/version.hpp"

TEST_CASE("Version string is non-empty") {
  REQUIRE(gf::core::kVersionMajor >= 0);
  REQUIRE(gf::core::kVersionMinor >= 0);
  REQUIRE(gf::core::kVersionPatch >= 0);
  REQUIRE(gf::core::kVersionString != nullptr);
  REQUIRE(std::string(gf::core::kVersionString).size() > 0);
}
