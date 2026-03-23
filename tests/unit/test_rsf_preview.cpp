#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

using Catch::Approx;

#include <gf/models/rsf.hpp>
#include <gf/models/rsf_preview.hpp>

TEST_CASE("rsf preview builder creates proxy objects and maps common params", "[rsf][preview]") {
  gf::models::rsf::document doc;
  doc.textures.push_back({"diffuse", "helmet_blue.dds"});

  gf::models::rsf::material m;
  m.name = "Helmet";
  m.params.push_back({"tex", {"DiffuseTexture"}, gf::models::rsf::param_tex{0}});
  m.params.push_back({"v2", {"Position"}, gf::models::rsf::param_vec{{64.0f, 96.0f}}});
  m.params.push_back({"v2", {"Scale"}, gf::models::rsf::param_vec{{2.0f, 3.0f}}});
  m.params.push_back({"f32", {"Rotation"}, gf::models::rsf::param_f32{45.0f}});
  doc.materials.push_back(m);

  const auto preview = gf::models::rsf::build_preview_document(doc);
  REQUIRE(preview.objects.size() == 1);
  const auto& obj = preview.objects.front();
  CHECK(obj.material.texture_index.has_value());
  CHECK(*obj.material.texture_index == 0);
  CHECK(obj.transform.x == Approx(64.0f));
  CHECK(obj.transform.y == Approx(96.0f));
  CHECK(obj.transform.scale_x == Approx(2.0f));
  CHECK(obj.transform.scale_y == Approx(3.0f));
  CHECK(obj.transform.rotation_deg == Approx(45.0f));
  CHECK(obj.transform_editable);
}

TEST_CASE("rsf geometry candidate decoder safely reports missing geometry", "[rsf][preview]") {
  gf::models::rsf::document doc;
  std::vector<std::uint8_t> raw{'R','S','F',0};
  const auto result = gf::models::rsf::decode_geom_candidates(doc, raw);
  CHECK(result.geom_section_size == 0);
  CHECK_FALSE(result.warnings.empty());
}
