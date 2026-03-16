#include <gf/apt/apt_reader.hpp>

#include <filesystem>
#include <algorithm>
#include <fstream>
#include <string_view>
#include <cstring>
#include <unordered_map>

namespace gf::apt {

static bool read_file(const std::filesystem::path& p, std::vector<std::uint8_t>* out, std::string* err) {
  std::ifstream f(p, std::ios::binary);
  if (!f) {
    if (err) *err = "Failed to open file: " + p.string();
    return false;
  }
  f.seekg(0, std::ios::end);
  const auto sz = f.tellg();
  if (sz < 0) {
    if (err) *err = "Failed to stat file: " + p.string();
    return false;
  }
  out->resize(static_cast<std::size_t>(sz));
  f.seekg(0, std::ios::beg);
  if (!out->empty()) f.read(reinterpret_cast<char*>(out->data()), static_cast<std::streamsize>(out->size()));
  return true;
}

static std::uint32_t read_u32be(const std::uint8_t* p) {
  return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 8) | (std::uint32_t(p[3]));
}

static std::int32_t read_i32be(const std::uint8_t* p) {
  return static_cast<std::int32_t>(read_u32be(p));
}

static float read_f32be(const std::uint8_t* p) {
  const std::uint32_t bits = read_u32be(p);
  float out = 0.0f;
  std::memcpy(&out, &bits, sizeof(out));
  return out;
}

static bool can_read(const std::vector<std::uint8_t>& b, std::uint64_t off, std::uint64_t n) {
  return off <= b.size() && n <= b.size() && (off + n) <= b.size();
}

static std::string read_cstr(const std::vector<std::uint8_t>& b, std::uint64_t off) {
  if (off >= b.size()) return {};
  const char* s = reinterpret_cast<const char*>(b.data() + off);
  // Read until NUL or end; keep it bounded.
  std::uint64_t i = off;
  const std::uint64_t maxLen = 4096;
  while (i < b.size() && (i - off) < maxLen) {
    if (b[static_cast<std::size_t>(i)] == 0) break;
    ++i;
  }
  return std::string(s, reinterpret_cast<const char*>(b.data() + i));
}

static std::uint64_t file_size_u64(const std::filesystem::path& p, std::string* err) {
  std::error_code ec;
  auto sz = std::filesystem::file_size(p, ec);
  if (ec) {
    if (err) *err = "Failed to stat file: " + p.string();
    return 0;
  }
  return static_cast<std::uint64_t>(sz);
}


static AptPlacement parse_place_object(const std::vector<std::uint8_t>& aptBuf,
                                      std::uint64_t itemPtr) {
  AptPlacement placement;
  placement.offset = itemPtr;
  placement.flags = read_u32be(aptBuf.data() + itemPtr + 4);
  placement.depth = static_cast<std::uint32_t>(read_i32be(aptBuf.data() + itemPtr + 8));

  const bool hasCharacter = (placement.flags & 0x2) != 0;
  const bool hasMatrix    = (placement.flags & 0x4) != 0;

  if (hasCharacter) placement.character = static_cast<std::uint32_t>(read_i32be(aptBuf.data() + itemPtr + 12));
  else placement.character = 0xFFFFFFFFu;

  if (hasMatrix) {
    placement.transform.scale_x       = static_cast<double>(read_f32be(aptBuf.data() + itemPtr + 16));
    placement.transform.rotate_skew_0 = static_cast<double>(read_f32be(aptBuf.data() + itemPtr + 20));
    placement.transform.rotate_skew_1 = static_cast<double>(read_f32be(aptBuf.data() + itemPtr + 24));
    placement.transform.scale_y       = static_cast<double>(read_f32be(aptBuf.data() + itemPtr + 28));
    placement.transform.x             = static_cast<double>(read_f32be(aptBuf.data() + itemPtr + 32));
    placement.transform.y             = static_cast<double>(read_f32be(aptBuf.data() + itemPtr + 36));
  }

  const std::uint32_t color = read_u32be(aptBuf.data() + itemPtr + 40);
  placement.color_transform.add_r = static_cast<double>((color >> 24) & 0xFF);
  placement.color_transform.add_g = static_cast<double>((color >> 16) & 0xFF);
  placement.color_transform.add_b = static_cast<double>((color >> 8) & 0xFF);
  placement.color_transform.add_a = static_cast<double>(color & 0xFF);

  const std::uint32_t namePtr = read_u32be(aptBuf.data() + itemPtr + 52);
  if (namePtr && namePtr < aptBuf.size()) placement.instance_name = read_cstr(aptBuf, namePtr);
  return placement;
}

