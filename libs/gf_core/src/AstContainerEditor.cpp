#include <gf/core/AstContainerEditor.hpp>

#include <gf/core/safe_write.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <limits>
#include <type_traits>
#include <zlib.h>

namespace gf::core {

namespace {

static bool readBytes(std::istream& is, std::vector<std::uint8_t>& out, std::size_t n) {
  out.resize(n);
  if (n == 0) return true;
  return static_cast<bool>(is.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(n)));
}

template <typename T>
static bool readLE(std::istream& is, T& out) {
  static_assert(std::is_integral_v<T> || std::is_enum_v<T>);
  std::array<std::uint8_t, sizeof(T)> buf{};
  if (!is.read(reinterpret_cast<char*>(buf.data()), buf.size())) return false;
  std::uint64_t v = 0;
  for (std::size_t i = 0; i < buf.size(); ++i) v |= (static_cast<std::uint64_t>(buf[i]) << (8ull * i));
  out = static_cast<T>(v);
  return true;
}

static bool readMagicBGFA(std::istream& is) {
  char m[4]{};
  if (!is.read(m, 4)) return false;
  return (m[0] == 'B' && m[1] == 'G' && m[2] == 'F' && m[3] == 'A');
}

static std::uint64_t readCustomUnsigned(std::istream& is, std::uint8_t byteCount) {
  std::uint64_t v = 0;
  for (std::uint8_t i = 0; i < byteCount; ++i) {
    std::uint8_t b = 0;
    if (!readLE(is, b)) return 0;
    v |= (static_cast<std::uint64_t>(b) << (8ull * i));
  }
  return v;
}

static std::uint64_t readCustomUnsignedShift(std::istream& is, std::uint8_t byteCount, std::uint8_t shift) {
  const std::uint64_t base = readCustomUnsigned(is, byteCount);
  if (shift >= 64) return 0;
  return (base << shift);
}

static void writeLE(std::vector<std::uint8_t>& out, std::uint64_t v, std::size_t bytes) {
  for (std::size_t i = 0; i < bytes; ++i) out.push_back(static_cast<std::uint8_t>((v >> (8ull * i)) & 0xFFu));
}

template <typename T>
static void writeLE(std::vector<std::uint8_t>& out, T v) {
  static_assert(std::is_integral_v<T> || std::is_enum_v<T>);
  writeLE(out, static_cast<std::uint64_t>(v), sizeof(T));
}

static bool writeLE(std::ostream& os, std::uint64_t v, std::size_t bytes) {
  for (std::size_t i = 0; i < bytes; ++i) {
    const char b = static_cast<char>((v >> (8ull * i)) & 0xFFu);
    if (!os.write(&b, 1)) return false;
  }
  return true;
}

template <typename T>
static bool writeLE(std::ostream& os, T v) {
  static_assert(std::is_integral_v<T> || std::is_enum_v<T>);
  return writeLE(os, static_cast<std::uint64_t>(v), sizeof(T));
}

static void patchLE(std::vector<std::uint8_t>& out, std::size_t offset, std::uint64_t v, std::size_t bytes) {
  for (std::size_t i = 0; i < bytes; ++i) {
    out[offset + i] = static_cast<std::uint8_t>((v >> (8ull * i)) & 0xFFu);
  }
}

static bool looks_like_zlib_stream(std::span<const std::uint8_t> in) {
  if (in.size() < 2) return false;
  const std::uint8_t cmf = in[0];
  const std::uint8_t flg = in[1];
  const int cm = (cmf & 0x0F);
  const int cinfo = (cmf >> 4);
  const bool fdict = ((flg & 0x20) != 0);
  if (cm != 8) return false;
  if (cinfo > 7) return false;
  if (fdict) return false;
  const int vv = (static_cast<int>(cmf) << 8) | static_cast<int>(flg);
  return (vv % 31) == 0;
}

static std::vector<std::uint8_t> zlib_deflate(std::span<const std::uint8_t> in, std::string* err) {
  z_stream zs{};
  int rc = ::deflateInit(&zs, Z_BEST_COMPRESSION);
  if (rc != Z_OK) {
    if (err) *err = "deflateInit failed";
    return {};
  }

  zs.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(in.data()));
  zs.avail_in = static_cast<uInt>(std::min<std::size_t>(in.size(), static_cast<std::size_t>(std::numeric_limits<uInt>::max())));

