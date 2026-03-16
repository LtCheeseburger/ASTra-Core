#include <catch2/catch_test_macros.hpp>

#include <gf/core/AstContainerBuilder.hpp>
#include <gf/core/AstContainerEditor.hpp>

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

static void write_bytes(const fs::path& path, const std::string& text) {
  fs::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary);
  out.write(text.data(), static_cast<std::streamsize>(text.size()));
}

static fs::path make_minimal_donor(const fs::path& donor) {
  gf::core::AstContainerEditor::Header h{};
  h.magicV = 1;
  h.fileCount = 0;
  h.fakeFileCount = 1;
  h.dirOffset = 64;
  h.dirSize = 4;
  h.nametype = 0;
  h.flagtype = 0;
  h.tagsize = 4;
  h.offsetsize = 4;
  h.compsize = 4;
  h.sizediffsize = 4;
  h.shiftsize = 4;
  h.unknownsize = 0;
  h.tagCount = 1;
  h.fileNameLength = 64;

  std::vector<std::uint8_t> donor_bytes;
  donor_bytes.insert(donor_bytes.end(), {'B','G','F','A'});
  auto push32=[&](std::uint32_t v){ for(int i=0;i<4;++i) donor_bytes.push_back(static_cast<std::uint8_t>((v>>(8*i))&0xFF)); };
  auto push64=[&](std::uint64_t v){ for(int i=0;i<8;++i) donor_bytes.push_back(static_cast<std::uint8_t>((v>>(8*i))&0xFF)); };
  push32(h.magicV); push32(h.fakeFileCount); push32(h.fileCount); push64(64); push64(4);
  donor_bytes.push_back(h.nametype); donor_bytes.push_back(h.flagtype); donor_bytes.push_back(h.tagsize); donor_bytes.push_back(h.offsetsize);
  donor_bytes.push_back(h.compsize); donor_bytes.push_back(h.sizediffsize); donor_bytes.push_back(h.shiftsize); donor_bytes.push_back(h.unknownsize);
  push32(h.tagCount); push32(h.fileNameLength);
  donor_bytes.resize(64, 0);
  push32(0);
  std::ofstream donor_out(donor, std::ios::binary);
  donor_out.write(reinterpret_cast<const char*>(donor_bytes.data()), static_cast<std::streamsize>(donor_bytes.size()));
  donor_out.close();
  return donor;
}

TEST_CASE("AstContainerBuilder builds donor-derived AST and reopens it", "[ast][builder]") {
  const fs::path temp = fs::temp_directory_path() / "astra_builder_test";
  fs::create_directories(temp);

  const fs::path donor = make_minimal_donor(temp / "donor.ast");
  const fs::path src1 = temp / "hello.txt";
  const fs::path outp = temp / "built.ast";

  write_bytes(src1, "hello builder");

  std::string err;
  gf::core::AstContainerBuilder builder;
  REQUIRE(builder.load_profile_from_donor(donor, &err));
  REQUIRE(builder.profile().has_value());
  REQUIRE(builder.profile()->requiresSyntheticEntry);

  auto entry = builder.make_entry_from_file(src1, {}, gf::core::AstBuildEntry::CompressionMode::None, &err);
  REQUIRE(entry.has_value());
  REQUIRE(builder.add_entry(std::move(*entry), &err));
  REQUIRE(builder.save_to_disk(outp, &err));

  auto reopened = gf::core::AstContainerEditor::load(outp, &err);
  REQUIRE(reopened.has_value());
  REQUIRE(reopened->entries().size() == 1);
  REQUIRE(reopened->header().fakeFileCount == reopened->header().fileCount + 1);
}

TEST_CASE("AstContainerBuilder ingests folders recursively with relative archive names", "[ast][builder][folder]") {
  const fs::path temp = fs::temp_directory_path() / "astra_builder_folder_test";
  fs::create_directories(temp);

  const fs::path donor = make_minimal_donor(temp / "donor.ast");
  const fs::path root = temp / "MyDLC";
  write_bytes(root / "uniforms" / "TEAM_A_HELMET.rsf", "rsf data");
  write_bytes(root / "config" / "teamdata.xml", "<?xml version=\"1.0\"?><root/>");

  std::string err;
  gf::core::AstContainerBuilder builder;
  REQUIRE(builder.load_profile_from_donor(donor, &err));
  std::vector<std::string> warnings;
  const auto added = builder.add_folder_entries(root, &err, &warnings);
  REQUIRE(err.empty());
  REQUIRE(added == 2);
  REQUIRE(builder.entries().size() == 2);
  REQUIRE(builder.entries()[0].archive_name == "config/teamdata.xml");
  REQUIRE(builder.entries()[1].archive_name == "uniforms/TEAM_A_HELMET.rsf");
}
