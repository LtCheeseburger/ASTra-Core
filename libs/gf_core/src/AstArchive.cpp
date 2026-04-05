#include <gf/core/AstArchive.hpp>

#include <array>
#include <span>
#include <sstream>
#include <stdexcept>
#include <filesystem>
#include <cstdio>
#include <fstream>
#include <limits>
#include <mutex>
#include <regex>
#include <cctype>
#include <unordered_map>
#include <zlib.h>
#include <type_traits>

namespace gf::core {

static std::atomic_bool g_debugClassificationTrace{false};

void AstArchive::setDebugClassificationTrace(bool enabled) {
  g_debugClassificationTrace.store(enabled, std::memory_order_relaxed);
}

bool AstArchive::debugClassificationTrace() {
  return g_debugClassificationTrace.load(std::memory_order_relaxed);
}

namespace {

static bool looks_like_zlib_cmf_flg(std::uint8_t cmf, std::uint8_t flg) {
  const int cm = (cmf & 0x0F);
  const int cinfo = (cmf >> 4);
  const bool fdict = ((flg & 0x20) != 0);
  if (cm != 8) return false;
  if (cinfo > 7) return false;
  if (fdict) return false;
  const int v = (static_cast<int>(cmf) << 8) | static_cast<int>(flg);
  return (v % 31) == 0;
}

static std::vector<std::uint8_t> zlib_inflate_unknown_size(std::span<const std::uint8_t> in) {
  // Safety: some malformed inputs can "inflate" to enormous sizes. We cap output to
  // avoid OOM / runaway allocations during best-effort parsing.
  constexpr std::size_t kMaxInflatedBytes = 64u * 1024u * 1024u; // 64 MiB
  constexpr std::size_t kChunk = 64u * 1024u;

  z_stream zs{};
  zs.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(in.data()));
  zs.avail_in = static_cast<uInt>(std::min<std::size_t>(in.size(),
                                                        static_cast<std::size_t>(std::numeric_limits<uInt>::max())));

  int rc = ::inflateInit2(&zs, 15); // zlib
  if (rc != Z_OK) {
    throw std::runtime_error("inflateInit2 failed");
  }

  std::vector<std::uint8_t> out;
  // Reserve conservatively. Never reserve beyond the safety cap.
  const std::size_t reserveHint = std::min<std::size_t>(kMaxInflatedBytes, in.size() * 2u + 1024u);
  out.reserve(reserveHint);

  std::array<std::uint8_t, kChunk> tmp{};

  while (true) {
    zs.next_out = reinterpret_cast<Bytef*>(tmp.data());
    zs.avail_out = static_cast<uInt>(tmp.size());

    rc = ::inflate(&zs, Z_NO_FLUSH);

    const std::size_t produced = tmp.size() - static_cast<std::size_t>(zs.avail_out);
    if (produced > 0) {
      if (out.size() + produced > kMaxInflatedBytes) {
        ::inflateEnd(&zs);
        throw std::runtime_error("inflate exceeded safety cap");
      }
      out.insert(out.end(), tmp.data(), tmp.data() + produced);
    }

    if (rc == Z_STREAM_END) break;
    if (rc != Z_OK) {
      ::inflateEnd(&zs);
      throw std::runtime_error("inflate failed");
    }
  }

  ::inflateEnd(&zs);
  return out;
}


static std::vector<std::uint8_t> read_file_slice(const std::filesystem::path& p,
                                                 std::uint64_t offset,
                                                 std::uint64_t size,
                                                 std::string* err) {
  std::ifstream f(p, std::ios::binary);
  if (!f) {
    if (err) *err = "Failed to open: " + p.string();
    return {};
  }
  f.seekg(0, std::ios::end);
  const auto endPos = f.tellg();
  if (endPos < 0) {
    if (err) *err = "tellg failed: " + p.string();
    return {};
  }
  const std::uint64_t fileSize = static_cast<std::uint64_t>(endPos);
  if (offset >= fileSize) {
    if (err) *err = "Offset out of range";
    return {};
  }
  const std::uint64_t maxRead = std::min<std::uint64_t>(size, fileSize - offset);
  std::vector<std::uint8_t> buf(static_cast<std::size_t>(maxRead));
  f.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
  if (!buf.empty()) {
    f.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
    if (!f) {
      if (err) *err = "Failed to read slice";
      return {};
    }
  }
  return buf;
}


// ============================================================================
// Forever caches (process lifetime)
// ============================================================================

struct IndexCacheKey {
  std::string path;
  std::uint64_t baseOffset{};
  std::uint64_t maxReadable{};

  bool operator==(const IndexCacheKey& o) const noexcept {
    return baseOffset == o.baseOffset && maxReadable == o.maxReadable && path == o.path;
  }
};

struct IndexCacheKeyHash {
  std::size_t operator()(const IndexCacheKey& k) const noexcept {
    std::size_t h = std::hash<std::string>{}(k.path);
    h ^= (std::hash<std::uint64_t>{}(k.baseOffset) + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2));
    h ^= (std::hash<std::uint64_t>{}(k.maxReadable) + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2));
    return h;
  }
};

struct MagicCacheKey {
  std::string path;
  std::uint64_t absOffset{};
  std::uint64_t size{};
  bool operator==(const MagicCacheKey& o) const noexcept {
    return absOffset == o.absOffset && size == o.size && path == o.path;
  }
};

struct MagicCacheKeyHash {
  std::size_t operator()(const MagicCacheKey& k) const noexcept {
    std::size_t h = std::hash<std::string>{}(k.path);
    h ^= (std::hash<std::uint64_t>{}(k.absOffset) + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2));
    h ^= (std::hash<std::uint64_t>{}(k.size) + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2));
    return h;
  }
};

static std::mutex g_cacheMutex;

static std::unordered_map<IndexCacheKey, std::shared_ptr<AstArchive::Index>, IndexCacheKeyHash> g_indexCache;

struct MagicHint {
  const char* type = nullptr; // e.g. "DDS", "XML", "AST", "ZLIB"
  const char* ext = nullptr;  // e.g. ".dds", ".xml"
  bool zlibWrapped = false;
  bool isText = false;
  bool isXml = false;
  std::string friendlyBase; // optional, no extension

  MagicHint() = default;
  MagicHint(const char* t, const char* e = nullptr, bool zw = false, bool txt = false, bool xml = false,
            std::string fb = {})
      : type(t), ext(e), zlibWrapped(zw), isText(txt), isXml(xml), friendlyBase(std::move(fb)) {}
};

static std::string toLowerAscii(std::string s) {
  for (char& c : s) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return s;
}

static std::string sanitizeFileStem(std::string s) {
  // Lowercase + trim.
  s = toLowerAscii(s);
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\n' || s.front() == '\r')) s.erase(s.begin());
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\n' || s.back() == '\r')) s.pop_back();

  // Replace separators and illegal filename chars.
  for (char& c : s) {
    if (c == '\\' || c == '/') c = '_';
    const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
    if (!ok) c = '_';
  }

  // Collapse repeats.
  std::string out;
  out.reserve(s.size());
  char prev = 0;
  for (char c : s) {
    if (c == '_' && prev == '_') continue;
    out.push_back(c);
    prev = c;
  }
  if (out.size() > 64) out.resize(64);
  while (!out.empty() && out.back() == '_') out.pop_back();
  return out;
}