  std::vector<std::uint8_t> out;
  out.reserve(in.size() / 2 + 64);

  std::array<std::uint8_t, 64 * 1024> tmp{};
  int flush = Z_NO_FLUSH;
  while (true) {
    zs.next_out = reinterpret_cast<Bytef*>(tmp.data());
    zs.avail_out = static_cast<uInt>(tmp.size());
    flush = (zs.avail_in == 0) ? Z_FINISH : Z_NO_FLUSH;
    rc = ::deflate(&zs, flush);
    const std::size_t produced = tmp.size() - static_cast<std::size_t>(zs.avail_out);
    if (produced) out.insert(out.end(), tmp.data(), tmp.data() + produced);
    if (rc == Z_STREAM_END) break;
    if (rc != Z_OK) {
      ::deflateEnd(&zs);
      if (err) *err = "deflate failed";
      return {};
    }
  }

  ::deflateEnd(&zs);
  return out;
}

static void align_to(std::vector<std::uint8_t>& out, std::uint64_t alignment) {
  if (alignment <= 1) return;
  const std::uint64_t cur = static_cast<std::uint64_t>(out.size());
  const std::uint64_t pad = (alignment - (cur % alignment)) % alignment;
  out.insert(out.end(), static_cast<std::size_t>(pad), 0);
}

static bool align_to(std::ostream& os, std::uint64_t alignment, std::uint64_t cur) {
  if (alignment <= 1) return true;
  const std::uint64_t pad = (alignment - (cur % alignment)) % alignment;
  if (pad == 0) return true;
  std::array<char, 4096> zeros{};
  std::uint64_t remaining = pad;
  while (remaining > 0) {
    const std::size_t chunk = static_cast<std::size_t>(std::min<std::uint64_t>(remaining, zeros.size()));
    if (!os.write(zeros.data(), static_cast<std::streamsize>(chunk))) return false;
    remaining -= chunk;
  }
  return true;
}

} // namespace


