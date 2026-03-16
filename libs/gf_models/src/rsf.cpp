#include <gf/models/rsf.hpp>

#include <algorithm>
#include <array>
#include <cstring>

namespace gf::models::rsf {
namespace {

constexpr std::uint32_t tag_u32(const char a, const char b, const char c, const char d) {
  return (std::uint32_t(a) << 24) | (std::uint32_t(b) << 16) | (std::uint32_t(c) << 8) | std::uint32_t(d);
}

constexpr std::uint32_t TAG_INFO = tag_u32('I','N','F','O');
constexpr std::uint32_t TAG_TEXT = tag_u32('T','E','X','T');
constexpr std::uint32_t TAG_MATL = tag_u32('M','A','T','L');
constexpr std::uint32_t TAG_SGRF = tag_u32('S','G','R','F');

static bool read_u32(std::span<const std::uint8_t> b, std::size_t off, endian order, std::uint32_t& out) {
  if (off + 4 > b.size()) return false;
  std::uint32_t v = 0;
  std::memcpy(&v, b.data() + off, 4);
  if (order == endian::big) {
    v = ((v & 0x000000FFu) << 24) |
        ((v & 0x0000FF00u) << 8) |
        ((v & 0x00FF0000u) >> 8) |
        ((v & 0xFF000000u) >> 24);
  }
  out = v;
  return true;
}

static bool read_f32(std::span<const std::uint8_t> b, std::size_t off, endian order, float& out) {
  std::uint32_t u = 0;
  if (!read_u32(b, off, order, u)) return false;
  float f = 0.0f;
  std::memcpy(&f, &u, 4);
  out = f;
  return true;
}

static void write_u32(std::vector<std::uint8_t>& out, endian order, std::uint32_t v) {
  std::array<std::uint8_t, 4> b{};
  if (order == endian::big) {
    b[0] = (v >> 24) & 0xFF;
    b[1] = (v >> 16) & 0xFF;
    b[2] = (v >> 8) & 0xFF;
    b[3] = (v) & 0xFF;
  } else {
    b[0] = (v) & 0xFF;
    b[1] = (v >> 8) & 0xFF;
    b[2] = (v >> 16) & 0xFF;
    b[3] = (v >> 24) & 0xFF;
  }
  out.insert(out.end(), b.begin(), b.end());
}

static void write_f32(std::vector<std::uint8_t>& out, endian order, float f) {
  std::uint32_t u = 0;
  static_assert(sizeof(float) == 4);
  std::memcpy(&u, &f, 4);
  write_u32(out, order, u);
}

static std::uint32_t align_pad(std::uint32_t size, std::uint32_t multiple) {
  if (multiple <= 1) return 0;
  const std::uint32_t rem = size % multiple;
  return rem == 0 ? 0 : (multiple - rem);
}

static std::size_t align_up4(std::size_t v) {
  return (v + 3u) & ~std::size_t(3u);
}

static std::string read_str(std::span<const std::uint8_t> b, std::size_t off, std::size_t len) {
  if (off + len > b.size()) return {};
  std::size_t n = 0;
  while (n < len && b[off + n] != 0) ++n;
  return std::string(reinterpret_cast<const char*>(b.data() + off), n);
}

// Madden/EASE store the "type" ID as a 4-byte ASCII field, right-aligned.
static std::string id_to_type(const std::array<std::uint8_t, 4>& id) {
  std::string s;
  for (auto c : id) if (c != 0) s.push_back(char(c));
  return s;
}

static std::array<std::uint8_t, 4> type_to_id(std::string_view t) {
  std::array<std::uint8_t, 4> id{};
  const std::size_t n = std::min<std::size_t>(4, t.size());
  const std::size_t start = 4 - n;
  for (std::size_t i = 0; i < n; ++i) id[start + i] = std::uint8_t(t[i]);
  return id;
}

static std::optional<endian> detect_endian(std::span<const std::uint8_t> bytes) {
  if (bytes.size() < 20) return std::nullopt;
  // RSF header is 16 bytes; first section tag starts at +16 and should be INFO.
  std::uint32_t v_le = 0;
  std::uint32_t v_be = 0;
  if (!read_u32(bytes, 16, endian::little, v_le)) return std::nullopt;
  if (!read_u32(bytes, 16, endian::big, v_be)) return std::nullopt;
  if (v_be == TAG_INFO) return endian::big;
  if (v_le == TAG_INFO) return endian::little;
  return std::nullopt;
}

static std::optional<section_slice> slice_section(std::span<const std::uint8_t> bytes, std::size_t cursor, endian order) {
  // Generic Tiburon section header:
  // u32 tag, u32 unknown, u32 totalLength, u32 headerLength, ...
  std::uint32_t tag = 0, total = 0;
  if (!read_u32(bytes, cursor + 0, order, tag)) return std::nullopt;
  if (!read_u32(bytes, cursor + 8, order, total)) return std::nullopt;
  if (total == 0) return std::nullopt;
  if (cursor + total > bytes.size()) return std::nullopt;
  return section_slice{tag, cursor, total};
}

static bool parse_textures(std::span<const std::uint8_t> bytes, endian order, const section_slice& s, std::vector<texture_entry>& out) {
  // TEXT section:
  // u32 TAG('TEXT'), u32 unknown, u32 totalLen, u32 headerLen, u32 count, then entries...
  std::uint32_t count = 0;
  if (!read_u32(bytes, s.offset + 16, order, count)) return false;
  std::size_t cur = s.offset + 20;
  const std::size_t end = s.offset + s.size;
  out.clear();
  out.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    if (cur + 8 > end) break;
    std::uint32_t tag = 0;
    if (!read_u32(bytes, cur + 0, order, tag)) break;
    if (tag != TAG_TEXT) break;
    std::uint32_t nameLen = 0;
    if (!read_u32(bytes, cur + 4, order, nameLen)) break;
    if (cur + 8 + std::size_t(nameLen) + 1 > end) break;
    const std::string name = read_str(bytes, cur + 8, nameLen);
    cur += 8 + nameLen + 1; // +NUL
    std::uint32_t fileLen = 0;
    if (!read_u32(bytes, cur + 0, order, fileLen)) break;
    if (cur + 4 + std::size_t(fileLen) + 1 > end) break;
    const std::string file = read_str(bytes, cur + 4, fileLen);
    cur += 4 + fileLen + 1;
    out.push_back(texture_entry{name, file});
  }
  return !out.empty() || count == 0;
}

static bool parse_materials(std::span<const std::uint8_t> bytes, endian order, const section_slice& s, std::vector<material>& out) {
  // Material section header:
  // u32 TAG('MATL'), u32 unknown, u32 totalLen, u32 headerLen, u32 count, then MATL blocks...
  std::uint32_t count = 0;
  if (!read_u32(bytes, s.offset + 16, order, count)) return false;
  std::size_t cur = s.offset + 20;
  const std::size_t end = s.offset + s.size;
  out.clear();
  out.reserve(count);

  for (std::uint32_t i = 0; i < count; ++i) {
    if (cur + 12 > end) break;
    std::uint32_t tag = 0;
    if (!read_u32(bytes, cur + 0, order, tag)) break;
    if (tag != TAG_MATL) break;
    material m;
    if (!read_u32(bytes, cur + 4, order, m.pad0)) break;
    std::uint32_t nameLen = 0;
    if (!read_u32(bytes, cur + 8, order, nameLen)) break;
    if (cur + 12 + std::size_t(nameLen) + 1 > end) break;
    m.name = read_str(bytes, cur + 12, nameLen);
    cur += 12 + nameLen + 1;
    if (cur + 16 > end) break;
    if (!read_u32(bytes, cur + 0, order, m.pad1)) break;
    if (!read_u32(bytes, cur + 4, order, m.pad2)) break;
    if (!read_u32(bytes, cur + 8, order, m.sub_name_count)) break;
    std::uint32_t subLen = 0;
    if (!read_u32(bytes, cur + 12, order, subLen)) break;
    if (cur + 16 + std::size_t(subLen) + 1 > end) break;
    m.sub_name = read_str(bytes, cur + 16, subLen);
    cur += 16 + subLen + 1;
    if (cur + 8 > end) break;
    if (!read_u32(bytes, cur + 0, order, m.unknown_count)) break;
    std::uint32_t paramCount = 0;
    if (!read_u32(bytes, cur + 4, order, paramCount)) break;
    cur += 8;

    m.params.clear();
    m.params.reserve(paramCount);
    bool materialOk = true;
    for (std::uint32_t p = 0; p < paramCount; ++p) {
      if (cur + 12 > end) { materialOk = false; break; }
      const std::array<std::uint8_t, 4> id{bytes[cur + 0], bytes[cur + 1], bytes[cur + 2], bytes[cur + 3]};
      material_param mp;
      mp.var_type = id_to_type(id);
      std::uint32_t nameCount = 0, nameLen2 = 0;
      if (!read_u32(bytes, cur + 4, order, nameCount)) { materialOk = false; break; }
      if (!read_u32(bytes, cur + 8, order, nameLen2)) { materialOk = false; break; }
      cur += 12;
      mp.names.clear();
      for (std::uint32_t n = 0; n < nameCount; ++n) {
        if (cur + std::size_t(nameLen2) + 1 > end) { materialOk = false; break; }
        mp.names.push_back(read_str(bytes, cur, nameLen2));
        cur += nameLen2 + 1;
      }
      if (!materialOk) break;

      if (mp.var_type == "tex") {
        std::uint32_t idx = 0;
        if (!read_u32(bytes, cur, order, idx)) { materialOk = false; break; }
        mp.value = param_tex{idx};
        cur += 4;
      } else if (!mp.var_type.empty() && mp.var_type[0] == 'v' && mp.var_type.size() >= 2) {
        const int k = int(mp.var_type[1] - '0');
        if (k <= 0 || k > 4) { materialOk = false; break; }
        if (cur + std::size_t(k * 4) > end) { materialOk = false; break; }
        param_vec v;
        v.values.resize(std::size_t(k));
        for (int j = 0; j < k; ++j) {
          float f = 0.0f;
          if (!read_f32(bytes, cur, order, f)) { materialOk = false; break; }
          v.values[std::size_t(j)] = f;
          cur += 4;
        }
        if (!materialOk) break;
        mp.value = std::move(v);
      } else if (mp.var_type == "bool") {
        if (cur + 1 > end) { materialOk = false; break; }
        mp.value = param_bool{bytes[cur] == 1};
        cur += 1;
      } else if (mp.var_type == "i32") {
        std::uint32_t u = 0;
        if (!read_u32(bytes, cur, order, u)) { materialOk = false; break; }
        mp.value = param_i32{std::int32_t(u)};
        cur += 4;
      } else if (mp.var_type == "f32") {
        float f = 0.0f;
        if (!read_f32(bytes, cur, order, f)) { materialOk = false; break; }
        mp.value = param_f32{f};
        cur += 4;
      } else {
        // Unknown type: keep the material if possible by consuming a 4-byte payload.
        if (cur + 4 > end) { materialOk = false; break; }
        std::uint32_t u = 0;
        if (!read_u32(bytes, cur, order, u)) { materialOk = false; break; }
        mp.value = param_i32{std::int32_t(u)};
        cur += 4;
      }

      m.params.push_back(std::move(mp));
    }

    if (!materialOk) break;
    out.push_back(std::move(m));
  }

  return !out.empty() || count == 0;
}

static std::vector<std::uint8_t> rebuild_text_section(const document& doc, std::span<const std::uint8_t> rawText) {
  // Preserve Unknown + HeaderLength from original section.
  std::uint32_t unknown = 0, headerLen = 16;
  read_u32(rawText, 4, doc.order, unknown);
  read_u32(rawText, 12, doc.order, headerLen);
  if (headerLen == 0) headerLen = 16;

  std::vector<std::uint8_t> out;
  write_u32(out, doc.order, TAG_TEXT);
  write_u32(out, doc.order, unknown);
  const std::size_t totalPos = out.size();
  write_u32(out, doc.order, 0); // patched later
  write_u32(out, doc.order, headerLen);
  write_u32(out, doc.order, static_cast<std::uint32_t>(doc.textures.size()));

  for (const auto& t : doc.textures) {
    write_u32(out, doc.order, TAG_TEXT);
    write_u32(out, doc.order, static_cast<std::uint32_t>(t.name.size()));
    out.insert(out.end(), t.name.begin(), t.name.end());
    out.push_back(0);
    write_u32(out, doc.order, static_cast<std::uint32_t>(t.filename.size()));
    out.insert(out.end(), t.filename.begin(), t.filename.end());
    out.push_back(0);
  }

  const std::uint32_t pad = align_pad(static_cast<std::uint32_t>(out.size()), headerLen);
  out.insert(out.end(), pad, 0);
  const std::uint32_t totalLen = static_cast<std::uint32_t>(out.size());

  std::vector<std::uint8_t> tmp;
  write_u32(tmp, doc.order, totalLen);
  std::copy(tmp.begin(), tmp.end(), out.begin() + std::ptrdiff_t(totalPos));
  return out;
}

static std::vector<std::uint8_t> rebuild_matl_section(const document& doc, std::span<const std::uint8_t> rawMatl) {
  std::uint32_t unknown = 0, headerLen = 16;
  read_u32(rawMatl, 4, doc.order, unknown);
  read_u32(rawMatl, 12, doc.order, headerLen);
  if (headerLen == 0) headerLen = 16;

  std::vector<std::uint8_t> out;
  write_u32(out, doc.order, TAG_MATL);
  write_u32(out, doc.order, unknown);
  const std::size_t totalPos = out.size();
  write_u32(out, doc.order, 0); // patched later
  write_u32(out, doc.order, headerLen);
  write_u32(out, doc.order, static_cast<std::uint32_t>(doc.materials.size()));

  for (const auto& m : doc.materials) {
    write_u32(out, doc.order, TAG_MATL);
    write_u32(out, doc.order, m.pad0);
    write_u32(out, doc.order, static_cast<std::uint32_t>(m.name.size()));
    out.insert(out.end(), m.name.begin(), m.name.end());
    out.push_back(0);
    write_u32(out, doc.order, m.pad1);
    write_u32(out, doc.order, m.pad2);
    write_u32(out, doc.order, m.sub_name_count);
    write_u32(out, doc.order, static_cast<std::uint32_t>(m.sub_name.size()));
    out.insert(out.end(), m.sub_name.begin(), m.sub_name.end());
    out.push_back(0);
    write_u32(out, doc.order, m.unknown_count);
    write_u32(out, doc.order, static_cast<std::uint32_t>(m.params.size()));

    for (const auto& p : m.params) {
      const auto id = type_to_id(p.var_type);
      out.insert(out.end(), id.begin(), id.end());
      write_u32(out, doc.order, static_cast<std::uint32_t>(p.names.size()));
      const std::uint32_t nameLen = p.names.empty() ? 0u : static_cast<std::uint32_t>(p.names[0].size());
      write_u32(out, doc.order, nameLen);
      for (const auto& nm : p.names) {
        out.insert(out.end(), nm.begin(), nm.end());
        out.push_back(0);
      }

      if (std::holds_alternative<param_tex>(p.value)) {
        write_u32(out, doc.order, std::get<param_tex>(p.value).texture_index);
      } else if (std::holds_alternative<param_vec>(p.value)) {
        for (float f : std::get<param_vec>(p.value).values) write_f32(out, doc.order, f);
      } else if (std::holds_alternative<param_bool>(p.value)) {
        out.push_back(std::get<param_bool>(p.value).value ? 1 : 0);
      } else if (std::holds_alternative<param_i32>(p.value)) {
        write_u32(out, doc.order, static_cast<std::uint32_t>(std::get<param_i32>(p.value).value));
      } else if (std::holds_alternative<param_f32>(p.value)) {
        write_f32(out, doc.order, std::get<param_f32>(p.value).value);
      }
    }
  }

  const std::uint32_t pad = align_pad(static_cast<std::uint32_t>(out.size()), headerLen);
  out.insert(out.end(), pad, 0);
  const std::uint32_t totalLen = static_cast<std::uint32_t>(out.size());

  std::vector<std::uint8_t> tmp;
  write_u32(tmp, doc.order, totalLen);
  std::copy(tmp.begin(), tmp.end(), out.begin() + std::ptrdiff_t(totalPos));
  return out;
}

} // namespace