static std::string extractXmlRootFriendly(const std::vector<std::uint8_t>& bytes) {
  // Very small, forgiving XML sniff: find first start element and pick root + a helpful attribute.
  const std::size_t max = std::min<std::size_t>(bytes.size(), 64ull * 1024ull);
  std::string s(reinterpret_cast<const char*>(bytes.data()), max);
  // Find first '<' that begins a tag (skip XML declarations / comments).
  std::size_t pos = 0;
  for (;;) {
    pos = s.find('<', pos);
    if (pos == std::string::npos) return {};
    if (pos + 1 >= s.size()) return {};
    if (s.compare(pos, 5, "<?xml") == 0 || s.compare(pos, 4, "<!--") == 0 || s.compare(pos, 2, "<?") == 0) {
      pos += 2;
      continue;
    }
    if (s[pos + 1] == '/' || s[pos + 1] == '!') {
      pos += 2;
      continue;
    }
    break;
  }

  std::size_t i = pos + 1;
  while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) ++i;
  std::size_t start = i;
  while (i < s.size()) {
    const char c = s[i];
    if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == ':' )) break;
    ++i;
  }
  if (i <= start) return {};
  std::string root = s.substr(start, i - start);

  // Search attributes in the start tag only.
  const std::size_t tagEnd = s.find('>', i);
  const std::size_t scanEnd = (tagEnd == std::string::npos) ? std::min<std::size_t>(s.size(), i + 4096) : tagEnd;
  std::string attrs = s.substr(i, scanEnd - i);
  auto pickAttr = [&](const char* key) -> std::string {
    const std::string k = std::string(key) + "=";
    std::size_t p = attrs.find(k);
    if (p == std::string::npos) return {};
    p += k.size();
    while (p < attrs.size() && (attrs[p] == ' ' || attrs[p] == '\t')) ++p;
    if (p >= attrs.size()) return {};
    const char quote = attrs[p];
    if (quote != '\"' && quote != '\'') return {};
    const std::size_t q2 = attrs.find(quote, p + 1);
    if (q2 == std::string::npos || q2 <= p + 1) return {};
    return attrs.substr(p + 1, q2 - (p + 1));
  };

  std::string best;
  for (const char* k : {"name", "id", "team", "asset", "path", "type"}) {
    best = pickAttr(k);
    if (!best.empty()) break;
  }
  if (!best.empty()) {
    for (char& c : best) if (c == '\\') c = '/';
    const std::size_t slash = best.rfind('/');
    if (slash != std::string::npos) best = best.substr(slash + 1);
    const std::size_t dot = best.rfind('.');
    if (dot != std::string::npos && dot > 0) best = best.substr(0, dot);
  }
  std::string stem = root;
  if (!best.empty()) stem += "_" + best;
  return sanitizeFileStem(stem);
}

static std::string inferFriendlyFromStrings(const std::vector<std::uint8_t>& bytes) {
  // Look for path-ish strings that end with known extensions; use the longest basename stem.
  const std::size_t max = std::min<std::size_t>(bytes.size(), 64ull * 1024ull);
  std::string s(reinterpret_cast<const char*>(bytes.data()), max);
  static const std::regex re(R"(([A-Za-z0-9_\-/\.]{3,96}\.(xml|dds|p3r|rsf|ast|apt|xfnr|db|sbk)))",
                             std::regex_constants::icase);
  std::string best;
  for (auto it = std::sregex_iterator(s.begin(), s.end(), re); it != std::sregex_iterator(); ++it) {
    std::string cand = (*it)[1].str();
    for (char& c : cand) if (c == '\\') c = '/';
    const std::size_t slash = cand.rfind('/');
    if (slash != std::string::npos) cand = cand.substr(slash + 1);
    const std::size_t dot = cand.rfind('.');
    if (dot != std::string::npos && dot > 0) cand = cand.substr(0, dot);
    cand = sanitizeFileStem(cand);
    if (cand.size() > best.size()) best = cand;
  }
  return best;
}

static std::string inferAptExportName(const std::vector<std::uint8_t>& bytes) {
  // Heuristic export-name extraction for APT/APT1:
  // Many APTs contain a table of ASCII strings including an "exports" marker
  // followed by an export name. GridironForge used this for UI-friendly naming.
  const std::size_t max = std::min<std::size_t>(bytes.size(), 256ull * 1024ull);
  std::vector<std::string> strings;
  strings.reserve(256);

  std::string cur;
  cur.reserve(128);

  auto flush = [&]() {
    if (cur.size() >= 3) {
      // Trim whitespace.
      std::size_t a = 0;
      while (a < cur.size() && static_cast<unsigned char>(cur[a]) <= 0x20) ++a;
      std::size_t b = cur.size();
      while (b > a && static_cast<unsigned char>(cur[b - 1]) <= 0x20) --b;
      if (b > a) strings.emplace_back(cur.substr(a, b - a));
    }
    cur.clear();
  };

  for (std::size_t i = 0; i < max; ++i) {
    const unsigned char c = bytes[i];
    const bool printable = (c >= 0x20 && c <= 0x7E);
    if (printable) {
      cur.push_back(static_cast<char>(c));
      if (cur.size() > 160) flush();
    } else {
      flush();
    }
  }
  flush();

  auto lower = [](std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
  };

  // 1) "exports" marker followed by a name-like token.
  for (std::size_t i = 0; i + 1 < strings.size(); ++i) {
    const std::string key = lower(strings[i]);
    if (key == "exports" || key == "export" || key == "exportname" || key == "export_name") {
      std::string cand = strings[i + 1];
      // Sometimes the export is stored as a path; take the basename stem.
      for (char& ch : cand) if (ch == '\\') ch = '/';
      if (auto p = cand.rfind('/'); p != std::string::npos) cand = cand.substr(p + 1);
      if (auto d = cand.rfind('.'); d != std::string::npos && d > 0) cand = cand.substr(0, d);
      cand = sanitizeFileStem(cand);
      if (!cand.empty()) return cand;
    }
  }

  // 2) Fallback: pick the best "name" looking string (prefer longer path-ish with no spaces).
  std::string best;
  for (const auto& s : strings) {
    if (s.size() < 4 || s.size() > 96) continue;
    // Avoid generic tokens.
    const auto ls = lower(s);
    if (ls == "apt" || ls == "apt1" || ls == "apt data" || ls == "aptdata") continue;
    // Prefer tokens that look like identifiers/paths.
    bool ok = true;
    for (char c : s) {
      const unsigned char uc = static_cast<unsigned char>(c);
      if (!(std::isalnum(uc) || c == '_' || c == '-' || c == '.' || c == '/' )) { ok = false; break; }
    }
    if (!ok) continue;
    std::string cand = s;
    for (char& ch : cand) if (ch == '\\') ch = '/';
    if (auto p = cand.rfind('/'); p != std::string::npos) cand = cand.substr(p + 1);
    if (auto d = cand.rfind('.'); d != std::string::npos && d > 0) cand = cand.substr(0, d);
    cand = sanitizeFileStem(cand);
    if (cand.size() > best.size()) best = cand;
  }
  return best;
}

static std::string inferFriendlyDdsDims(const std::vector<std::uint8_t>& bytes) {
  if (bytes.size() < 128) return {};
  if (!(bytes[0] == 'D' && bytes[1] == 'D' && bytes[2] == 'S' && bytes[3] == ' ')) return {};
  auto rd_u32 = [&](std::size_t off) -> std::uint32_t {
    return std::uint32_t(bytes[off]) | (std::uint32_t(bytes[off + 1]) << 8) |
           (std::uint32_t(bytes[off + 2]) << 16) | (std::uint32_t(bytes[off + 3]) << 24);
  };
  const std::uint32_t h = rd_u32(12);
  const std::uint32_t w = rd_u32(16);
  if (!w || !h) return {};
  return "tex_" + std::to_string(w) + "x" + std::to_string(h);
}