std::optional<AstContainerEditor> AstContainerEditor::load(const std::filesystem::path& astPath,
                                                          std::string* err) {
  if (err) err->clear();

  std::ifstream f(astPath, std::ios::binary);
  if (!f) {
    if (err) *err = "Failed to open AST file.";
    return std::nullopt;
  }

  if (!readMagicBGFA(f)) {
    if (err) *err = "Not a BGFA/AST file (magic mismatch).";
    return std::nullopt;
  }

  AstContainerEditor ed;
  ed.m_path = astPath;

  Header h;
  if (!readLE(f, h.magicV) || !readLE(f, h.fakeFileCount) || !readLE(f, h.fileCount)) {
    if (err) *err = "Truncated AST header.";
    return std::nullopt;
  }

  std::int64_t dirOff = 0;
  std::int64_t dirSz = 0;
  if (!readLE(f, dirOff) || !readLE(f, dirSz)) {
    if (err) *err = "Truncated AST header (dir offsets).";
    return std::nullopt;
  }
  h.dirOffset = static_cast<std::uint64_t>(dirOff);
  h.dirSize = static_cast<std::uint64_t>(dirSz);

  if (!readLE(f, h.nametype) || !readLE(f, h.flagtype) || !readLE(f, h.tagsize) ||
      !readLE(f, h.offsetsize) || !readLE(f, h.compsize) || !readLE(f, h.sizediffsize) ||
      !readLE(f, h.shiftsize) || !readLE(f, h.unknownsize)) {
    if (err) *err = "Truncated AST header (field sizes).";
    return std::nullopt;
  }
  if (!readLE(f, h.tagCount) || !readLE(f, h.fileNameLength)) {
    if (err) *err = "Truncated AST header (tag/name counts).";
    return std::nullopt;
  }

  // Jump to directory.
  f.seekg(static_cast<std::streamoff>(h.dirOffset), std::ios::beg);
  if (!f) {
    if (err) *err = "Failed to seek to directory.";
    return std::nullopt;
  }

  ed.m_tags.clear();
  ed.m_tags.reserve(h.tagCount);
  for (std::uint32_t i = 0; i < h.tagCount; ++i) {
    std::uint32_t t = 0;
    if (!readLE(f, t)) {
      if (err) *err = "Truncated tag list.";
      return std::nullopt;
    }
    ed.m_tags.push_back(t);
  }

  const std::uint8_t recordFlagSize = 1;
  const std::uint8_t flagsWidth = static_cast<std::uint8_t>(recordFlagSize + h.flagtype);

  ed.m_entries.clear();
  ed.m_entries.reserve(h.fileCount);

  for (std::uint32_t i = 0; i < h.fileCount; ++i) {
    Entry e;
    e.index = i;
    e.flags = static_cast<std::uint32_t>(readCustomUnsigned(f, flagsWidth));

    if (!readBytes(f, e.tagBytes, h.tagsize)) {
      if (err) *err = "Truncated directory entry (tag bytes).";
      return std::nullopt;
    }

    e.dataOffset = readCustomUnsignedShift(f, h.offsetsize, h.shiftsize);
    e.compressedSize = readCustomUnsigned(f, h.compsize);

    std::uint64_t sizeDiff = 0;
    if (h.sizediffsize) sizeDiff = readCustomUnsigned(f, h.sizediffsize);
    e.uncompressedSize = e.compressedSize + sizeDiff;

    if (h.unknownsize) {
      e.unknownField = readCustomUnsigned(f, h.unknownsize);
    }

    if (!readBytes(f, e.nameBytes, h.fileNameLength)) {
      if (err) *err = "Truncated directory entry (name).";
      return std::nullopt;
    }

    ed.m_entries.push_back(std::move(e));
  }

  // Read payload bytes for each entry.
  for (auto& e : ed.m_entries) {
    e.storedBytes.clear();
    if (e.compressedSize == 0) continue;
    f.clear();
    f.seekg(static_cast<std::streamoff>(e.dataOffset), std::ios::beg);
    if (!f) {
      if (err) *err = "Failed to seek to entry payload.";
      return std::nullopt;
    }
    if (!readBytes(f, e.storedBytes, static_cast<std::size_t>(e.compressedSize))) {
      if (err) *err = "Failed to read entry payload.";
      return std::nullopt;
    }
  }

  ed.m_header = h;
  return ed;
}


bool AstContainerEditor::replaceEntryBytes(std::uint32_t entryIndex,
                                          std::span<const std::uint8_t> newBytes,
                                          ReplaceMode mode,
                                          std::string* err) {
  if (err) err->clear();
  if (entryIndex >= m_entries.size()) {
    if (err) *err = "Entry index out of range.";
    return false;
  }
  auto& e = m_entries[entryIndex];

  std::vector<std::uint8_t> stored;
  if (mode == ReplaceMode::PreserveZlibIfPresent && looks_like_zlib_stream(e.storedBytes)) {
    stored = zlib_deflate(newBytes, err);
    if (stored.empty() && !newBytes.empty()) {
      if (err && err->empty()) *err = "Failed to zlib-compress replacement.";
      return false;
    }
    e.uncompressedSize = static_cast<std::uint64_t>(newBytes.size());
  } else {
    stored.assign(newBytes.begin(), newBytes.end());
    e.uncompressedSize = static_cast<std::uint64_t>(stored.size());
  }

  e.storedBytes = std::move(stored);
  e.compressedSize = static_cast<std::uint64_t>(e.storedBytes.size());
  return true;
}


