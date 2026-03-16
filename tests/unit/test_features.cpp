#include <catch2/catch_test_macros.hpp>

#include "gf/core/features.hpp"

TEST_CASE("Feature gates are disabled by default in v0.5.x") {
  using gf::core::Feature;
  using gf::core::feature_enabled;

  REQUIRE(feature_enabled(Feature::AstEditor) == false);
  REQUIRE(feature_enabled(Feature::RsfConfigViewer) == false);
  REQUIRE(feature_enabled(Feature::TexturePreview) == false);
  REQUIRE(feature_enabled(Feature::AptEditor) == false);
  REQUIRE(feature_enabled(Feature::AptLiveEditing) == false);
}