static std::vector<std::uint8_t> inflateZlibPrefix(const std::vector<std::uint8_t>& in, std::size_t outCap) {
  std::vector<std::uint8_t> out;
  out.resize(outCap);
  z_stream zs{};
  zs.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(in.data()));
  zs.avail_in = static_cast<uInt>(in.size());
  zs.next_out = reinterpret_cast<Bytef*>(out.data());
  zs.avail_out = static_cast<uInt>(out.size());
  if (inflateInit(&zs) != Z_OK) return {};
  const int rc = inflate(&zs, Z_SYNC_FLUSH);
  inflateEnd(&zs);
  if (rc != Z_OK && rc != Z_STREAM_END) return {};
  out.resize(static_cast<std::size_t>(zs.total_out));
  return out;
}


} // namespace (anonymous)

// Public wrapper used by the GUI for hex/text preview.
std::vector<std::uint8_t> AstArchive::inflateZlibPreview(const std::vector<std::uint8_t>& in,
                                                         std::size_t outCap) {
  if (in.empty() || outCap == 0) return {};
  return inflateZlibPrefix(in, outCap);
}

namespace {

static std::vector<std::uint8_t> readPayloadPrefixPossiblyZlib(std::istream& f,
                                                               std::uint64_t absOffset,
                                                               std::uint64_t compSize,
                                                               std::uint64_t fileSize) {
  const std::uint64_t maxIn = std::min<std::uint64_t>(compSize, 64ull * 1024ull);
  if (maxIn < 8 || absOffset + maxIn > fileSize) return {};
  std::vector<std::uint8_t> in(static_cast<std::size_t>(maxIn));
  const auto back = f.tellg();
  f.clear();
  f.seekg(static_cast<std::streamoff>(absOffset), std::ios::beg);
  if (!f.read(reinterpret_cast<char*>(in.data()), static_cast<std::streamsize>(in.size()))) {
    f.clear();
    f.seekg(back, std::ios::beg);
    return {};
  }
  f.clear();
  f.seekg(back, std::ios::beg);

  // If zlib-ish, inflate a larger prefix for naming.
  if (in.size() >= 2 && in[0] == 0x78 && (in[1] == 0x01 || in[1] == 0x9C || in[1] == 0xDA)) {
    auto out = inflateZlibPrefix(in, 64u * 1024u);
    if (!out.empty()) return out;
  }
  return in;
}

static std::unordered_map<MagicCacheKey, MagicHint, MagicCacheKeyHash> g_magicCache;

template <typename T>

bool readLE(std::istream& is, T& out) {
  static_assert(std::is_integral_v<T> || std::is_enum_v<T>);
  std::array<std::uint8_t, sizeof(T)> buf{};
  if (!is.read(reinterpret_cast<char*>(buf.data()), buf.size())) return false;
  std::uint64_t v = 0;
  for (std::size_t i = 0; i < buf.size(); ++i) {
    v |= (static_cast<std::uint64_t>(buf[i]) << (8ull * i));
  }
  out = static_cast<T>(v);
  return true;
}

bool readBytes(std::istream& is, std::vector<std::uint8_t>& out, std::size_t n) {
  out.resize(n);
  if (n == 0) return true;
  return static_cast<bool>(is.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(n)));
}

std::string bytesToCString(const std::vector<std::uint8_t>& bytes) {
  std::size_t len = 0;
  while (len < bytes.size() && bytes[len] != 0) ++len;
  return std::string(reinterpret_cast<const char*>(bytes.data()), len);
}

std::uint64_t readCustomUnsigned(std::istream& is, std::uint8_t byteCount) {
  std::uint64_t v = 0;
  for (std::uint8_t i = 0; i < byteCount; ++i) {
    std::uint8_t b = 0;
    if (!readLE(is, b)) return 0;
    v |= (static_cast<std::uint64_t>(b) << (8ull * i));
  }
  return v;
}

// Read N bytes (LE) and then apply a left shift.
std::uint64_t readCustomUnsignedShift(std::istream& is, std::uint8_t byteCount, std::uint8_t shift) {
  const std::uint64_t base = readCustomUnsigned(is, byteCount);
  if (shift >= 64) return 0;
  return (base << shift);
}



MagicHint sniffMagic(const std::array<std::uint8_t, 16>& b) {
  // Common uncompressed file signatures (best-effort).
  if (b[0] == 'D' && b[1] == 'D' && b[2] == 'S' && b[3] == ' ') return {"DDS", ".dds"};
  if (b[0] == 'p' && b[1] == '3' && b[2] == 'R') return {"P3R", ".p3r"};
  // APT can appear with several headers:
  //  - "APT" / "apt" (common)
  //  - "APT1" (variant)
  //  - "APTData" (wrapper used by some UI bundles)
  if (b[0] == 'A' && b[1] == 'P' && b[2] == 'T' && b[3] == '1') return {"APT1", ".apt1"};
  if (b[0] == 'A' && b[1] == 'P' && b[2] == 'T') return {"APT", ".apt"};
  if (b[0] == 'a' && b[1] == 'p' && b[2] == 't') return {"APT", ".apt"};
  if (b[0] == 'A' && b[1] == 'P' && b[2] == 'T' && b[3] == 'D' && b[4] == 'a' && b[5] == 't' && b[6] == 'a') return {"APT", ".apt"};
  // Some bundles use mixed-case/spaced headers ("Apt Data", "Apt1").
  if (b[0] == 'A' && b[1] == 'p' && b[2] == 't' && b[3] == '1') return {"CONST", ".const"};
  // "Apt constant file\x1A..." — EA APT CONST companion (e.g. Madden / NCAA Xbox 360).
  if (b[0] == 'A' && b[1] == 'p' && b[2] == 't' && b[3] == ' ' &&
      b[4] == 'c' && b[5] == 'o' && b[6] == 'n' && b[7] == 's') return {"CONST", ".const"};
  if (b[0] == 'A' && b[1] == 'p' && b[2] == 't' && b[3] == 'D' && b[4] == 'a' && b[5] == 't' && b[6] == 'a') return {"APT", ".apt"};
  if (b[0] == 'A' && b[1] == 'p' && b[2] == 't' && b[3] == ' ' && b[4] == 'D' && b[5] == 'a' && b[6] == 't' && b[7] == 'a') return {"APT", ".apt"};

  if (b[0] == 'D' && b[1] == 'B') return {"DB", ".db"};
  if (b[0] == 'T' && b[1] == 'e' && b[2] == 'x' && b[3] == 't') return {"TXT", ".txt"};
  if (b[0] == 0x89 && b[1] == 'P' && b[2] == 'N' && b[3] == 'G') return {"PNG", ".png"};
  if (b[0] == 'S' && b[1] == 'B' && b[2] == 'K' && b[3] == 'R') return {"SBK", ".sbkr"};
  if (b[0] == 'S' && b[1] == 'B' && b[2] == 'b' && b[3] == 'e') return {"SBB", ".sbbe"};
  if (b[0] == 'O' && b[1] == 'g' && b[2] == 'g' && b[3] == 'S') return {"OGG", ".ogg"};
  if (b[0] == 'R' && b[1] == 'I' && b[2] == 'F' && b[3] == 'F') return {"RIFF", ".riff"};
  if (b[0] == 'B' && b[1] == 'G' && b[2] == 'F' && b[3] == 'A') return {"AST/BGFA", ".ast"};
  // EA VP6 container: "MVhd" at offset 0, "vp6" at offset 8 (payload codec id)
  if (b[0] == 'M' && b[1] == 'V' && b[2] == 'h' && b[3] == 'd' &&
      b[8] == 'v' && b[9] == 'p' && b[10] == '6') return {"VP6", ".vp6"};
  // ISO Base Media (MP4/MOV/M4V): "ftyp" box at offset 4
  if (b[4] == 'f' && b[5] == 't' && b[6] == 'y' && b[7] == 'p') return {"MP4", ".mp4"};
  if (b[0] == 'X' && b[1] == 'P' && b[2] == 'R' && b[3] == '2') return {"XPR2", ".xpr2"};
  if (b[0] == 'X' && b[1] == 'P' && b[2] == 'R' && b[3] == 0x00) return {"XPR", ".xpr"};
// XML / markup: allow UTF-8 BOM and leading whitespace, and common UTF-16LE markup.
{
  std::size_t off = 0;
  // UTF-8 BOM
  if (b[0] == 0xEF && b[1] == 0xBB && b[2] == 0xBF) off = 3;
  // Skip ASCII whitespace
  while (off < b.size() && (b[off] == ' ' || b[off] == '\t' || b[off] == '\r' || b[off] == '\n')) off++;
  if (off < b.size() && b[off] == '<') return {"XML", ".xml"};
  // UTF-16LE BOM + '<'
  if (b[0] == 0xFF && b[1] == 0xFE) {
    // "<" 0x00
    if (b.size() >= 4 && b[2] == '<' && b[3] == 0x00) return {"XML", ".xml"};
    // "<" 0x00 "?" 0x00 "x" 0x00 "m" 0x00 "l" 0x00
    if (b.size() >= 12 &&
        b[2] == '<' && b[3] == 0x00 &&
        b[4] == '?' && b[5] == 0x00 &&
        (b[6] == 'x' || b[6] == 'X') && b[7] == 0x00) {
      return {"XML", ".xml"};
    }
  }
}


  // zlib stream (very common for packed entries) - avoid mislabeling.
  if (b[0] == 0x78 && (b[1] == 0x01 || b[1] == 0x9C || b[1] == 0xDA)) return {"zlib", nullptr};

  // RSF containers in some EA titles often begin with 'RSF' or 'RSG' (heuristic).
  if (b[0] == 'R' && b[1] == 'S' && b[2] == 'F') return {"RSF", ".rsf"};
  if (b[0] == 'R' && b[1] == 'S' && b[2] == 'G') return {"RSG", ".rsg"};

  // DAT geometry file (no magic bytes; validated by structural heuristic).
  // Must be last — rule out everything else first.
  if (b.size() >= 16) {
    const std::uint32_t dat_flen  = (b[0]<<24)|(b[1]<<16)|(b[2]<<8)|b[3];
    const std::uint32_t dat_nimgs = (b[4]<<24)|(b[5]<<16)|(b[6]<<8)|b[7];
    const std::uint32_t dat_zeros = (b[8]<<24)|(b[9]<<16)|(b[10]<<8)|b[11];
    const std::uint32_t dat_off0  = (b[12]<<24)|(b[13]<<16)|(b[14]<<8)|b[15];
    if (dat_zeros == 0 && dat_nimgs > 0 && dat_nimgs < 5000 &&
        dat_flen > 0 &&
        dat_off0 >= (12u + dat_nimgs * 4u) && dat_off0 < dat_flen)
      return {"DAT", ".dat"};
  }

  return {};
}