// Write one directory entry's bytes into `out`.
static void writeDirectoryEntry(std::vector<std::uint8_t>& out,
                                const AstContainerEditor::Entry& e,
                                const AstContainerEditor::Header& h,
                                std::uint64_t newOffset) {
  const std::uint8_t flagsWidth = static_cast<std::uint8_t>(1u + h.flagtype);

  writeLE(out, static_cast<std::uint64_t>(e.flags), flagsWidth);

  // tag bytes — always exactly tagsize (pad/truncate if needed)
  if (e.tagBytes.size() != h.tagsize) {
    std::vector<std::uint8_t> tmp = e.tagBytes;
    tmp.resize(h.tagsize, 0);
    out.insert(out.end(), tmp.begin(), tmp.end());
  } else {
    out.insert(out.end(), e.tagBytes.begin(), e.tagBytes.end());
  }

  // stored offset is right-shifted by shiftsize
  const std::uint64_t base = (h.shiftsize >= 64) ? 0ull : (newOffset >> h.shiftsize);
  writeLE(out, base, h.offsetsize);

  writeLE(out, e.compressedSize, h.compsize);

  if (h.sizediffsize) {
    const std::uint64_t sizeDiff = (e.uncompressedSize > e.compressedSize)
                                      ? (e.uncompressedSize - e.compressedSize)
                                      : 0ull;
    writeLE(out, sizeDiff, h.sizediffsize);
  }
  if (h.unknownsize) {
    writeLE(out, e.unknownField, h.unknownsize);
  }

  // name bytes — always exactly fileNameLength
  if (e.nameBytes.size() != h.fileNameLength) {
    std::vector<std::uint8_t> tmp = e.nameBytes;
    tmp.resize(h.fileNameLength, 0);
    out.insert(out.end(), tmp.begin(), tmp.end());
  } else {
    out.insert(out.end(), e.nameBytes.begin(), e.nameBytes.end());
  }
}

static bool writeDirectoryEntry(std::ostream& os,
                                const AstContainerEditor::Entry& e,
                                const AstContainerEditor::Header& h,
                                std::uint64_t newOffset) {
  const std::uint8_t flagsWidth = static_cast<std::uint8_t>(1u + h.flagtype);

  if (!writeLE(os, static_cast<std::uint64_t>(e.flags), flagsWidth)) return false;

  if (e.tagBytes.size() != h.tagsize) {
    std::vector<std::uint8_t> tmp = e.tagBytes;
    tmp.resize(h.tagsize, 0);
    if (!tmp.empty() && !os.write(reinterpret_cast<const char*>(tmp.data()), static_cast<std::streamsize>(tmp.size()))) return false;
  } else if (!e.tagBytes.empty()) {
    if (!os.write(reinterpret_cast<const char*>(e.tagBytes.data()), static_cast<std::streamsize>(e.tagBytes.size()))) return false;
  }

  const std::uint64_t base = (h.shiftsize >= 64) ? 0ull : (newOffset >> h.shiftsize);
  if (!writeLE(os, base, h.offsetsize)) return false;
  if (!writeLE(os, e.compressedSize, h.compsize)) return false;

  if (h.sizediffsize) {
    const std::uint64_t sizeDiff = (e.uncompressedSize > e.compressedSize)
                                      ? (e.uncompressedSize - e.compressedSize)
                                      : 0ull;
    if (!writeLE(os, sizeDiff, h.sizediffsize)) return false;
  }
  if (h.unknownsize) {
    if (!writeLE(os, e.unknownField, h.unknownsize)) return false;
  }

  if (e.nameBytes.size() != h.fileNameLength) {
    std::vector<std::uint8_t> tmp = e.nameBytes;
    tmp.resize(h.fileNameLength, 0);
    if (!tmp.empty() && !os.write(reinterpret_cast<const char*>(tmp.data()), static_cast<std::streamsize>(tmp.size()))) return false;
  } else if (!e.nameBytes.empty()) {
    if (!os.write(reinterpret_cast<const char*>(e.nameBytes.data()), static_cast<std::streamsize>(e.nameBytes.size()))) return false;
  }

  return static_cast<bool>(os);
}

