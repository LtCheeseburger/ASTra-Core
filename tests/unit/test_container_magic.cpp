#include <catch2/catch_test_macros.hpp>

#include "gf/core/container_magic.hpp"

TEST_CASE("Container magic classification") {
  using gf::core::ContainerMagicType;
  using gf::core::classify_container_magic;

  REQUIRE(classify_container_magic({'B','I','G','F'}) == ContainerMagicType::Big);
  REQUIRE(classify_container_magic({'B','I','G','4'}) == ContainerMagicType::Big);
  REQUIRE(classify_container_magic({'B','I','G',' '}) == ContainerMagicType::Big);
  REQUIRE(classify_container_magic({'T','E','R','F'}) == ContainerMagicType::Terf);
  REQUIRE(classify_container_magic({'D','D','S',' '}) == ContainerMagicType::Unknown);
}