// Try sniffing a type from a decompressed (or partially decompressed) prefix.
// Many EA payloads include a small custom header before the real file signature.
static MagicHint sniffFromInflatedPrefix(const std::vector<std::uint8_t>& out) {
  if (out.empty()) return {};

  auto sniffAt = [&](std::size_t off) -> MagicHint {
    if (off + 16 > out.size()) return {};
    std::array<std::uint8_t, 16> b{};
    for (std::size_t i = 0; i < b.size(); ++i) b[i] = out[off + i];
    return sniffMagic(b);
  };

  // 1) Trim leading whitespace/nulls then sniff.
  {
    std::size_t off = 0;
    while (off < out.size()) {
      const auto c = out[off];
      if (c == 0 || c == ' ' || c == '\t' || c == '\r' || c == '\n') {
        ++off;
        continue;
      }
      break;
    }
    if (auto h = sniffAt(off); h.type && std::string_view(h.type) != "zlib") {
      // Accept XML only when it appears right at the start after trimming.
      if (std::string_view(h.type) == "XML") {
        const std::size_t maxScan = std::min<std::size_t>(out.size(), off + 512u);
        bool hasClose = false;
        for (std::size_t i = off; i < maxScan; ++i) {
          if (out[i] == '>') { hasClose = true; break; }
        }
        if (hasClose) return h;
      } else {
        return h;
      }
    }
  }

  // 2) Common offset probes (EA wrappers before DDS/XML/etc.).
  for (std::size_t off : {std::size_t(0x10), std::size_t(0x20), std::size_t(0x30), std::size_t(0x40), std::size_t(0x80)}) {
    if (auto h = sniffAt(off); h.type && std::string_view(h.type) != "zlib") {
      // Accept XML only when it appears right at the start after trimming.
      if (std::string_view(h.type) == "XML") {
        const std::size_t maxScan = std::min<std::size_t>(out.size(), off + 512u);
        bool hasClose = false;
        for (std::size_t i = off; i < maxScan; ++i) {
          if (out[i] == '>') { hasClose = true; break; }
        }
        if (hasClose) return h;
      } else {
        return h;
      }
    }
  }

  // 3) Scan within first 4KB for *strong* signatures.
  // IMPORTANT: Do NOT treat '<' as XML during a broad scan, because APT/APT1/APTData
  // payloads frequently embed XML fragments near the beginning. XML must be a fallback
  // only when no stronger match is found.
  const std::size_t scanMax = std::min<std::size_t>(out.size(), 4096u);
  auto hasAt = [&](std::size_t i, const char* s, std::size_t n) -> bool {
    if (i + n > scanMax) return false;
    for (std::size_t k = 0; k < n; ++k) {
      if (out[i + k] != static_cast<std::uint8_t>(s[k])) return false;
    }
    return true;
  };
  for (std::size_t i = 0; i + 4 <= scanMax; ++i) {
    if (hasAt(i, "DDS ", 4)) return {"DDS", ".dds"};
    if (hasAt(i, "BGFA", 4)) return {"AST/BGFA", ".ast"};
    // APT payloads may sit behind small wrappers inside zlib streams.
    if (hasAt(i, "APT1", 4)) return {"APT1", ".apt1"};
    if (hasAt(i, "APTData", 7)) return {"APT", ".apt"};
    if (hasAt(i, "XPR2", 4)) return {"XPR2", ".xpr2"};
    if (hasAt(i, "OggS", 4)) return {"OGG", ".ogg"};
    if (hasAt(i, "RIFF", 4)) return {"RIFF", ".riff"};
  }

  return {};
}