static void parse_frame_items(const std::vector<std::uint8_t>& aptBuf,
                              std::uint64_t itemsOffset,
                              std::uint32_t itemCount,
                              AptFrame* outFrame) {
  if (!outFrame || !itemsOffset || !itemCount) return;
  if (!can_read(aptBuf, itemsOffset, std::uint64_t(itemCount) * 4)) return;

  constexpr std::uint32_t kFrameItemAction          = 1;
  constexpr std::uint32_t kFrameItemFrameLabel      = 2;
  constexpr std::uint32_t kFrameItemPlaceObject     = 3;
  constexpr std::uint32_t kFrameItemRemoveObject    = 4;
  constexpr std::uint32_t kFrameItemBackgroundColor = 5;
  constexpr std::uint32_t kFrameItemInitAction      = 8;
  constexpr std::uint64_t kOutputPlaceObjectSize    = 64;

  for (std::uint32_t i = 0; i < itemCount; ++i) {
    const std::uint64_t ptrOff = itemsOffset + std::uint64_t(i) * 4;
    const std::uint32_t itemPtr = read_u32be(aptBuf.data() + ptrOff);
    if (!itemPtr || !can_read(aptBuf, itemPtr, 4)) continue;

    AptFrameItem item;
    item.offset = itemPtr;
    const std::uint32_t itemType = read_u32be(aptBuf.data() + itemPtr + 0);
    item.kind = static_cast<AptFrameItemKind>(itemType);

    switch (itemType) {
      case kFrameItemPlaceObject:
        if (can_read(aptBuf, itemPtr, kOutputPlaceObjectSize)) {
          item.placement = parse_place_object(aptBuf, itemPtr);
          outFrame->placements.push_back(item.placement);
        }
        break;
      case kFrameItemRemoveObject:
        if (can_read(aptBuf, itemPtr, 8)) item.remove_depth = static_cast<std::uint32_t>(read_i32be(aptBuf.data() + itemPtr + 4));
        break;
      case kFrameItemFrameLabel: {
        if (can_read(aptBuf, itemPtr, 8)) {
          const std::uint32_t strPtr = read_u32be(aptBuf.data() + itemPtr + 4);
          if (strPtr && strPtr < aptBuf.size()) item.label = read_cstr(aptBuf, strPtr);
        }
        break;
      }
      case kFrameItemBackgroundColor:
        if (can_read(aptBuf, itemPtr, 8)) item.background_rgba = read_u32be(aptBuf.data() + itemPtr + 4);
        break;
      case kFrameItemInitAction: {
        if (can_read(aptBuf, itemPtr, 8)) item.init_sprite = read_u32be(aptBuf.data() + itemPtr + 4);
        if (can_read(aptBuf, itemPtr, 12)) {
          const std::uint32_t actPtr = read_u32be(aptBuf.data() + itemPtr + 8);
          if (actPtr && actPtr < aptBuf.size()) item.action_bytes_offset = actPtr;
        }
        break;
      }
      case kFrameItemAction: {
        if (can_read(aptBuf, itemPtr, 8)) {
          const std::uint32_t actPtr = read_u32be(aptBuf.data() + itemPtr + 4);
          if (actPtr && actPtr < aptBuf.size()) item.action_bytes_offset = actPtr;
        }
        break;
      }
      default:
        break;
    }

    outFrame->items.push_back(std::move(item));
  }
}