bool AstContainerEditor::rebuildToStream(std::ostream& os, std::string* err) const {
  if (err) err->clear();

  // ── Layout (matches EASE1 / original EA format) ──────────────────────────
  // [Header 64 bytes] [Directory at offset 64] [Data aligned to 1<<shiftsize]
  //
  // The header is always 64 bytes: 48 bytes of parsed fields + 16 bytes of
  // reserved zero padding.  ASTra's previous rebuild wrote only 48 bytes,
  // which caused the game engine to misread bytes 48-63 (first 16 bytes of
  // the first entry's data) as reserved header bytes.
  //
  // The directory is placed immediately after the header (dirOffset = 64).
  // Data follows the directory, aligned to DirOffsetMultiple = 2^shiftsize.
  // This is identical to EASE1's FixDirectory() / WriteData() contract.
  // ─────────────────────────────────────────────────────────────────────────

  constexpr std::uint64_t kHeaderSize = 64;
  const std::uint64_t alignment = (m_header.shiftsize >= 63) ? 1ull : (1ull << m_header.shiftsize);
  const std::uint8_t  flagsWidth = static_cast<std::uint8_t>(1u + m_header.flagtype);

  // ── Pre-calculate directory size (deterministic from header fields) ──────
  // recordSize = flagsWidth + tagsize + offsetsize + compsize
  //              + sizediffsize + unknownsize + fileNameLength
  const std::uint64_t recordSize =
      static_cast<std::uint64_t>(flagsWidth)
    + static_cast<std::uint64_t>(m_header.tagsize)
    + static_cast<std::uint64_t>(m_header.offsetsize)
    + static_cast<std::uint64_t>(m_header.compsize)
    + static_cast<std::uint64_t>(m_header.sizediffsize)
    + static_cast<std::uint64_t>(m_header.unknownsize)
    + static_cast<std::uint64_t>(m_header.fileNameLength);

  const std::uint64_t dirSize =
      static_cast<std::uint64_t>(m_header.tagCount) * 4ull
    + recordSize * static_cast<std::uint64_t>(m_entries.size());

  const std::uint64_t dirOffset = kHeaderSize; // always 64

  // ── Sort entries by original dataOffset to preserve original data order ──
  // (EASE1 does the same: DIR.Sort by DataReadOffset before writing data)
  std::vector<const Entry*> sortedByOffset;
  sortedByOffset.reserve(m_entries.size());
  for (const auto& e : m_entries) sortedByOffset.push_back(&e);
  std::stable_sort(sortedByOffset.begin(), sortedByOffset.end(),
      [](const Entry* a, const Entry* b) { return a->dataOffset < b->dataOffset; });

  // ── Pre-calculate new data offsets ───────────────────────────────────────
  // Data region starts at dirOffset + dirSize, aligned to DirOffsetMultiple.
  std::vector<std::uint64_t> newOffsets(m_entries.size(), 0);
  {
    std::uint64_t cursor = dirOffset + dirSize;
    for (const Entry* ep : sortedByOffset) {
      if (alignment > 1) {
        const std::uint64_t pad = (alignment - (cursor % alignment)) % alignment;
        cursor += pad;
      }
      newOffsets[ep->index] = cursor;
      cursor += ep->storedBytes.size(); // 0 for empty entries — that is OK
    }
  }

  // ── Write header (64 bytes) ───────────────────────────────────────────────
  if (!os.write("BGFA", 4) ||
      !writeLE(os, m_header.magicV) ||
      !writeLE(os, m_header.fakeFileCount) ||
      !writeLE(os, m_header.fileCount) ||
      !writeLE(os, dirOffset, 8) ||
      !writeLE(os, dirSize, 8) ||
      !writeLE(os, m_header.nametype) ||
      !writeLE(os, m_header.flagtype) ||
      !writeLE(os, m_header.tagsize) ||
      !writeLE(os, m_header.offsetsize) ||
      !writeLE(os, m_header.compsize) ||
      !writeLE(os, m_header.sizediffsize) ||
      !writeLE(os, m_header.shiftsize) ||
      !writeLE(os, m_header.unknownsize) ||
      !writeLE(os, m_header.tagCount) ||
      !writeLE(os, m_header.fileNameLength)) {
    if (err) *err = "Failed to write AST header.";
    return false;
  }
  std::array<char, 16> reserved{};
  if (!os.write(reserved.data(), static_cast<std::streamsize>(reserved.size()))) {
    if (err) *err = "Failed to write AST header padding.";
    return false;
  }

  // ── Write directory at offset 64 ─────────────────────────────────────────
  for (std::uint32_t t : m_tags) {
    if (!writeLE(os, t)) {
      if (err) *err = "Failed to write AST tag table.";
      return false;
    }
  }

  for (const auto& e : m_entries) {
    if (!writeDirectoryEntry(os, e, m_header, newOffsets[e.index])) {
      if (err) *err = "Failed to write AST directory entry.";
      return false;
    }
  }

  // ── Write data region (entries in original data-offset order) ────────────
  for (const Entry* ep : sortedByOffset) {
    if (!align_to(os, alignment, newOffsets[ep->index])) {
      if (err) *err = "Failed to write alignment padding.";
      return false;
    }
    if (!ep->storedBytes.empty() &&
        !os.write(reinterpret_cast<const char*>(ep->storedBytes.data()),
                  static_cast<std::streamsize>(ep->storedBytes.size()))) {
      if (err) *err = "Failed to write entry payload bytes.";
      return false;
    }
  }

  if (!os) {
    if (err) *err = "Output stream entered a failed state during rebuild.";
    return false;
  }
  return true;
}