// If data looks like zlib, try inflating a small prefix and sniffing that.
MagicHint sniffMagicPossiblyZlib(std::istream& f,
                                std::uint64_t absOffset,
                                std::uint64_t compSize,
                                std::uint64_t fileBound,
                                const std::string* pathOrNull) {
  auto trace = [&](const char* stage, const MagicHint& h) {
    if (!AstArchive::debugClassificationTrace()) return;
    const char* t = h.type ? h.type : "(null)";
    std::fprintf(stderr,
                 "[ASTra][classify] %s off=0x%llX comp=%llu -> %s\n",
                 stage,
                 static_cast<unsigned long long>(absOffset),
                 static_cast<unsigned long long>(compSize),
                 t);
  };
  // Cache first (fast path). For embedded parsing we may not have a path.
  if (pathOrNull) {
    MagicCacheKey key{*pathOrNull, absOffset, compSize};
    std::lock_guard<std::mutex> lock(g_cacheMutex);
    auto it = g_magicCache.find(key);
    if (it != g_magicCache.end()) {
      return it->second;
    }
  }

  std::array<std::uint8_t, 16> head{};
  if (absOffset >= fileBound) return {};
  f.clear();
  f.seekg(static_cast<std::streamoff>(absOffset), std::ios::beg);
  if (!f) return {};
  if (!f.read(reinterpret_cast<char*>(head.data()), static_cast<std::streamsize>(head.size()))) return {};

  auto hint = sniffMagic(head);
  if (hint.type && std::string_view(hint.type) != "zlib") {
    trace("head", hint);
    if (pathOrNull) {
      std::lock_guard<std::mutex> lock(g_cacheMutex);
      g_magicCache.emplace(MagicCacheKey{*pathOrNull, absOffset, compSize},
                           hint);
    }
    return hint;
  }

  // If not zlib-ish, we're done.
  if (!(head[0] == 0x78 && (head[1] == 0x01 || head[1] == 0x9C || head[1] == 0xDA))) {
    if (pathOrNull && hint.type) {
      std::lock_guard<std::mutex> lock(g_cacheMutex);
      g_magicCache.emplace(MagicCacheKey{*pathOrNull, absOffset, compSize},
                           hint);
    }
    if (hint.type) trace("head-weak", hint);
    return hint;
  }

  // Deeper zlib sniff (FAST):
  // - read a small compressed prefix
  // - inflate only a tiny prefix
  // - sniff known magics / XML root
  const std::uint64_t maxIn = std::min<std::uint64_t>(compSize, 64ull * 1024ull);
  if (maxIn < 8) {
    trace("zlib-too-small", MagicHint{"ZLIB", nullptr});
    if (pathOrNull) {
      std::lock_guard<std::mutex> lock(g_cacheMutex);
      g_magicCache.emplace(MagicCacheKey{*pathOrNull, absOffset, compSize},
                           MagicHint{"ZLIB", nullptr});
    }
    return {"ZLIB", nullptr};
  }

  std::vector<std::uint8_t> in(maxIn);
  f.clear();
  f.seekg(static_cast<std::streamoff>(absOffset), std::ios::beg);
  if (!f.read(reinterpret_cast<char*>(in.data()), static_cast<std::streamsize>(in.size()))) {
    trace("zlib-read-fail", MagicHint{"ZLIB", nullptr});
    if (pathOrNull) {
      std::lock_guard<std::mutex> lock(g_cacheMutex);
      g_magicCache.emplace(MagicCacheKey{*pathOrNull, absOffset, compSize},
                           MagicHint{"ZLIB", nullptr});
    }
    return {"ZLIB", nullptr};
  }

  std::vector<std::uint8_t> out;
  out.resize(4u * 1024u);

  z_stream zs{};
  zs.next_in = reinterpret_cast<Bytef*>(in.data());
  zs.avail_in = static_cast<uInt>(in.size());
  zs.next_out = reinterpret_cast<Bytef*>(out.data());
  zs.avail_out = static_cast<uInt>(out.size());

  if (inflateInit(&zs) != Z_OK) {
    trace("zlib-inflateinit-fail", MagicHint{"ZLIB", nullptr});
    if (pathOrNull) {
      std::lock_guard<std::mutex> lock(g_cacheMutex);
      g_magicCache.emplace(MagicCacheKey{*pathOrNull, absOffset, compSize},
                           MagicHint{"ZLIB", nullptr});
    }
    return {"ZLIB", nullptr};
  }
  const int rc = inflate(&zs, Z_SYNC_FLUSH);
  inflateEnd(&zs);
  if (rc != Z_OK && rc != Z_STREAM_END) {
    trace("zlib-inflate-fail", MagicHint{"ZLIB", nullptr});
    if (pathOrNull) {
      std::lock_guard<std::mutex> lock(g_cacheMutex);
      g_magicCache.emplace(MagicCacheKey{*pathOrNull, absOffset, compSize},
                           MagicHint{"ZLIB", nullptr});
    }
    return {"ZLIB", nullptr};
  }

  out.resize(static_cast<std::size_t>(zs.total_out));
  MagicHint inner = sniffFromInflatedPrefix(out);
  if (!inner.type) inner = {"ZLIB", nullptr};
  trace("zlib-inflated", inner);

  if (pathOrNull) {
    std::lock_guard<std::mutex> lock(g_cacheMutex);
    g_magicCache.emplace(MagicCacheKey{*pathOrNull, absOffset, compSize},
                         inner);
  }
  return inner;
}


bool hasExtension(const std::string& s) {
  const auto pos = s.find_last_of('.');
  if (pos == std::string::npos) return false;
  // Ignore leading dots (".bashrc") style.
  return pos > 0 && pos + 1 < s.size();
}


bool readMagicBGFA(std::istream& is) {
  char m[4] = {0, 0, 0, 0};
  if (!is.read(m, 4)) return false;
  return (m[0] == 'B' && m[1] == 'G' && m[2] == 'F' && m[3] == 'A');
}

} // namespace

