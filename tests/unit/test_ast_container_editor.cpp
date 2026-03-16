// Regression tests for AstContainerEditor save/rebuild pipeline.
//
// Coverage:
//   - round-trip: build synthetic BGFA → load → rebuild → load again (no changes)
//   - header layout: first 4 bytes 'BGFA', header is 64 bytes, dirOffset == 64
//   - directory-before-data: all entry payloads start at/after dirOffset+dirSize
//   - entry alignment: each payload is aligned to 2^shiftsize
//   - single replace + round-trip: modified entry reads back correctly
//   - zlib replace + round-trip: zlib-compressed entry recompressed correctly
//   - validate() agrees with rebuild() output
//   - validate() rejects known-bad inputs

#include <catch2/catch_test_macros.hpp>

#include <gf/core/AstContainerEditor.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <zlib.h>

using gf::core::AstContainerEditor;

// ─── Helpers ────────────────────────────────────────────────────────────────

static void writeLE4(std::vector<std::uint8_t>& v, std::uint32_t val) {
  v.push_back(static_cast<std::uint8_t>(val & 0xFF));
  v.push_back(static_cast<std::uint8_t>((val >> 8) & 0xFF));
  v.push_back(static_cast<std::uint8_t>((val >> 16) & 0xFF));
  v.push_back(static_cast<std::uint8_t>((val >> 24) & 0xFF));
}
static void writeLE8(std::vector<std::uint8_t>& v, std::uint64_t val) {
  for (int i = 0; i < 8; ++i) v.push_back(static_cast<std::uint8_t>((val >> (8*i)) & 0xFF));
}
static void pad(std::vector<std::uint8_t>& v, std::size_t n, std::uint8_t byte = 0) {
  v.insert(v.end(), n, byte);
}

// Build a minimal valid BGFA in memory.
//
// Parameters:
//   shiftsize  – alignment shift (DirOffsetMultiple = 2^shiftsize)
//   entryPayloads – raw bytes for each entry (may be empty)
//
// Returns the raw BGFA bytes.  Layout:
//   [Header 64 bytes][Directory][Data aligned to 2^shiftsize]
static std::vector<std::uint8_t> buildMinimalBgfa(
    std::uint8_t shiftsize,
    const std::vector<std::vector<std::uint8_t>>& entryPayloads)
{
  const std::uint32_t fileCount   = static_cast<std::uint32_t>(entryPayloads.size());
  const std::uint32_t fakeCount   = fileCount + 1;
  const std::uint32_t tagCount    = 1;
  const std::uint8_t  flagtype    = 0;    // flagsWidth = 1
  const std::uint8_t  tagsize     = 4;
  const std::uint8_t  offsetsize  = 4;
  const std::uint8_t  compsize    = 4;
  const std::uint8_t  sizediff    = 4;
  const std::uint8_t  unknownsize = 0;
  const std::uint32_t fnameLen    = 0;
  const std::uint8_t  flagsWidth  = 1 + flagtype;

  // per-entry record size: flags + tag + offset + comp + sizediff + fname
  const std::uint64_t recordSize = flagsWidth + tagsize + offsetsize + compsize + sizediff + unknownsize + fnameLen;
  const std::uint64_t dirSize    = static_cast<std::uint64_t>(tagCount) * 4ull + recordSize * fileCount;
  const std::uint64_t dirOffset  = 64; // always

  const std::uint64_t alignment = (shiftsize >= 63) ? 1ull : (1ull << shiftsize);

  // Pre-calculate entry offsets (after directory, aligned).
  std::vector<std::uint64_t> offsets(fileCount, 0);
  std::uint64_t cursor = dirOffset + dirSize;
  for (std::uint32_t i = 0; i < fileCount; ++i) {
    if (alignment > 1) cursor = (cursor + alignment - 1) & ~(alignment - 1);
    offsets[i] = cursor;
    cursor += entryPayloads[i].size();
  }

  std::vector<std::uint8_t> out;
  out.reserve(static_cast<std::size_t>(cursor + 64));

  // Header (64 bytes)
  out.push_back('B'); out.push_back('G'); out.push_back('F'); out.push_back('A');
  writeLE4(out, 0x00000001);        // magicV
  writeLE4(out, fakeCount);
  writeLE4(out, fileCount);
  writeLE8(out, dirOffset);         // 64
  writeLE8(out, dirSize);
  out.push_back(0);                 // nametype
  out.push_back(flagtype);
  out.push_back(tagsize);
  out.push_back(offsetsize);
  out.push_back(compsize);
  out.push_back(sizediff);
  out.push_back(shiftsize);
  out.push_back(unknownsize);
  writeLE4(out, tagCount);
  writeLE4(out, fnameLen);
  pad(out, 16);                     // 16-byte reserved padding
  // out.size() == 64

  // Directory
  writeLE4(out, 0xDEADBEEF);        // single tag

  for (std::uint32_t i = 0; i < fileCount; ++i) {
    out.push_back(0x00);            // flags (1 byte, uncompressed)
    // tag bytes (4 bytes — use the entry index as a simple tag)
    writeLE4(out, i + 1u);
    // offset (stored >> shiftsize)
    const std::uint64_t stored = (shiftsize >= 64) ? 0ull : (offsets[i] >> shiftsize);
    writeLE4(out, static_cast<std::uint32_t>(stored));
    // compressedSize
    writeLE4(out, static_cast<std::uint32_t>(entryPayloads[i].size()));
    // sizeDiff  (0 — no compression)
    writeLE4(out, 0);
  }
  // out.size() == dirOffset + dirSize == 64 + dirSize

  // Data
  for (std::uint32_t i = 0; i < fileCount; ++i) {
    // Align
    while (alignment > 1 && (out.size() % alignment) != 0) out.push_back(0);
    out.insert(out.end(), entryPayloads[i].begin(), entryPayloads[i].end());
  }

  return out;
}