std::string fourcc(std::uint32_t tag) {
  char s[5];
  s[0] = char((tag >> 24) & 0xFF);
  s[1] = char((tag >> 16) & 0xFF);
  s[2] = char((tag >> 8) & 0xFF);
  s[3] = char(tag & 0xFF);
  s[4] = 0;
  return std::string(s);
}

std::optional<document> parse(std::span<const std::uint8_t> bytes) {
  if (bytes.size() < 32) return std::nullopt;
  if (!(bytes[0] == 'R' && bytes[1] == 'S' && bytes[2] == 'F') &&
      !(bytes[1] == 'R' && bytes[2] == 'S' && bytes[3] == 'F')) {
    return std::nullopt;
  }

  const auto ordOpt = detect_endian(bytes);
  if (!ordOpt) return std::nullopt;

  document doc;
  doc.order = *ordOpt;
  doc.header16.assign(bytes.begin(), bytes.begin() + 16);

  std::size_t cursor = 16;
  doc.sections.clear();
  doc.textures.clear();
  doc.materials.clear();
  doc.tail.clear();

  while (cursor + 16 <= bytes.size()) {
    auto slOpt = slice_section(bytes, cursor, doc.order);
    if (!slOpt) break;
    const section_slice s = *slOpt;
    doc.sections.push_back(s);

    if (s.tag == TAG_TEXT) {
      if (!parse_textures(bytes, doc.order, s, doc.textures)) {
        doc.textures.clear();
      }
    } else if (s.tag == TAG_MATL) {
      if (!parse_materials(bytes, doc.order, s, doc.materials)) {
        doc.materials.clear();
      }
    }

    cursor = s.offset + s.size;
    if (s.tag == TAG_SGRF) break;
  }

  if (cursor < bytes.size()) {
    doc.tail.assign(bytes.begin() + cursor, bytes.end());
  }

  const bool hasText = std::any_of(doc.sections.begin(), doc.sections.end(), [](const section_slice& s) { return s.tag == TAG_TEXT; });
  const bool hasMatl = std::any_of(doc.sections.begin(), doc.sections.end(), [](const section_slice& s) { return s.tag == TAG_MATL; });
  if (!hasText && !hasMatl) return std::nullopt;

  return doc;
}

std::vector<std::uint8_t> rebuild(const document& doc, std::span<const std::uint8_t> original_bytes) {
  std::vector<std::uint8_t> out;
  if (doc.header16.size() == 16) out.insert(out.end(), doc.header16.begin(), doc.header16.end());

  for (const auto& s : doc.sections) {
    if (s.offset + s.size > original_bytes.size()) break;
    const auto raw = original_bytes.subspan(s.offset, s.size);
    if (s.tag == TAG_TEXT) {
      const auto rebuilt = rebuild_text_section(doc, raw);
      out.insert(out.end(), rebuilt.begin(), rebuilt.end());
    } else if (s.tag == TAG_MATL) {
      const auto rebuilt = rebuild_matl_section(doc, raw);
      out.insert(out.end(), rebuilt.begin(), rebuilt.end());
    } else {
      out.insert(out.end(), raw.begin(), raw.end());
    }
  }

  out.insert(out.end(), doc.tail.begin(), doc.tail.end());
  return out;
}

} // namespace gf::models::rsf
