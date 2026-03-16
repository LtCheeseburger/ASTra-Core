#include <catch2/catch_test_macros.hpp>
#include <gf/apt/apt_reader.hpp>
#include <gf/apt/apt_renderer.hpp>

TEST_CASE("build_display_list_frame applies place, move, and remove cumulatively") {
  using namespace gf::apt;

  std::vector<AptFrame> frames(3);
  for (std::size_t i = 0; i < frames.size(); ++i) frames[i].index = static_cast<std::uint32_t>(i);

  AptFrameItem place0;
  place0.kind = AptFrameItemKind::PlaceObject;
  place0.placement.depth = 5;
  place0.placement.character = 7;
  place0.placement.flags = 0x2u | 0x4u; // character + matrix
  place0.placement.transform.x = 10.0;
  place0.placement.transform.y = 20.0;
  frames[0].items.push_back(place0);
  frames[0].placements.push_back(place0.placement);

  AptFrameItem move1;
  move1.kind = AptFrameItemKind::PlaceObject;
  move1.placement.depth = 5;
  move1.placement.flags = 0x1u | 0x4u; // move existing + matrix
  move1.placement.transform.x = 30.0;
  move1.placement.transform.y = 40.0;
  frames[1].items.push_back(move1);
  frames[1].placements.push_back(move1.placement);

  AptFrameItem remove2;
  remove2.kind = AptFrameItemKind::RemoveObject;
  remove2.remove_depth = 5;
  frames[2].items.push_back(remove2);

  const AptFrame resolved1 = build_display_list_frame(frames, 1);
  REQUIRE(resolved1.placements.size() == 1);
  REQUIRE(resolved1.placements[0].character == 7);
  REQUIRE(resolved1.placements[0].depth == 5);
  REQUIRE(resolved1.placements[0].transform.x == 30.0);
  REQUIRE(resolved1.placements[0].transform.y == 40.0);

  const AptFrame resolved2 = build_display_list_frame(frames, 2);
  REQUIRE(resolved2.placements.empty());
}

TEST_CASE("renderAptTimelineFrame uses cumulative display-list state") {
  using namespace gf::apt;

  AptFile file;
  file.characters.resize(3);
  file.characters[1].type = 1;
  file.characters[1].bounds = AptBounds{0.0f, 0.0f, 50.0f, 20.0f};
  file.characters[2].type = 1;
  file.characters[2].bounds = AptBounds{0.0f, 0.0f, 80.0f, 30.0f};

  std::vector<AptFrame> frames(2);
  for (std::size_t i = 0; i < frames.size(); ++i) frames[i].index = static_cast<std::uint32_t>(i);

  AptFrameItem place;
  place.kind = AptFrameItemKind::PlaceObject;
  place.placement.depth = 10;
  place.placement.character = 1;
  place.placement.flags = 0x2u | 0x4u;
  frames[0].items.push_back(place);
  frames[0].placements.push_back(place.placement);

  AptFrameItem replace;
  replace.kind = AptFrameItemKind::PlaceObject;
  replace.placement.depth = 10;
  replace.placement.character = 2;
  replace.placement.flags = 0x1u | 0x2u | 0x4u;
  frames[1].items.push_back(replace);
  frames[1].placements.push_back(replace.placement);

  const auto nodes0 = renderAptTimelineFrame(file, frames, 0);
  REQUIRE(nodes0.size() == 1);
  REQUIRE(nodes0[0].characterId == 1);

  const auto nodes1 = renderAptTimelineFrame(file, frames, 1);
  REQUIRE(nodes1.size() == 1);
  REQUIRE(nodes1[0].characterId == 2);
}