// Write bytes to a temp file, return the path.
static std::filesystem::path writeTempFile(const std::vector<std::uint8_t>& bytes,
                                           const std::string& name) {
  const auto path = std::filesystem::temp_directory_path() / ("astra_test_" + name);
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  REQUIRE(f.is_open());
  f.write(reinterpret_cast<const char*>(bytes.data()),
          static_cast<std::streamsize>(bytes.size()));
  REQUIRE(f.good());
  return path;
}

// ─── Tests ──────────────────────────────────────────────────────────────────

TEST_CASE("BGFA header is exactly 64 bytes with dirOffset=64 after rebuild") {
  const std::vector<std::uint8_t> payload = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE};
  auto raw = buildMinimalBgfa(4, {payload});
  const auto path = writeTempFile(raw, "header_size.ast");

  auto edOpt = AstContainerEditor::load(path);
  REQUIRE(edOpt.has_value());

  const auto rebuilt = edOpt->rebuild();
  REQUIRE(rebuilt.size() >= 64);

  // Magic
  REQUIRE(rebuilt[0] == 'B');
  REQUIRE(rebuilt[1] == 'G');
  REQUIRE(rebuilt[2] == 'F');
  REQUIRE(rebuilt[3] == 'A');

  // Bytes 48-63 must be the 16-byte reserved padding (all zero)
  for (int i = 48; i < 64; ++i) {
    INFO("Byte " << i << " should be zero (reserved padding)");
    REQUIRE(rebuilt[i] == 0);
  }

  // dirOffset (LE uint64 at offset 16) must equal 64
  std::uint64_t dirOff = 0;
  for (int i = 0; i < 8; ++i)
    dirOff |= (static_cast<std::uint64_t>(rebuilt[16 + i]) << (8 * i));
  REQUIRE(dirOff == 64);

  std::filesystem::remove(path);
}

TEST_CASE("Round-trip: load → rebuild → load → same entry count and data") {
  std::vector<std::uint8_t> p0 = {0x01, 0x02, 0x03};
  std::vector<std::uint8_t> p1 = {0xAA, 0xBB, 0xCC, 0xDD};
  std::vector<std::uint8_t> p2 = {0xFF};
  auto raw = buildMinimalBgfa(4, {p0, p1, p2});
  const auto path = writeTempFile(raw, "roundtrip.ast");

  auto edOpt = AstContainerEditor::load(path);
  REQUIRE(edOpt.has_value());
  REQUIRE(edOpt->entries().size() == 3);

  const auto rebuilt = edOpt->rebuild();
  REQUIRE(!rebuilt.empty());

  // Write rebuilt bytes to a second temp file and reload.
  const auto path2 = writeTempFile(rebuilt, "roundtrip2.ast");
  auto ed2Opt = AstContainerEditor::load(path2);
  REQUIRE(ed2Opt.has_value());
  REQUIRE(ed2Opt->entries().size() == 3);

  // Entry data must be identical after round-trip.
  for (std::uint32_t i = 0; i < 3; ++i) {
    auto orig  = edOpt->getEntryStoredBytes(i);
    auto rtrip = ed2Opt->getEntryStoredBytes(i);
    REQUIRE(orig.has_value());
    REQUIRE(rtrip.has_value());
    REQUIRE(*orig == *rtrip);
  }

  std::filesystem::remove(path);
  std::filesystem::remove(path2);
}

