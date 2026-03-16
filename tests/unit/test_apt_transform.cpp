#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <gf/apt/apt_transform.hpp>

using gf::apt::Transform2D;
using Catch::Matchers::WithinAbs;

static constexpr double kEps = 1e-9;
static constexpr double kFEps = 1e-6; // looser for float-path checks

// ── identity ──────────────────────────────────────────────────────────────────

TEST_CASE("Transform2D::identity() maps points unchanged") {
  const Transform2D id = Transform2D::identity();
  REQUIRE(id.isIdentity());

  const auto [x, y] = id.mapPoint(100.0, -200.0);
  REQUIRE_THAT(x, WithinAbs(100.0,  kEps));
  REQUIRE_THAT(y, WithinAbs(-200.0, kEps));
}

// ── fromTranslation ───────────────────────────────────────────────────────────

TEST_CASE("fromTranslation shifts points correctly") {
  const Transform2D t = Transform2D::fromTranslation(30.0, -15.0);
  const auto [x, y] = t.mapPoint(10.0, 5.0);
  REQUIRE_THAT(x, WithinAbs(40.0,  kEps));
  REQUIRE_THAT(y, WithinAbs(-10.0, kEps));
}

// ── fromScale ─────────────────────────────────────────────────────────────────

TEST_CASE("fromScale scales points correctly") {
  const Transform2D s = Transform2D::fromScale(2.0, 0.5);
  const auto [x, y] = s.mapPoint(10.0, 20.0);
  REQUIRE_THAT(x, WithinAbs(20.0, kEps));
  REQUIRE_THAT(y, WithinAbs(10.0, kEps));
}

// ── composeWith: translation then scale ───────────────────────────────────────
//
// Scenario: child translates by (10,0), parent scales by (2,2).
// Expected world: point (0,0) → local translate → (10,0) → parent scale → (20,0).

TEST_CASE("composeWith: child translate + parent scale") {
  const Transform2D child  = Transform2D::fromTranslation(10.0, 0.0);
  const Transform2D parent = Transform2D::fromScale(2.0, 2.0);

  const Transform2D world = child.composeWith(parent);
  const auto [x, y] = world.mapPoint(0.0, 0.0);
  REQUIRE_THAT(x, WithinAbs(20.0, kEps));
  REQUIRE_THAT(y, WithinAbs(0.0,  kEps));
}

// ── composeWith: scale then translation ───────────────────────────────────────
//
// Child scales by (3,3), parent translates by (5,5).
// Point (2,2) → child scale → (6,6) → parent translate → (11,11).

TEST_CASE("composeWith: child scale + parent translate") {
  const Transform2D child  = Transform2D::fromScale(3.0, 3.0);
  const Transform2D parent = Transform2D::fromTranslation(5.0, 5.0);

  const Transform2D world = child.composeWith(parent);
  const auto [x, y] = world.mapPoint(2.0, 2.0);
  REQUIRE_THAT(x, WithinAbs(11.0, kEps));
  REQUIRE_THAT(y, WithinAbs(11.0, kEps));
}

// ── composeWith: three levels (grandparent → parent → child) ─────────────────
//
// Grandparent: translate (100, 0)
// Parent:      scale (2, 2)
// Child:       translate (1, 0)
// Point (0,0) → child translate → (1,0)
//             → parent scale    → (2,0)
//             → grandparent translate → (102,0)

TEST_CASE("composeWith: three-level nesting") {
  const Transform2D gp    = Transform2D::fromTranslation(100.0, 0.0);
  const Transform2D par   = Transform2D::fromScale(2.0, 2.0);
  const Transform2D child = Transform2D::fromTranslation(1.0, 0.0);

  // Build world: child → parent, then → grandparent.
  const Transform2D childWorld = child.composeWith(par);
  const Transform2D world      = childWorld.composeWith(gp);

  const auto [x, y] = world.mapPoint(0.0, 0.0);
  REQUIRE_THAT(x, WithinAbs(102.0, kEps));
  REQUIRE_THAT(y, WithinAbs(0.0,   kEps));
}

// ── composeWith: order is NOT commutative ────────────────────────────────────

TEST_CASE("composeWith is not commutative for translate+scale") {
  const Transform2D t = Transform2D::fromTranslation(10.0, 0.0);
  const Transform2D s = Transform2D::fromScale(2.0, 1.0);

  // t first then s:  point (0,0) → translate (10,0) → scale (20,0)
  const Transform2D ts = t.composeWith(s);
  const auto [x1, y1] = ts.mapPoint(0.0, 0.0);
  REQUIRE_THAT(x1, WithinAbs(20.0, kEps));

  // s first then t:  point (0,0) → scale (0,0) → translate (10,0)
  const Transform2D st = s.composeWith(t);
  const auto [x2, y2] = st.mapPoint(0.0, 0.0);
  REQUIRE_THAT(x2, WithinAbs(10.0, kEps));

  REQUIRE(x1 != x2);
}

// ── mapBounds: axis-aligned rectangle ────────────────────────────────────────

TEST_CASE("mapBounds: identity transform returns same rect") {
  const Transform2D id = Transform2D::identity();
  const auto aabb = id.mapBounds(10.0, 20.0, 50.0, 80.0);
  REQUIRE_THAT(aabb.minX, WithinAbs(10.0, kEps));
  REQUIRE_THAT(aabb.minY, WithinAbs(20.0, kEps));
  REQUIRE_THAT(aabb.maxX, WithinAbs(50.0, kEps));
  REQUIRE_THAT(aabb.maxY, WithinAbs(80.0, kEps));
}

TEST_CASE("mapBounds: scale transform scales rect") {
  const Transform2D s = Transform2D::fromScale(2.0, 3.0);
  const auto aabb = s.mapBounds(0.0, 0.0, 10.0, 5.0);
  REQUIRE_THAT(aabb.minX, WithinAbs(0.0,  kEps));
  REQUIRE_THAT(aabb.minY, WithinAbs(0.0,  kEps));
  REQUIRE_THAT(aabb.maxX, WithinAbs(20.0, kEps));
  REQUIRE_THAT(aabb.maxY, WithinAbs(15.0, kEps));
}

// ── isIdentity ────────────────────────────────────────────────────────────────

TEST_CASE("isIdentity returns false for non-identity transforms") {
  REQUIRE_FALSE(Transform2D::fromTranslation(0.001, 0.0).isIdentity());
  REQUIRE_FALSE(Transform2D::fromScale(0.999, 1.0).isIdentity());

  Transform2D t;
  t.b = 1e-10;
  REQUIRE(t.isIdentity(1e-9));   // within tolerance
  t.b = 1e-8;
  REQUIRE_FALSE(t.isIdentity(1e-9));  // outside tolerance
}

// ── operator== ───────────────────────────────────────────────────────────────

TEST_CASE("operator== compares all fields") {
  const Transform2D a = Transform2D::fromTranslation(5.0, 3.0);
  const Transform2D b = Transform2D::fromTranslation(5.0, 3.0);
  const Transform2D c = Transform2D::fromTranslation(5.0, 4.0);
  REQUIRE(a == b);
  REQUIRE_FALSE(a == c);
}