std::vector<std::uint8_t> AstContainerEditor::rebuild(std::string* err) const {
  if (err) err->clear();

  constexpr std::uint64_t kHeaderSize = 64;
  const std::uint8_t flagsWidth = static_cast<std::uint8_t>(1u + m_header.flagtype);
  const std::uint64_t recordSize =
      static_cast<std::uint64_t>(flagsWidth)
    + static_cast<std::uint64_t>(m_header.tagsize)
    + static_cast<std::uint64_t>(m_header.offsetsize)
    + static_cast<std::uint64_t>(m_header.compsize)
    + static_cast<std::uint64_t>(m_header.sizediffsize)
    + static_cast<std::uint64_t>(m_header.unknownsize)
    + static_cast<std::uint64_t>(m_header.fileNameLength);
  const std::uint64_t dirSize =
      static_cast<std::uint64_t>(m_header.tagCount) * 4ull
    + recordSize * static_cast<std::uint64_t>(m_entries.size());
  const std::uint64_t dirOffset = kHeaderSize;
  const std::uint64_t alignment = (m_header.shiftsize >= 63) ? 1ull : (1ull << m_header.shiftsize);

  std::vector<const Entry*> sortedByOffset;
  sortedByOffset.reserve(m_entries.size());
  for (const auto& e : m_entries) sortedByOffset.push_back(&e);
  std::stable_sort(sortedByOffset.begin(), sortedByOffset.end(),
      [](const Entry* a, const Entry* b) { return a->dataOffset < b->dataOffset; });

  std::uint64_t finalSize = dirOffset + dirSize;
  for (const Entry* ep : sortedByOffset) {
    if (alignment > 1) {
      const std::uint64_t pad = (alignment - (finalSize % alignment)) % alignment;
      finalSize += pad;
    }
    finalSize += static_cast<std::uint64_t>(ep->storedBytes.size());
  }

  std::vector<std::uint8_t> out;
  if (finalSize > static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)())) {
    if (err) *err = "Rebuilt AST would exceed addressable memory size on this build.";
    return out;
  }
  out.reserve(static_cast<std::size_t>(finalSize));

  class VectorOStreamBuf final : public std::streambuf {
  public:
    explicit VectorOStreamBuf(std::vector<std::uint8_t>& out) : m_out(out) {}
  protected:
    std::streamsize xsputn(const char* s, std::streamsize n) override {
      if (n > 0) {
        const auto* begin = reinterpret_cast<const std::uint8_t*>(s);
        m_out.insert(m_out.end(), begin, begin + static_cast<std::size_t>(n));
      }
      return n;
    }
    int overflow(int ch) override {
      if (ch == traits_type::eof()) return traits_type::not_eof(ch);
      m_out.push_back(static_cast<std::uint8_t>(ch));
      return ch;
    }
  private:
    std::vector<std::uint8_t>& m_out;
  } buf(out);

  std::ostream os(&buf);
  if (!rebuildToStream(os, err)) {
    out.clear();
    return out;
  }
  return out;
}