std::optional<AstArchive::Index> AstArchive::readIndex(const std::filesystem::path& astPath,
                                                       std::string* err) {
  if (err) err->clear();

  const std::uint64_t fileSize = std::filesystem::exists(astPath)
                                    ? static_cast<std::uint64_t>(std::filesystem::file_size(astPath))
                                    : 0ull;

  const IndexCacheKey cacheKey{astPath.string(), 0ull, fileSize};
  {
    std::scoped_lock lk(g_cacheMutex);
    auto it = g_indexCache.find(cacheKey);
    if (it != g_indexCache.end() && it->second) {
      return *(it->second);
    }
  }
  std::ifstream f(astPath, std::ios::binary);
  if (!f) {
    if (err) *err = "Failed to open AST file.";
    return std::nullopt;
  }

  // Header
  if (!readMagicBGFA(f)) {
    if (err) *err = "Not a BGFA/AST file (magic mismatch).";
    return std::nullopt;
  }

  Index idx;

  // The legacy tools treat everything after magic as little-endian.
  std::uint32_t magicV = 0;
  std::uint32_t fakeFileCount = 0;
  if (!readLE(f, magicV) || !readLE(f, fakeFileCount) || !readLE(f, idx.file_count)) {
    if (err) *err = "Truncated AST header.";
    return std::nullopt;
  }

  std::int64_t dirOffset = 0;
  std::int64_t dirSize = 0;
  if (!readLE(f, dirOffset) || !readLE(f, dirSize)) {
    if (err) *err = "Truncated AST header (dir offsets).";
    return std::nullopt;
  }
  idx.dir_offset = static_cast<std::uint64_t>(dirOffset);
  idx.dir_size = static_cast<std::uint64_t>(dirSize);

  std::uint8_t nametype = 0;
  std::uint8_t flagtype = 0;
  std::uint8_t tagsize = 0;
  std::uint8_t offsetsize = 0;
  std::uint8_t compsize = 0;
  std::uint8_t sizediffsize = 0;
  std::uint8_t shiftsize = 0;
  std::uint8_t unknownsize = 0;
  if (!readLE(f, nametype) || !readLE(f, flagtype) || !readLE(f, tagsize) ||
      !readLE(f, offsetsize) || !readLE(f, compsize) || !readLE(f, sizediffsize) ||
      !readLE(f, shiftsize) || !readLE(f, unknownsize)) {
    if (err) *err = "Truncated AST header (field sizes).";
    return std::nullopt;
  }

  if (!readLE(f, idx.tag_count) || !readLE(f, idx.file_name_length)) {
    if (err) *err = "Truncated AST header (tag/name counts).";
    return std::nullopt;
  }

  // Basic sanity limits to avoid pathological reads.
  if (idx.file_count > 5'000'000u) {
    if (err) *err = "Unreasonable file count; refusing to parse.";
    return std::nullopt;
  }
  if (idx.file_name_length > 4096u) {
    if (err) *err = "Unreasonable filename length; refusing to parse.";
    return std::nullopt;
  }
  if (tagsize > 32u || offsetsize > 16u || compsize > 16u || sizediffsize > 16u) {
    if (err) *err = "Unsupported header field sizes.";
    return std::nullopt;
  }

  // Directory bounds checks (crash safety).
  if (idx.dir_offset >= fileSize) {
    if (err) *err = "Directory offset out of range.";
    return std::nullopt;
  }
  if (idx.dir_size != 0 && (idx.dir_offset + idx.dir_size) > fileSize) {
    if (err) *err = "Directory range out of file bounds.";
    return std::nullopt;
  }

  // Jump to directory.
  f.seekg(static_cast<std::streamoff>(idx.dir_offset), std::ios::beg);
  if (!f) {
    if (err) *err = "Failed to seek to directory.";
    return std::nullopt;
  }

  // Read tag list (used for platform hints in legacy tools).
  std::vector<std::uint32_t> tags;
  tags.reserve(idx.tag_count);
  for (std::uint32_t i = 0; i < idx.tag_count; ++i) {
    std::uint32_t t = 0;
    if (!readLE(f, t)) {
      if (err) *err = "Truncated tag list.";
      return std::nullopt;
    }
    tags.push_back(t);
  }
  // Known magic tags from legacy tools.
  // XBOX: 777539666 (0x2E56B9D2)
  // PS3 : 1814246228 (0x6C28D674)
  for (auto t : tags) {
    if (t == 777539666u) idx.is_xbox = true;
    if (t == 1814246228u) idx.is_ps3 = true;
  }

  // Directory records.
  idx.entries.clear();
  idx.entries.reserve(idx.file_count);

  constexpr std::uint8_t recordFlagSize = 1; // per legacy BGFA
  const std::uint32_t recordSize =
      static_cast<std::uint32_t>(recordFlagSize) + static_cast<std::uint32_t>(flagtype) +
      static_cast<std::uint32_t>(tagsize) + static_cast<std::uint32_t>(offsetsize) +
      static_cast<std::uint32_t>(compsize) + static_cast<std::uint32_t>(sizediffsize) +
      static_cast<std::uint32_t>(unknownsize) + idx.file_name_length;
  // Directory truncation safety: if the directory region is smaller than the declared
  // record count, clamp to what can be safely read. This prevents crashes on malformed ASTs
  // (e.g. some legacy titles) while still allowing partial browsing.
  const auto curPos0 = f.tellg();
  if (curPos0 < 0) {
    if (err) *err = "tellg failed while reading directory.";
    return std::nullopt;
  }
  const std::uint64_t curPos = static_cast<std::uint64_t>(curPos0);
  const std::uint64_t dirEnd = (idx.dir_size != 0) ? (idx.dir_offset + idx.dir_size) : fileSize;
  if (curPos > dirEnd) {
    if (err) *err = "Directory cursor out of range.";
    return std::nullopt;
  }
  if (recordSize != 0) {
    const std::uint64_t remaining = dirEnd - curPos;
    const std::uint64_t maxRecords = remaining / static_cast<std::uint64_t>(recordSize);
    if (maxRecords < idx.file_count) {
      if (err) {
        *err = "Directory truncated; returning partial index (" +
               std::to_string(maxRecords) + " of " + std::to_string(idx.file_count) + " entries).";
      }
      idx.file_count = static_cast<std::uint32_t>(maxRecords);
    }
  }



  // A weak sanity check: directory should be able to contain file_count records.
  if (idx.dir_size != 0 && (static_cast<std::uint64_t>(recordSize) * idx.file_count) > idx.dir_size + 64ull) {
    // Allow some wiggle room (tag list included, etc.), but still guard.
    // Don't hard fail; just proceed carefully.
  }

  for (std::uint32_t i = 0; i < idx.file_count; ++i) {
    Entry e;
    e.index = i;

    // Record flags.
    // The header exposes an additional `flagtype` width; legacy tools treat the
    // on-disk record as 1 byte (recordFlagSize) plus `flagtype` bytes.
    // If we fail to consume `flagtype`, subsequent fields become misaligned
    // and names/offsets appear "skewed".
    const std::uint8_t flagsWidth = static_cast<std::uint8_t>(recordFlagSize + flagtype);
    e.flags = static_cast<std::uint32_t>(readCustomUnsigned(f, flagsWidth));

    // Tag bytes / ID/Hash field
    std::vector<std::uint8_t> tagBytes;
    if (!readBytes(f, tagBytes, tagsize)) {
      if (err) *err = "Truncated directory entry (tag bytes).";
      return std::nullopt;
    }
    // Best-effort interpret first 8 bytes as little-endian number.
    std::uint64_t idOrHash = 0;
    for (std::size_t b = 0; b < tagBytes.size() && b < 8; ++b) {
      idOrHash |= (static_cast<std::uint64_t>(tagBytes[b]) << (8ull * b));
    }
    e.id_or_hash = idOrHash;

    e.data_offset = readCustomUnsignedShift(f, offsetsize, shiftsize);
    e.compressed_size = readCustomUnsigned(f, compsize);
    std::uint64_t sizeDiff = 0;
    if (sizediffsize != 0) sizeDiff = readCustomUnsigned(f, sizediffsize);
    e.uncompressed_size = e.compressed_size + sizeDiff;

    // Skip unknown field if present.
    if (unknownsize) {
      (void)readCustomUnsigned(f, unknownsize);
    }

    if (idx.file_name_length) {
      std::vector<std::uint8_t> nameBytes;
      if (!readBytes(f, nameBytes, idx.file_name_length)) {
        if (err) *err = "Truncated directory entry (name).";
        return std::nullopt;
      }
      e.name = bytesToCString(nameBytes);
    }
    // Magic sniffing (best-effort) to improve UX naming.
    // Only sniff when the data_offset looks sane and we can read a small header.
// Type sniffing (fast, best-effort). For packed zlib entries, we inflate a tiny prefix to detect inner type.
if (e.data_offset + 16ull <= fileSize && e.compressed_size >= 8ull) {
  const auto backPos = f.tellg();
  const std::string pstr = astPath.string();
  const auto hint = sniffMagicPossiblyZlib(f, e.data_offset, e.compressed_size, fileSize, &pstr);
  if (hint.type) {
    std::string t = hint.type;
    // Normalize display strings (UI expects file type only).
    if (t == "AST/BGFA") t = "AST";
    if (t == "zlib") t = "ZLIB";
    e.type_hint = std::move(t);
  }
  if (hint.ext) e.ext_hint = hint.ext;
  // Restore stream position to continue directory reading.
  f.clear();
  f.seekg(backPos, std::ios::beg);
}

    // Friendly naming pipeline (EASE benchmark):
    //  1) explicit archive name (already in e.name)
    //  2) inferred friendly base from payload prefix (XML root, path-like strings, DDS dims)
    //  3) fallback File_XXXXXX
    char tmp[64];
    std::snprintf(tmp, sizeof(tmp), "File_%06u", e.index);
    std::string fileName = tmp;
    if (!e.ext_hint.empty() && !hasExtension(fileName)) fileName += e.ext_hint;

    if (e.name.empty()) e.name = fileName;
    if (!e.ext_hint.empty() && !hasExtension(e.name)) e.name += e.ext_hint;

    // Only do inference if we don't already have a real name.
    bool isSynthetic = (e.name.rfind("File_", 0) == 0);
    if (isSynthetic) {
      const std::uint64_t absOff = e.data_offset;
      auto payload = readPayloadPrefixPossiblyZlib(f, absOff, e.compressed_size, fileSize);
      std::string friendly;
      if (!payload.empty()) {
        const std::string type = e.type_hint;
        if (type == "APT" || type == "APT1") {
          friendly = inferAptExportName(payload);
          if (friendly.empty()) {
            // Fall back to generic string heuristics if no explicit export marker exists.
            friendly = inferFriendlyFromStrings(payload);
          }
        } else if (type == "XML") {
          friendly = extractXmlRootFriendly(payload);
        } else {
          friendly = inferFriendlyFromStrings(payload);
          if (friendly.empty() && (type == "DDS" || e.ext_hint == ".dds")) {
            friendly = inferFriendlyDdsDims(payload);
          }
        }
      }
      if (!friendly.empty()) {
        std::string disp = friendly;
        if (!e.ext_hint.empty() && !hasExtension(disp)) disp += e.ext_hint;
        disp += " (" + fileName + ")";
        e.display_name = std::move(disp);

        if (AstArchive::debugClassificationTrace()) {
          std::fprintf(stderr,
                       "[ASTra][name] %s @0x%llX -> %s\n",
                       fileName.c_str(),
                       static_cast<unsigned long long>(absOff),
                       e.display_name.c_str());
        }
      }
    }


    idx.entries.push_back(std::move(e));
  }

  (void)magicV;
  (void)fakeFileCount;
  (void)nametype;
  {
    std::lock_guard<std::mutex> lk(g_cacheMutex);
    g_indexCache.emplace(cacheKey, std::make_shared<AstArchive::Index>(idx));
  }
  return idx;
}