// Forward declaration — defined after parse_nested_character_frames.
static void parse_character_table_into(const std::vector<std::uint8_t>& aptBuf,
                                       std::uint64_t charsOff,
                                       std::uint32_t characterCount,
                                       std::vector<AptCharacter>& out,
                                       int recursionDepth);

// Binary layout offsets per character type:
//
//  Sprite (type 5):  OutputSprite : Character
//    +0  type, +4 sig, +8 framecount, +12 frames*
//
//  Movie (type 9):   OutputMovie : Character
//    +0  type, +4 sig, +8 unk2, +12 unk3
//    +16 framecount, +20 frames*, +24 ptr(runtime)
//    +28 charactercount, +32 characters*
//    +36 screensizex, +40 screensizey
static void parse_nested_character_frames(const std::vector<std::uint8_t>& aptBuf,
                                          AptCharacter* ch,
                                          int recursionDepth = 0) {
  if (!ch) return;
  if (ch->type != 5 && ch->type != 9) return;

  const std::uint64_t base = ch->offset;

  // Choose frame-table offsets based on character type.
  std::uint64_t framecountOff, framesOff_field;
  if (ch->type == 5) {           // Sprite: framecount at +8, frames* at +12
    framecountOff = base + 8;
    framesOff_field = base + 12;
  } else {                       // Movie:  framecount at +16, frames* at +20
    framecountOff = base + 16;
    framesOff_field = base + 20;
  }

  if (!can_read(aptBuf, framecountOff, 8)) return;
  const std::uint32_t frameCount = read_u32be(aptBuf.data() + framecountOff);
  const std::uint32_t framesOff  = read_u32be(aptBuf.data() + framesOff_field);

  if (frameCount && framesOff &&
      can_read(aptBuf, framesOff, std::uint64_t(frameCount) * 8)) {
    ch->frames.clear();
    ch->frames.reserve(frameCount);
    for (std::uint32_t i = 0; i < frameCount; ++i) {
      const std::uint64_t fo = framesOff + std::uint64_t(i) * 8;
      AptFrame fr;
      fr.index = i;
      fr.offset = fo;
      fr.frameitemcount = read_u32be(aptBuf.data() + fo + 0);
      fr.items_offset   = read_u32be(aptBuf.data() + fo + 4);
      parse_frame_items(aptBuf, fr.items_offset, fr.frameitemcount, &fr);
      ch->frames.push_back(std::move(fr));
    }
  }

  // For Movie characters, also parse their own character pointer table so the
  // renderer can look up characters placed within the movie's frames.
  if (ch->type == 9 && recursionDepth < 3) {
    if (can_read(aptBuf, base + 28, 8)) {
      const std::uint32_t charCount = read_u32be(aptBuf.data() + base + 28);
      const std::uint32_t charTableOff = read_u32be(aptBuf.data() + base + 32);
      if (charCount && charTableOff) {
        ch->nested_characters.clear();
        parse_character_table_into(aptBuf, charTableOff, charCount,
                                   ch->nested_characters, recursionDepth + 1);
      }
    }
  }
}