bool AstContainerEditor::validate(std::span<const std::uint8_t> data, std::string* err) {
  if (err) err->clear();

  auto fail = [&](std::string msg) -> bool {
    if (err) *err = std::move(msg);
    return false;
  };

  // Minimum: header (64 bytes).
  if (data.size() < 64) return fail("File too small to contain a valid BGFA header (< 64 bytes).");

  // Magic
  if (data[0] != 'B' || data[1] != 'G' || data[2] != 'F' || data[3] != 'A')
    return fail("Bad magic: expected 'BGFA'.");

  // Read header fields from the validated bytes (LE).
  auto rdU32 = [&](std::size_t off) -> std::uint32_t {
    return static_cast<std::uint32_t>(data[off])
         | (static_cast<std::uint32_t>(data[off+1]) << 8)
         | (static_cast<std::uint32_t>(data[off+2]) << 16)
         | (static_cast<std::uint32_t>(data[off+3]) << 24);
  };
  auto rdU64 = [&](std::size_t off) -> std::uint64_t {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= (static_cast<std::uint64_t>(data[off+i]) << (8*i));
    return v;
  };
  auto rdU8  = [&](std::size_t off) -> std::uint8_t { return data[off]; };

  // fileCount / fakeFileCount
  const std::uint32_t fakeCount = rdU32(8);
  const std::uint32_t fileCount = rdU32(12);
  if (fakeCount != fileCount + 1) {
    return fail("fakeFileCount (" + std::to_string(fakeCount) +
                ") is not fileCount+1 (" + std::to_string(fileCount) + ").");
  }

  const std::uint64_t dirOffset = rdU64(16);
  const std::uint64_t dirSize   = rdU64(24);

  if (dirOffset < 64) return fail("dirOffset (" + std::to_string(dirOffset) + ") is less than header size (64).");
  if (dirOffset > data.size()) return fail("dirOffset (" + std::to_string(dirOffset) + ") is beyond end of file.");
  if (dirSize  > data.size()) return fail("dirSize (" + std::to_string(dirSize) + ") is larger than file.");
  if (dirOffset + dirSize > data.size())
    return fail("dirOffset + dirSize extends beyond end of file.");

  const std::uint8_t flagtype    = rdU8(33);
  const std::uint8_t tagsize     = rdU8(34);
  const std::uint8_t offsetsize  = rdU8(35);
  const std::uint8_t compsize    = rdU8(36);
  const std::uint8_t sizediff    = rdU8(37);
  const std::uint8_t shiftsize   = rdU8(38);
  const std::uint8_t unknownsize = rdU8(39);
  const std::uint32_t tagCount   = rdU32(40);
  const std::uint32_t fnameLen   = rdU32(44);

  const std::uint8_t flagsWidth  = static_cast<std::uint8_t>(1u + flagtype);
  const std::uint64_t recordSize =
      static_cast<std::uint64_t>(flagsWidth) + tagsize + offsetsize + compsize
    + sizediff + unknownsize + fnameLen;

  // Verify dirSize matches expected formula.
  const std::uint64_t expectedDirSize = static_cast<std::uint64_t>(tagCount) * 4ull
                                      + recordSize * static_cast<std::uint64_t>(fileCount);
  if (dirSize != expectedDirSize) {
    return fail("dirSize (" + std::to_string(dirSize) +
                ") does not match expected " + std::to_string(expectedDirSize) +
                " (tagCount=" + std::to_string(tagCount) +
                " recordSize=" + std::to_string(recordSize) +
                " fileCount=" + std::to_string(fileCount) + ").");
  }

  // Check alignment
  const std::uint64_t alignment = (shiftsize >= 63) ? 1ull : (1ull << shiftsize);

  // Walk directory entries and validate each dataOffset and compressedSize.
  std::uint64_t dirPos = dirOffset + static_cast<std::uint64_t>(tagCount) * 4ull;
  for (std::uint32_t i = 0; i < fileCount; ++i) {
    if (dirPos + recordSize > data.size())
      return fail("Directory entry " + std::to_string(i) + " overruns end of file.");

    // Skip flags + tag bytes.
    dirPos += flagsWidth + tagsize;

    // Read stored offset (shifted).
    std::uint64_t storedOff = 0;
    for (std::uint8_t b = 0; b < offsetsize; ++b)
      storedOff |= (static_cast<std::uint64_t>(data[static_cast<std::size_t>(dirPos + b)]) << (8*b));
    const std::uint64_t dataOff = (shiftsize >= 64) ? 0ull : (storedOff << shiftsize);
    dirPos += offsetsize;

    // Read compressedSize.
    std::uint64_t compSz = 0;
    for (std::uint8_t b = 0; b < compsize; ++b)
      compSz |= (static_cast<std::uint64_t>(data[static_cast<std::size_t>(dirPos + b)]) << (8*b));
    dirPos += compsize + sizediff + unknownsize + fnameLen;

    if (compSz == 0) continue; // empty entry — OK

    if (dataOff < dirOffset + dirSize)
      return fail("Entry " + std::to_string(i) + " dataOffset (" + std::to_string(dataOff) +
                  ") overlaps with directory region (ends at " +
                  std::to_string(dirOffset + dirSize) + ").");

    if (alignment > 1 && (dataOff % alignment) != 0)
      return fail("Entry " + std::to_string(i) + " dataOffset (" + std::to_string(dataOff) +
                  ") is not aligned to " + std::to_string(alignment) + " bytes.");

    if (dataOff + compSz > data.size())
      return fail("Entry " + std::to_string(i) + " payload (offset=" + std::to_string(dataOff) +
                  " size=" + std::to_string(compSz) + ") extends beyond end of file.");
  }

  return true;
}