std::optional<AstArchive::Index>
AstArchive::readEmbeddedIndex(std::istream& stream,
                              std::uint64_t baseOffset,
                              std::uint64_t maxReadable,
                              std::string* err)
                                                {
  if (err) err->clear();

  auto& f = stream;
  const std::uint64_t fileSize = baseOffset + maxReadable;

  f.clear();
  f.seekg(static_cast<std::streamoff>(baseOffset), std::ios::beg);
  if (!f) {
    if (err) *err = "Failed to seek to embedded AST offset.";
    return std::nullopt;
  }

  // Header

  if (!readMagicBGFA(f)) {
    if (err) *err = "Not a BGFA/AST file (magic mismatch).";
    return std::nullopt;
  }

  Index idx;

  // The legacy tools treat everything after magic as little-endian.
  std::uint32_t magicV = 0;
  std::uint32_t fakeFileCount = 0;
  if (!readLE(f, magicV) || !readLE(f, fakeFileCount) || !readLE(f, idx.file_count)) {
    if (err) *err = "Truncated AST header.";
    return std::nullopt;
  }

  std::int64_t dirOffset = 0;
  std::int64_t dirSize = 0;
  if (!readLE(f, dirOffset) || !readLE(f, dirSize)) {
    if (err) *err = "Truncated AST header (dir offsets).";
    return std::nullopt;
  }
  idx.dir_offset = static_cast<std::uint64_t>(dirOffset);
  idx.dir_size = static_cast<std::uint64_t>(dirSize);

  std::uint8_t nametype = 0;
  std::uint8_t flagtype = 0;
  std::uint8_t tagsize = 0;
  std::uint8_t offsetsize = 0;
  std::uint8_t compsize = 0;
  std::uint8_t sizediffsize = 0;
  std::uint8_t shiftsize = 0;
  std::uint8_t unknownsize = 0;
  if (!readLE(f, nametype) || !readLE(f, flagtype) || !readLE(f, tagsize) ||
      !readLE(f, offsetsize) || !readLE(f, compsize) || !readLE(f, sizediffsize) ||
      !readLE(f, shiftsize) || !readLE(f, unknownsize)) {
    if (err) *err = "Truncated AST header (field sizes).";
    return std::nullopt;
  }

  if (!readLE(f, idx.tag_count) || !readLE(f, idx.file_name_length)) {
    if (err) *err = "Truncated AST header (tag/name counts).";
    return std::nullopt;
  }

  // Basic sanity limits to avoid pathological reads.
  if (idx.file_count > 5'000'000u) {
    if (err) *err = "Unreasonable file count; refusing to parse.";
    return std::nullopt;
  }
  if (idx.file_name_length > 4096u) {
    if (err) *err = "Unreasonable filename length; refusing to parse.";
    return std::nullopt;
  }
  if (tagsize > 32u || offsetsize > 16u || compsize > 16u || sizediffsize > 16u) {
    if (err) *err = "Unsupported header field sizes.";
    return std::nullopt;
  }

  // Directory bounds checks (crash safety).
  // Embedded header offsets are relative to the embedded BGFA base.
  if (idx.dir_offset >= maxReadable) {
    if (err) *err = "Directory offset out of embedded bounds.";
    return std::nullopt;
  }
  if (idx.dir_size != 0 && (idx.dir_offset + idx.dir_size) > maxReadable) {
    if (err) *err = "Directory range out of embedded bounds.";
    return std::nullopt;
  }

  // Jump to directory.
  // Embedded AST header offsets are relative to the embedded BGFA base.
  f.seekg(static_cast<std::streamoff>(baseOffset + idx.dir_offset), std::ios::beg);
  if (!f) {
    if (err) *err = "Failed to seek to directory.";
    return std::nullopt;
  }

  // Read tag list (used for platform hints in legacy tools).
  std::vector<std::uint32_t> tags;
  tags.reserve(idx.tag_count);
  for (std::uint32_t i = 0; i < idx.tag_count; ++i) {
    std::uint32_t t = 0;
    if (!readLE(f, t)) {
      if (err) *err = "Truncated tag list.";
      return std::nullopt;
    }
    tags.push_back(t);
  }
  // Known magic tags from legacy tools.
  // XBOX: 777539666 (0x2E56B9D2)
  // PS3 : 1814246228 (0x6C28D674)
  for (auto t : tags) {
    if (t == 777539666u) idx.is_xbox = true;
    if (t == 1814246228u) idx.is_ps3 = true;
  }

  // Directory records.
  idx.entries.clear();
  idx.entries.reserve(idx.file_count);

  constexpr std::uint8_t recordFlagSize = 1; // per legacy BGFA
  const std::uint32_t recordSize =
      static_cast<std::uint32_t>(recordFlagSize) + static_cast<std::uint32_t>(flagtype) +
      static_cast<std::uint32_t>(tagsize) + static_cast<std::uint32_t>(offsetsize) +
      static_cast<std::uint32_t>(compsize) + static_cast<std::uint32_t>(sizediffsize) +
      static_cast<std::uint32_t>(unknownsize) + idx.file_name_length;
  // Directory truncation safety: if the directory region is smaller than the declared
  // record count, clamp to what can be safely read. This prevents crashes on malformed ASTs
  // while still allowing partial browsing.
  const auto curPos0 = f.tellg();
  if (curPos0 < 0) {
    if (err) *err = "tellg failed while reading embedded directory.";
    return std::nullopt;
  }
  const std::uint64_t curPos = static_cast<std::uint64_t>(curPos0);
  const std::uint64_t dirBeginAbs = baseOffset + idx.dir_offset;
  const std::uint64_t dirEnd = (idx.dir_size != 0) ? (dirBeginAbs + idx.dir_size) : fileSize;
  if (curPos > dirEnd) {
    if (err) *err = "Embedded directory cursor out of range.";
    return std::nullopt;
  }
  if (recordSize != 0) {
    const std::uint64_t remaining = dirEnd - curPos;
    const std::uint64_t maxRecords = remaining / static_cast<std::uint64_t>(recordSize);
    if (maxRecords < idx.file_count) {
      if (err) {
        *err = "Embedded directory truncated; returning partial index (" +
               std::to_string(maxRecords) + " of " + std::to_string(idx.file_count) + " entries).";
      }
      idx.file_count = static_cast<std::uint32_t>(maxRecords);
    }
  }



  // A weak sanity check: directory should be able to contain file_count records.
  if (idx.dir_size != 0 && (static_cast<std::uint64_t>(recordSize) * idx.file_count) > idx.dir_size + 64ull) {
    // Allow some wiggle room (tag list included, etc.), but still guard.
    // Don't hard fail; just proceed carefully.
  }

  for (std::uint32_t i = 0; i < idx.file_count; ++i) {
    Entry e;
    e.index = i;

    // Record flags.
    // The header exposes an additional `flagtype` width; legacy tools treat the
    // on-disk record as 1 byte (recordFlagSize) plus `flagtype` bytes.
    // If we fail to consume `flagtype`, subsequent fields become misaligned.
    const std::uint8_t flagsWidth = static_cast<std::uint8_t>(recordFlagSize + flagtype);
    e.flags = static_cast<std::uint32_t>(readCustomUnsigned(f, flagsWidth));

    // Tag bytes / ID/Hash field
    std::vector<std::uint8_t> tagBytes;
    if (!readBytes(f, tagBytes, tagsize)) {
      if (err) *err = "Truncated directory entry (tag bytes).";
      return std::nullopt;
    }
    // Best-effort interpret first 8 bytes as little-endian number.
    std::uint64_t idOrHash = 0;
    for (std::size_t b = 0; b < tagBytes.size() && b < 8; ++b) {
      idOrHash |= (static_cast<std::uint64_t>(tagBytes[b]) << (8ull * b));
    }
    e.id_or_hash = idOrHash;

    e.data_offset = readCustomUnsignedShift(f, offsetsize, shiftsize);
    e.compressed_size = readCustomUnsigned(f, compsize);
    std::uint64_t sizeDiff = 0;
    if (sizediffsize != 0) sizeDiff = readCustomUnsigned(f, sizediffsize);
    e.uncompressed_size = e.compressed_size + sizeDiff;

    // Skip unknown field if present.
    if (unknownsize) {
      (void)readCustomUnsigned(f, unknownsize);
    }

    if (idx.file_name_length) {
      std::vector<std::uint8_t> nameBytes;
      if (!readBytes(f, nameBytes, idx.file_name_length)) {
        if (err) *err = "Truncated directory entry (name).";
        return std::nullopt;
      }
      e.name = bytesToCString(nameBytes);
    }
    // Magic sniffing (best-effort) to improve UX naming.
    // Only sniff when the data_offset looks sane and we can read a small header.
// Type sniffing (fast, best-effort). For packed zlib entries, we inflate a tiny prefix to detect inner type.
const std::uint64_t absDataOffset = baseOffset + e.data_offset;
    if (absDataOffset + 16ull <= fileSize && e.compressed_size >= 8ull) {
  const auto backPos = f.tellg();
    const auto hint = sniffMagicPossiblyZlib(f, absDataOffset, e.compressed_size, fileSize, nullptr);
  if (hint.type) {
    std::string t = hint.type;
    // Normalize display strings (UI expects file type only).
    if (t == "AST/BGFA") t = "AST";
    if (t == "zlib") t = "ZLIB";
    e.type_hint = std::move(t);
  }
  if (hint.ext) e.ext_hint = hint.ext;
  // Restore stream position to continue directory reading.
  f.clear();
  f.seekg(backPos, std::ios::beg);
}

    // Friendly naming pipeline (EASE benchmark):
    //  1) explicit archive name (already in e.name)
    //  2) inferred friendly base from payload prefix (XML root, path-like strings, DDS dims)
    //  3) fallback File_XXXXXX
    char tmp[64];
    std::snprintf(tmp, sizeof(tmp), "File_%06u", e.index);
    std::string fileName = tmp;
    if (!e.ext_hint.empty() && !hasExtension(fileName)) fileName += e.ext_hint;

    if (e.name.empty()) e.name = fileName;
    if (!e.ext_hint.empty() && !hasExtension(e.name)) e.name += e.ext_hint;

    // Only do inference if we don't already have a real name.
    bool isSynthetic = (e.name.rfind("File_", 0) == 0);
    if (isSynthetic) {
      auto payload = readPayloadPrefixPossiblyZlib(f, absDataOffset, e.compressed_size, fileSize);
      std::string friendly;
      if (!payload.empty()) {
        const std::string type = e.type_hint;
        if (type == "APT" || type == "APT1") {
          friendly = inferAptExportName(payload);
          if (friendly.empty()) {
            // Fall back to generic string heuristics if no explicit export marker exists.
            friendly = inferFriendlyFromStrings(payload);
          }
        } else if (type == "XML") {
          friendly = extractXmlRootFriendly(payload);
        } else {
          friendly = inferFriendlyFromStrings(payload);
          if (friendly.empty() && (type == "DDS" || e.ext_hint == ".dds")) {
            friendly = inferFriendlyDdsDims(payload);
          }
        }
      }
      if (!friendly.empty()) {
        std::string disp = friendly;
        if (!e.ext_hint.empty() && !hasExtension(disp)) disp += e.ext_hint;
        disp += " (" + fileName + ")";
        e.display_name = std::move(disp);
      }
    }


    idx.entries.push_back(std::move(e));
  }

  (void)magicV;
  (void)fakeFileCount;
  (void)nametype;
  return idx;
}