// Parses the character pointer table at charsOff into `out`, index-aligned.
// recursionDepth limits how deeply we recurse into nested movies.
static void parse_character_table_into(const std::vector<std::uint8_t>& aptBuf,
                                       std::uint64_t charsOff,
                                       std::uint32_t characterCount,
                                       std::vector<AptCharacter>& out,
                                       int recursionDepth) {
  if (!characterCount || !charsOff) return;
  if (!can_read(aptBuf, charsOff, std::uint64_t(characterCount) * 4)) return;

  out.reserve(characterCount);
  for (std::uint32_t i = 0; i < characterCount; ++i) {
    const std::uint64_t po  = charsOff + std::uint64_t(i) * 4;
    const std::uint32_t chOff = read_u32be(aptBuf.data() + po);

    if (!chOff || !can_read(aptBuf, chOff, 8)) {
      AptCharacter dummy;
      dummy.index = i;
      dummy.id    = i;
      out.push_back(dummy);
      continue;
    }

    AptCharacter ch;
    ch.index = i;
    ch.id    = i;
    ch.offset = chOff;
    ch.type      = read_u32be(aptBuf.data() + chOff + 0);
    ch.signature = read_u32be(aptBuf.data() + chOff + 4);

    // Parse bounds for types that carry a Rect after the base header.
    {
      std::uint64_t boundsOff = 0;
      if (ch.type == 1 || ch.type == 2 || ch.type == 10) boundsOff = chOff + 8;
      else if (ch.type == 4)                              boundsOff = chOff + 12;
      if (boundsOff && can_read(aptBuf, boundsOff, 16)) {
        AptBounds b;
        b.left   = read_f32be(aptBuf.data() + boundsOff +  0);
        b.top    = read_f32be(aptBuf.data() + boundsOff +  4);
        b.right  = read_f32be(aptBuf.data() + boundsOff +  8);
        b.bottom = read_f32be(aptBuf.data() + boundsOff + 12);
        ch.bounds = b;
      }
    }

    parse_nested_character_frames(aptBuf, &ch, recursionDepth);
    out.push_back(std::move(ch));
  }
}

// rebuild_binary() — in-place patch model
//
// SAFE to edit and round-trip (fields patched directly into original_apt):
//   AptSummary::screensizex/screensizey          (OutputMovie header +36/+40)
//   AptPlacement::depth                          (PlaceObject +8)
//   AptPlacement::character                      (PlaceObject +12)
//   AptPlacement::flags                          (PlaceObject +4)
//   AptPlacement::transform (scale_x/y, rotate_skew_0/1, x, y)  (+16..+36)
//
// NOT patched — in-memory changes do NOT persist to the saved file:
//   AptPlacement::instance_name  — pointer-backed string, cannot relocate in-place
//   AptImport::movie / AptImport::name  — same: pointer-backed
//   AptExport::name              — same: pointer-backed
//   Frame labels / FrameLabel items    — string data not relocated
//   Action bytecode bytes              — read-only, never touched
//   Character type / bounds / signature — structural, not editable
//
// The buffer is never resized; only values at existing AptPlacement::offset
// absolute positions are overwritten.
std::vector<std::uint8_t> AptFile::rebuild_binary() const {
  std::vector<std::uint8_t> buf = original_apt;
  if (buf.empty()) return buf;

  // Write a big-endian u32 at byte offset off.
  auto write_u32be = [&](std::uint64_t off, std::uint32_t v) {
    if (off + 4 > buf.size()) return;
    buf[off + 0] = static_cast<std::uint8_t>((v >> 24) & 0xFF);
    buf[off + 1] = static_cast<std::uint8_t>((v >> 16) & 0xFF);
    buf[off + 2] = static_cast<std::uint8_t>((v >>  8) & 0xFF);
    buf[off + 3] = static_cast<std::uint8_t>( v        & 0xFF);
  };
  auto write_i32be = [&](std::uint64_t off, std::int32_t v) {
    write_u32be(off, static_cast<std::uint32_t>(v));
  };
  auto write_f32be = [&](std::uint64_t off, float v) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &v, sizeof(bits));
    write_u32be(off, bits);
  };

  // Patch OutputMovie header: screen dimensions at fixed relative offsets.
  // OutputMovie layout (u32 big-endian): type(0) sig(4) unk(8) unk(12)
  //   framecount(16) framesOff(20) unk(24) charcount(28) charsOff(32)
  //   sx(36) sy(40) unk(44) importcount(48) importsOff(52)
  //   exportcount(56) exportsOff(60)
  const std::uint64_t movieOff = summary.aptdataoffset;
  if (movieOff + 17 * 4 <= buf.size()) {
    write_u32be(movieOff + 36, summary.screensizex);
    write_u32be(movieOff + 40, summary.screensizey);
  }

  // Patch each PlaceObject record in-place using the stored absolute offset.
  // Offsets within the 64-byte OutputPlaceObject structure:
  //  +0  type (u32)  — not patched (structural)
  //  +4  flags (u32)
  //  +8  depth (i32)
  //  +12 character (i32)
  //  +16 matrix a = scale_x      (f32)
  //  +20 matrix b = rotate_skew_0 (f32)
  //  +24 matrix c = rotate_skew_1 (f32)
  //  +28 matrix d = scale_y      (f32)
  //  +32 tx = x                  (f32)
  //  +36 ty = y                  (f32)
  //  +40..+63 color/name data — not patched here
  auto patchPlacement = [&](const AptPlacement& pl) {
    const std::uint64_t base = pl.offset;
    if (base == 0 || base + 64 > buf.size()) return;
    write_u32be(base +  4, pl.flags);
    write_i32be(base +  8, static_cast<std::int32_t>(pl.depth));
    write_i32be(base + 12, static_cast<std::int32_t>(pl.character));
    write_f32be(base + 16, static_cast<float>(pl.transform.scale_x));
    write_f32be(base + 20, static_cast<float>(pl.transform.rotate_skew_0));
    write_f32be(base + 24, static_cast<float>(pl.transform.rotate_skew_1));
    write_f32be(base + 28, static_cast<float>(pl.transform.scale_y));
    write_f32be(base + 32, static_cast<float>(pl.transform.x));
    write_f32be(base + 36, static_cast<float>(pl.transform.y));
  };

  for (const auto& fr : frames)
    for (const auto& pl : fr.placements)
      patchPlacement(pl);

  for (const auto& ch : characters)
    for (const auto& fr : ch.frames)
      for (const auto& pl : fr.placements)
        patchPlacement(pl);

  return buf;
}

