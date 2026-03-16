#include "gf/platform/ps3/param_sfo.hpp"

#include "gf/core/log.hpp"

#include <filesystem>
#include <fstream>

namespace gf::platform::ps3 {
namespace fs = std::filesystem;

static uint16_t u16le(const uint8_t* p) { return uint16_t(p[0]) | (uint16_t(p[1]) << 8); }
static uint32_t u32le(const uint8_t* p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }

#pragma pack(push, 1)
struct PsfHeader {
  char magic[4];        // '\0PSF'
  uint32_t version;     // usually 0x00000101
  uint32_t key_table;   // offset
  uint32_t data_table;  // offset
  uint32_t entries;     // count
};

struct PsfEntry {
  uint16_t key_off;
  uint16_t fmt;         // 0x0404 = string, etc. We'll treat non-string as skip.
  uint32_t len;
  uint32_t max_len;
  uint32_t data_off;
};
#pragma pack(pop)

static bool is_magic(const char m[4]) {
  return (m[0] == '\0' && m[1] == 'P' && m[2] == 'S' && m[3] == 'F');
}

std::optional<ParamSfoData> read_param_sfo(const std::string& path, std::string* out_error) {
  auto fail = [&](const std::string& e) -> std::optional<ParamSfoData> {
    if (out_error) *out_error = e;
    gf::core::Log::get()->warn("PARAM.SFO parse failed: {} ({})", e, path);
    return std::nullopt;
  };

  if (!fs::exists(path)) return fail("file not found");

  std::ifstream in(path, std::ios::binary);
  if (!in) return fail("could not open");

  in.seekg(0, std::ios::end);
  const auto size = static_cast<size_t>(in.tellg());
  in.seekg(0, std::ios::beg);

  if (size < sizeof(PsfHeader)) return fail("too small");

  std::vector<uint8_t> buf(size);
  in.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
  if (!in) return fail("read error");

  const auto* hdr = reinterpret_cast<const PsfHeader*>(buf.data());
  if (!is_magic(hdr->magic)) return fail("bad magic");

  const uint32_t key_table = hdr->key_table;
  const uint32_t data_table = hdr->data_table;
  const uint32_t count = hdr->entries;

  const size_t entries_off = sizeof(PsfHeader);
  const size_t entries_size = size_t(count) * sizeof(PsfEntry);

  if (entries_off + entries_size > buf.size()) return fail("entry table out of bounds");
  if (key_table >= buf.size() || data_table >= buf.size()) return fail("tables out of bounds");

  ParamSfoData out;

  for (uint32_t i = 0; i < count; ++i) {
    const auto* ent = reinterpret_cast<const PsfEntry*>(buf.data() + entries_off + size_t(i) * sizeof(PsfEntry));

    const size_t key_off = size_t(key_table) + ent->key_off;
    if (key_off >= buf.size()) continue;

    // Read null-terminated key
    std::string key;
    for (size_t k = key_off; k < buf.size(); ++k) {
      const char c = static_cast<char>(buf[k]);
      if (c == '\0') break;
      key.push_back(c);
      if (key.size() > 256) break;
    }
    if (key.empty()) continue;

    // Data
    const size_t data_off = size_t(data_table) + ent->data_off;
    if (data_off >= buf.size()) continue;

    // If it's a string, interpret as UTF-8/ASCII null-terminated
    // Common string format is 0x0204 or 0x0404 depending on docs; be permissive.
    const bool maybe_string = (ent->fmt == 0x0204 || ent->fmt == 0x0404 || (ent->fmt & 0x00FF) == 0x04);
    if (!maybe_string) continue;

    const size_t len = std::min<size_t>(ent->len, buf.size() - data_off);
    std::string val;
    val.reserve(len);
    for (size_t b = 0; b < len; ++b) {
      const char c = static_cast<char>(buf[data_off + b]);
      if (c == '\0') break;
      val.push_back(c);
    }

    if (!val.empty()) out.strings[key] = val;
  }

  return out;
}

std::optional<ParamSfoResult> extract_title_and_id(const ParamSfoData& sfo) {
  ParamSfoResult r;
  auto it_title = sfo.strings.find("TITLE");
  auto it_id = sfo.strings.find("TITLE_ID");
  if (it_title != sfo.strings.end()) r.title = it_title->second;
  if (it_id != sfo.strings.end()) r.title_id = it_id->second;

  if (r.title.empty() && r.title_id.empty()) return std::nullopt;
  return r;
}

} // namespace gf::platform::ps3