std::optional<AstArchive::Index>
AstArchive::readEmbeddedIndexFromFile(const std::filesystem::path& parentAstPath,
                                                                      std::uint64_t baseOffset,
                                                                      std::uint64_t maxReadable,
                                                                      std::string* err) {
  if (err) err->clear();

  IndexCacheKey key{parentAstPath.string(), baseOffset, maxReadable};
  {
    std::lock_guard<std::mutex> lk(g_cacheMutex);
    auto it = g_indexCache.find(key);
    if (it != g_indexCache.end()) {
      return *it->second;
    }
  }

  std::ifstream f(parentAstPath, std::ios::binary);
  if (!f) {
    if (err) *err = "Failed to open parent AST file.";
    return std::nullopt;
  }
  auto idx = AstArchive::readEmbeddedIndex(f, baseOffset, maxReadable, err);
  if (idx) {
    std::lock_guard<std::mutex> lk(g_cacheMutex);
    g_indexCache.emplace(key, std::make_shared<AstArchive::Index>(*idx));
  }
  return idx;
}



std::optional<AstArchive::Index>
AstArchive::readEmbeddedIndexFromFileSmart(const std::filesystem::path& parentAstPath,
                                           std::uint64_t baseOffset,
                                           std::uint64_t maxReadable,
                                           std::string* err) {
  // Read a small slice to determine if this is raw BGFA or zlib-wrapped BGFA.
  std::string localErr;
  auto slice = read_file_slice(parentAstPath, baseOffset, maxReadable, err ? err : &localErr);
  if (slice.empty()) return std::nullopt;

  if (slice.size() >= 4 && slice[0] == 'B' && slice[1] == 'G' && slice[2] == 'F' && slice[3] == 'A') {
    // Raw BGFA embedded in the file at baseOffset.
    return AstArchive::readEmbeddedIndexFromFile(parentAstPath, baseOffset, maxReadable, err);
  }

  if (slice.size() >= 2 && looks_like_zlib_cmf_flg(slice[0], slice[1])) {
    try {
      auto inflated = zlib_inflate_unknown_size(slice);
      if (inflated.size() >= 4 && inflated[0] == 'B' && inflated[1] == 'G' && inflated[2] == 'F' && inflated[3] == 'A') {
        std::string s(reinterpret_cast<const char*>(inflated.data()), inflated.size());
        std::istringstream ss(s, std::ios::binary);
        return AstArchive::readEmbeddedIndex(ss, 0ull, static_cast<std::uint64_t>(inflated.size()), err);
      }
      if (err) *err = "Inflated payload is not BGFA";
      return std::nullopt;
    } catch (const std::exception& ex) {
      if (err) *err = std::string("Zlib inflate failed: ") + ex.what();
      return std::nullopt;
    }
  }

  if (err) *err = "Embedded payload is neither BGFA nor zlib-wrapped BGFA";
  return std::nullopt;
}

} // namespace gf::core