TEST_CASE("All entry payloads start after the directory (not inside header)") {
  std::vector<std::uint8_t> p = {0x11, 0x22, 0x33, 0x44};
  auto raw = buildMinimalBgfa(4, {p});
  const auto path = writeTempFile(raw, "layout.ast");

  auto edOpt = AstContainerEditor::load(path);
  REQUIRE(edOpt.has_value());

  const auto rebuilt = edOpt->rebuild();
  REQUIRE(!rebuilt.empty());

  // dirOffset and dirSize from rebuilt header
  auto rdU64 = [&](std::size_t off) {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i)
      v |= (static_cast<std::uint64_t>(rebuilt[off+i]) << (8*i));
    return v;
  };
  const std::uint64_t dirOffset = rdU64(16);
  const std::uint64_t dirSize   = rdU64(24);
  REQUIRE(dirOffset == 64);

  // Every entry's dataOffset >= dirOffset + dirSize
  for (const auto& e : edOpt->entries()) {
    // Reload from rebuilt to get the new offset
  }

  // Re-load the rebuilt file to inspect updated offsets.
  const auto path2 = writeTempFile(rebuilt, "layout2.ast");
  auto ed2Opt = AstContainerEditor::load(path2);
  REQUIRE(ed2Opt.has_value());
  for (const auto& e : ed2Opt->entries()) {
    if (e.compressedSize == 0) continue;
    INFO("Entry " << e.index << " dataOffset=" << e.dataOffset
         << " must be >= dirOffset+dirSize=" << dirOffset + dirSize);
    REQUIRE(e.dataOffset >= dirOffset + dirSize);
  }

  std::filesystem::remove(path);
  std::filesystem::remove(path2);
}

TEST_CASE("Entry payloads are aligned to 2^shiftsize after rebuild") {
  // Use shiftsize=4 → alignment=16
  std::vector<std::uint8_t> p0(7,  0xAA); // 7 bytes — forces padding before next entry
  std::vector<std::uint8_t> p1(13, 0xBB);
  auto raw = buildMinimalBgfa(4, {p0, p1});
  const auto path = writeTempFile(raw, "alignment.ast");

  auto edOpt = AstContainerEditor::load(path);
  REQUIRE(edOpt.has_value());

  const auto rebuilt = edOpt->rebuild();
  const auto path2   = writeTempFile(rebuilt, "alignment2.ast");
  auto ed2Opt = AstContainerEditor::load(path2);
  REQUIRE(ed2Opt.has_value());

  constexpr std::uint64_t alignment = 16; // 1 << 4
  for (const auto& e : ed2Opt->entries()) {
    if (e.compressedSize == 0) continue;
    INFO("Entry " << e.index << " dataOffset=" << e.dataOffset << " must be 16-byte aligned");
    REQUIRE((e.dataOffset % alignment) == 0);
  }

  std::filesystem::remove(path);
  std::filesystem::remove(path2);
}

TEST_CASE("Replace entry then round-trip: modified data persists") {
  std::vector<std::uint8_t> original  = {0x01, 0x02, 0x03, 0x04};
  std::vector<std::uint8_t> replaced  = {0xDE, 0xAD, 0xBE, 0xEF, 0x42};

  auto raw = buildMinimalBgfa(4, {original});
  const auto path = writeTempFile(raw, "replace.ast");

  auto edOpt = AstContainerEditor::load(path);
  REQUIRE(edOpt.has_value());

  std::string err;
  REQUIRE(edOpt->replaceEntryBytes(0, replaced, AstContainerEditor::ReplaceMode::Raw, &err));

  const auto rebuilt = edOpt->rebuild();
  const auto path2   = writeTempFile(rebuilt, "replace2.ast");
  auto ed2Opt = AstContainerEditor::load(path2);
  REQUIRE(ed2Opt.has_value());

  const auto stored = ed2Opt->getEntryStoredBytes(0);
  REQUIRE(stored.has_value());
  REQUIRE(*stored == replaced);

  std::filesystem::remove(path);
  std::filesystem::remove(path2);
}