std::optional<AptFile> read_apt_file(const std::string& apt_path,
                                    const std::string& const_path,
                                    std::string* err) {
  const std::filesystem::path aptp(apt_path);
  const std::filesystem::path constp(const_path);

  if (!std::filesystem::exists(aptp)) {
    if (err) *err = "APT not found: " + aptp.string();
    return std::nullopt;
  }
  if (!std::filesystem::exists(constp)) {
    if (err) *err = "CONST not found: " + constp.string();
    return std::nullopt;
  }

  // Basic sanity: can open both.
  {
    std::ifstream f(aptp, std::ios::binary);
    if (!f) {
      if (err) *err = "Failed to open APT: " + aptp.string();
      return std::nullopt;
    }
  }
  {
    std::ifstream f(constp, std::ios::binary);
    if (!f) {
      if (err) *err = "Failed to open CONST: " + constp.string();
      return std::nullopt;
    }
  }

  AptFile out;

  const std::uint64_t aptSize = file_size_u64(aptp, err);
  (void)file_size_u64(constp, err);

  std::vector<std::uint8_t> aptBuf;
  std::vector<std::uint8_t> constBuf;
  if (!read_file(aptp, &aptBuf, err)) return std::nullopt;
  if (!read_file(constp, &constBuf, err)) return std::nullopt;

  // Parse CONST header (big-endian) per AptConverter:
  // skip 0x14, then aptdataoffset (u32), itemcount (u32), skip 4.
  if (constBuf.size() >= 0x14 + 12) {
    const std::uint64_t o = 0x14;
    out.summary.aptdataoffset = read_u32be(constBuf.data() + o);
  }

  // Parse OutputMovie header at aptdataoffset (big-endian fields)
  // Layout (u32): type, signature, unknown2, unknown3, framecount, framesOff,
  // pointer, charcount, charactersOff, sx, sy, unknown, importcount, importsOff,
  // exportcount, exportsOff, count.
  const std::uint64_t movieOff = out.summary.aptdataoffset;
  if (can_read(aptBuf, movieOff, 17 * 4)) {
    const std::uint8_t* p = aptBuf.data() + movieOff;
    // Skip type/signature
    (void)read_u32be(p + 0);
    (void)read_u32be(p + 4);
    // unknown2/unknown3
    (void)read_u32be(p + 8);
    (void)read_u32be(p + 12);
    out.summary.framecount = read_u32be(p + 16);
    const std::uint32_t framesOff = read_u32be(p + 20);
    out.summary.charactercount = read_u32be(p + 28);
    const std::uint32_t charsOff = read_u32be(p + 32);
    out.summary.screensizex = read_u32be(p + 36);
    out.summary.screensizey = read_u32be(p + 40);
    out.summary.importcount = read_u32be(p + 48);
    const std::uint32_t importsOff = read_u32be(p + 52);
    out.summary.exportcount = read_u32be(p + 56);
    const std::uint32_t exportsOff = read_u32be(p + 60);

    // Slices for hex preview
    out.slices.push_back({"Header", 0, (aptSize >= 64 ? 64 : aptSize)});
    out.slices.push_back({"Movie Header", movieOff, 17 * 4});
    if (importsOff && can_read(aptBuf, importsOff, std::uint64_t(out.summary.importcount) * 16))
      out.slices.push_back({"Imports Table", importsOff, std::uint64_t(out.summary.importcount) * 16});
    if (exportsOff && can_read(aptBuf, exportsOff, std::uint64_t(out.summary.exportcount) * 8))
      out.slices.push_back({"Exports Table", exportsOff, std::uint64_t(out.summary.exportcount) * 8});
    if (framesOff && can_read(aptBuf, framesOff, std::uint64_t(out.summary.framecount) * 8))
      out.slices.push_back({"Frames Table", framesOff, std::uint64_t(out.summary.framecount) * 8});
    if (charsOff && can_read(aptBuf, charsOff, std::uint64_t(out.summary.charactercount) * 4))
      out.slices.push_back({"Characters Ptr Table", charsOff, std::uint64_t(out.summary.charactercount) * 4});

    // Parse Imports (best-effort)
    if (importsOff && can_read(aptBuf, importsOff, std::uint64_t(out.summary.importcount) * 16)) {
      for (std::uint32_t i = 0; i < out.summary.importcount; ++i) {
        const std::uint64_t io = importsOff + std::uint64_t(i) * 16;
        const std::uint32_t moviePtr = read_u32be(aptBuf.data() + io + 0);
        const std::uint32_t namePtr = read_u32be(aptBuf.data() + io + 4);
        const std::uint32_t ch = read_u32be(aptBuf.data() + io + 8);
        AptImport imp;
        imp.offset = io;
        imp.character = ch;
        imp.movie = read_cstr(aptBuf, moviePtr);
        imp.name = read_cstr(aptBuf, namePtr);
        out.imports.push_back(std::move(imp));
      }
    }

    // Parse Exports (best-effort)
    if (exportsOff && can_read(aptBuf, exportsOff, std::uint64_t(out.summary.exportcount) * 8)) {
      for (std::uint32_t i = 0; i < out.summary.exportcount; ++i) {
        const std::uint64_t eo = exportsOff + std::uint64_t(i) * 8;
        const std::uint32_t namePtr = read_u32be(aptBuf.data() + eo + 0);
        const std::uint32_t ch = read_u32be(aptBuf.data() + eo + 4);
        AptExport ex;
        ex.offset = eo;
        ex.character = ch;
        ex.name = read_cstr(aptBuf, namePtr);
        out.exports.push_back(std::move(ex));
      }
    }

    // Parse Frames (OutputFrame table)
    if (framesOff && can_read(aptBuf, framesOff, std::uint64_t(out.summary.framecount) * 8)) {
      for (std::uint32_t i = 0; i < out.summary.framecount; ++i) {
        const std::uint64_t fo = framesOff + std::uint64_t(i) * 8;
        AptFrame fr;
        fr.index = i;
        fr.offset = fo;
        fr.frameitemcount = read_u32be(aptBuf.data() + fo + 0);
        fr.items_offset = read_u32be(aptBuf.data() + fo + 4);
        parse_frame_items(aptBuf, fr.items_offset, fr.frameitemcount, &fr);
        out.frames.push_back(fr);
      }
    }

    // Parse the root movie's character pointer table using the shared helper.
    // The helper keeps the vector index-aligned with the binary table (type=0
    // dummy entries for null/import-reserved slots) and recursively parses
    // nested Sprite/Movie characters, including their own character tables.
    parse_character_table_into(aptBuf, charsOff, out.summary.charactercount,
                               out.characters, 0);
  } else {
    // Fallback slices
    out.slices.push_back({"Header", 0, (aptSize >= 64 ? 64 : aptSize)});
    out.slices.push_back({"APT File", 0, aptSize});
  }

  out.original_apt = std::move(aptBuf);
  out.original_const = std::move(constBuf);

  // Always provide full file slice
  out.slices.push_back({"APT File", 0, aptSize});
  return out;
}