std::optional<std::vector<std::uint8_t>> AstContainerEditor::getEntryStoredBytes(std::uint32_t entryIndex) const {
  for (const auto& e : m_entries) {
    if (e.index == entryIndex) return e.storedBytes;
  }
  return std::nullopt;
}

std::optional<std::vector<std::uint8_t>> AstContainerEditor::getEntryInflatedBytes(std::uint32_t entryIndex,
                                                                                   std::string* err) const {
  const Entry* target = nullptr;
  for (const auto& e : m_entries) {
    if (e.index == entryIndex) {
      target = &e;
      break;
    }
  }
  if (!target) return std::nullopt;

  // If it doesn't look like zlib, return as-is.
  if (target->storedBytes.size() < 2) return target->storedBytes;
  const bool looksZlib = (target->storedBytes[0] == 0x78); // common zlib header byte
  if (!looksZlib) return target->storedBytes;

  // Inflate to expected size when possible; fall back to a bounded attempt.
  uLongf outLen = 0;
  if (target->uncompressedSize > 0 && target->uncompressedSize <= (std::numeric_limits<uLongf>::max)()) {
    outLen = static_cast<uLongf>(target->uncompressedSize);
  } else {
    // conservative cap: 64 MiB
    outLen = 64u * 1024u * 1024u;
  }

  std::vector<std::uint8_t> out;
  out.resize(outLen);

  const int z = ::uncompress(reinterpret_cast<Bytef*>(out.data()),
                             &outLen,
                             reinterpret_cast<const Bytef*>(target->storedBytes.data()),
                             static_cast<uLong>(target->storedBytes.size()));
  if (z != Z_OK) {
    if (err) *err = "zlib inflate failed (uncompress returned " + std::to_string(z) + ")";
    return std::nullopt;
  }
  out.resize(outLen);
  return out;
}

bool AstContainerEditor::writeInPlace(std::string* err, bool makeBackup, std::uint64_t maxWriteBytes) {
  if (err) err->clear();

  SafeWriteOptions opt;
  opt.make_backup = makeBackup;
  opt.max_bytes = maxWriteBytes;

  const auto res = safe_write_streamed(
      m_path,
      [this](std::ostream& os, std::string& cbErr) {
        if (!this->rebuildToStream(os, &cbErr)) {
          if (cbErr.empty()) cbErr = "rebuildToStream() failed.";
          return false;
        }
        return true;
      },
      opt);
  if (!res.ok) {
    if (err) *err = res.message;
    return false;
  }
  return true;
}

} // namespace gf::core