TEST_CASE("Replace zlib-compressed entry: PreserveZlibIfPresent recompresses") {
  // Build a payload that looks like a zlib stream.
  std::vector<std::uint8_t> original = {0xAA, 0xBB, 0xCC, 0xDD};
  // Compress it with zlib so the stored bytes look like a zlib stream.
  uLongf compLen = compressBound(static_cast<uLong>(original.size()));
  std::vector<std::uint8_t> zlibPayload(compLen);
  REQUIRE(::compress2(zlibPayload.data(), &compLen,
                       original.data(), static_cast<uLong>(original.size()),
                       Z_DEFAULT_COMPRESSION) == Z_OK);
  zlibPayload.resize(compLen);

  // Build BGFA with the compressed payload as the entry's stored bytes.
  // (We store it directly — no sizeDiff needed for this test.)
  auto raw = buildMinimalBgfa(4, {zlibPayload});
  const auto path = writeTempFile(raw, "zlib.ast");

  auto edOpt = AstContainerEditor::load(path);
  REQUIRE(edOpt.has_value());

  // Replace with new uncompressed content; PreserveZlibIfPresent should
  // re-compress because the current stored bytes look like zlib.
  const std::vector<std::uint8_t> newContent = {0x01, 0x02, 0x03, 0x04, 0x05};
  std::string err;
  REQUIRE(edOpt->replaceEntryBytes(0, newContent,
                                   AstContainerEditor::ReplaceMode::PreserveZlibIfPresent,
                                   &err));

  // The stored bytes should now be a zlib stream (first byte 0x78).
  const auto stored = edOpt->getEntryStoredBytes(0);
  REQUIRE(stored.has_value());
  REQUIRE(!stored->empty());
  REQUIRE((*stored)[0] == 0x78);

  // Inflate should give back newContent.
  const auto inflated = edOpt->getEntryInflatedBytes(0, &err);
  REQUIRE(inflated.has_value());
  REQUIRE(*inflated == newContent);

  std::filesystem::remove(path);
}

TEST_CASE("validate() accepts well-formed rebuild() output") {
  auto raw = buildMinimalBgfa(4, {{0xAA, 0xBB, 0xCC}});
  const auto path = writeTempFile(raw, "validate_ok.ast");

  auto edOpt = AstContainerEditor::load(path);
  REQUIRE(edOpt.has_value());

  const auto rebuilt = edOpt->rebuild();
  const auto span    = std::span<const std::uint8_t>(rebuilt.data(), rebuilt.size());

  std::string err;
  REQUIRE(AstContainerEditor::validate(span, &err));
  REQUIRE(err.empty());

  std::filesystem::remove(path);
}

TEST_CASE("validate() rejects truncated file") {
  std::vector<std::uint8_t> tooShort = {0x42, 0x47, 0x46, 0x41}; // "BGFA" only
  const auto span = std::span<const std::uint8_t>(tooShort.data(), tooShort.size());
  std::string err;
  REQUIRE_FALSE(AstContainerEditor::validate(span, &err));
  REQUIRE(!err.empty());
}

TEST_CASE("validate() rejects bad magic") {
  auto raw = buildMinimalBgfa(4, {});
  raw[0] = 'X'; // corrupt magic
  const auto span = std::span<const std::uint8_t>(raw.data(), raw.size());
  std::string err;
  REQUIRE_FALSE(AstContainerEditor::validate(span, &err));
  REQUIRE(!err.empty());
}

TEST_CASE("validate() rejects out-of-range entry offset") {
  auto raw = buildMinimalBgfa(4, {{0x01, 0x02}});
  const auto path = writeTempFile(raw, "validate_bad_offset.ast");

  auto edOpt = AstContainerEditor::load(path);
  REQUIRE(edOpt.has_value());

  auto rebuilt = edOpt->rebuild();
  // Corrupt the first entry's stored-offset field in the directory.
  // dirOffset=64, tagCount=1 → tags end at 64+4=68.
  // Entry 0 starts at 68: flags(1)+tag(4)+offset(4)... offset is at 68+1+4=73.
  // Set it to a huge value.
  rebuilt[73] = 0xFF;
  rebuilt[74] = 0xFF;
  rebuilt[75] = 0xFF;
  rebuilt[76] = 0xFF;

  const auto span = std::span<const std::uint8_t>(rebuilt.data(), rebuilt.size());
  std::string err;
  REQUIRE_FALSE(AstContainerEditor::validate(span, &err));
  REQUIRE(!err.empty());

  std::filesystem::remove(path);
}

TEST_CASE("Empty BGFA (zero entries) round-trips cleanly") {
  auto raw = buildMinimalBgfa(4, {});
  const auto path = writeTempFile(raw, "empty.ast");

  auto edOpt = AstContainerEditor::load(path);
  REQUIRE(edOpt.has_value());
  REQUIRE(edOpt->entries().empty());

  const auto rebuilt = edOpt->rebuild();
  REQUIRE(!rebuilt.empty());

  std::string err;
  REQUIRE(AstContainerEditor::validate(
      std::span<const std::uint8_t>(rebuilt.data(), rebuilt.size()), &err));

  const auto path2 = writeTempFile(rebuilt, "empty2.ast");
  auto ed2Opt = AstContainerEditor::load(path2);
  REQUIRE(ed2Opt.has_value());
  REQUIRE(ed2Opt->entries().empty());

  std::filesystem::remove(path);
  std::filesystem::remove(path2);
}