AptFrame build_display_list_frame(const std::vector<AptFrame>& timeline_frames,
                                 std::size_t frame_index) {
  AptFrame resolved;
  if (timeline_frames.empty()) return resolved;
  if (frame_index >= timeline_frames.size()) frame_index = timeline_frames.size() - 1;

  std::unordered_map<std::uint32_t, AptPlacement> display_list;
  for (std::size_t fi = 0; fi <= frame_index; ++fi) {
    const AptFrame& src = timeline_frames[fi];
    for (const auto& item : src.items) {
      if (item.kind == AptFrameItemKind::RemoveObject) {
        display_list.erase(item.remove_depth);
        continue;
      }
      if (item.kind != AptFrameItemKind::PlaceObject) continue;

      const AptPlacement& pl = item.placement;
      auto& slot = display_list[pl.depth];
      const bool hasMove      = (pl.flags & 0x1u) != 0;
      const bool hasCharacter = (pl.flags & 0x2u) != 0;
      const bool hasMatrix    = (pl.flags & 0x4u) != 0;
      const bool hasName      = (pl.flags & 0x20u) != 0;

      if (!hasMove || !slot.offset) {
        slot = pl;
        if (!hasCharacter) slot.character = 0xFFFFFFFFu;
        continue;
      }

      // Flash-style move/update at an existing depth slot.
      slot.flags = pl.flags;
      slot.depth = pl.depth;
      slot.offset = pl.offset;
      if (hasCharacter) slot.character = pl.character;
      if (hasMatrix) slot.transform = pl.transform;
      slot.color_transform = pl.color_transform;
      if (hasName) slot.instance_name = pl.instance_name;
    }
  }

  resolved.index = static_cast<std::uint32_t>(frame_index);
  const AptFrame& srcFrame = timeline_frames[frame_index];
  resolved.frameitemcount = srcFrame.frameitemcount;
  resolved.offset = srcFrame.offset;
  resolved.items_offset = srcFrame.items_offset;
  resolved.items = srcFrame.items;
  resolved.placements.reserve(display_list.size());
  for (auto& [depth, placement] : display_list) {
    (void) depth;
    if (placement.character == 0xFFFFFFFFu) continue;
    resolved.placements.push_back(std::move(placement));
  }
  std::stable_sort(resolved.placements.begin(), resolved.placements.end(),
                   [](const AptPlacement& a, const AptPlacement& b) {
                     return a.depth < b.depth;
                   });
  return resolved;
}

std::optional<AptSummary> read_apt_summary(const std::string& apt_path,
                                          const std::string& const_path,
                                          std::string* err) {
  auto file = read_apt_file(apt_path, const_path, err);
  if (!file) return std::nullopt;

  AptSummary summary = file->summary;
  summary.framecount = static_cast<std::uint32_t>(file->frames.size());
  summary.charactercount = static_cast<std::uint32_t>(file->characters.size());
  summary.importcount = static_cast<std::uint32_t>(file->imports.size());
  summary.exportcount = static_cast<std::uint32_t>(file->exports.size());
  return summary;
}

} // namespace gf::apt
