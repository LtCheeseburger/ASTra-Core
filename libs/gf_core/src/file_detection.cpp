#include <gf/core/file_detection.hpp>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>

namespace gf::core {

namespace {

// ── Internal byte-access helpers ──────────────────────────────────────────────

static std::uint8_t byteAt(std::span<const std::uint8_t> s, std::size_t i) noexcept {
  return (i < s.size()) ? s[i] : 0;
}

static bool matchMagic(std::span<const std::uint8_t> s, std::size_t offset,
                        const char* magic, std::size_t len) noexcept {
  if (s.size() < offset + len) return false;
  return std::memcmp(s.data() + offset, magic, len) == 0;
}

// Extract the lowercase file extension from a filename/path string.
// Returns an empty string if no extension is found.
static std::string extractExtLower(std::string_view filename) noexcept {
  if (filename.empty()) return {};
  // Find the last dot after the last slash/backslash.
  const std::size_t lastSep = filename.find_last_of("/\\");
  const std::string_view base = (lastSep == std::string_view::npos)
                                  ? filename
                                  : filename.substr(lastSep + 1);
  const std::size_t dot = base.rfind('.');
  if (dot == std::string_view::npos) return {};
  std::string ext(base.substr(dot + 1));
  for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return ext;
}

} // anonymous namespace

// ── looksLikeText ─────────────────────────────────────────────────────────────

bool looksLikeText(std::span<const std::uint8_t> bytes, double threshold) noexcept {
  if (bytes.empty()) return false;
  const std::size_t sampleSize = std::min(bytes.size(), std::size_t{512});
  std::size_t printable = 0;
  for (std::size_t i = 0; i < sampleSize; ++i) {
    const unsigned char c = bytes[i];
    // Accept normal printable ASCII, tab, LF, CR.
    if (c == 0x09 || c == 0x0A || c == 0x0D ||
        (c >= 0x20 && c <= 0x7E)) {
      ++printable;
    } else if (c >= 0x80) {
      // Permissive: treat high bytes as possibly UTF-8 multibyte.
      ++printable;
    } else if (c == 0x00) {
      // Null byte almost always means binary.
      return false;
    }
  }
  return static_cast<double>(printable) / static_cast<double>(sampleSize) >= threshold;
}

// ── recommendedViewerForKind ──────────────────────────────────────────────────

std::string_view recommendedViewerForKind(FileKind kind) noexcept {
  switch (kind) {
    case FileKind::Dds:
    case FileKind::P3r:
    case FileKind::Xpr2:
    case FileKind::Xpr:
    case FileKind::ImageGeneric:
      return kViewerTexture;
    case FileKind::Apt:
    case FileKind::AptConst:
      return kViewerApt;
    case FileKind::Dat:
      return kViewerDat;
    case FileKind::Rsf:
      return kViewerRsf;
    case FileKind::Xml:
    case FileKind::Json:
    case FileKind::IniLike:
    case FileKind::TextPlain:
      return kViewerText;
    case FileKind::Ast:
      return kViewerHex; // AST containers expand in the tree; hex is the raw view
    case FileKind::BinaryUnknown:
    case FileKind::Unknown:
    default:
      return kViewerInspector;
  }
}

// ── kindDisplayName ───────────────────────────────────────────────────────────

std::string_view kindDisplayName(FileKind kind) noexcept {
  switch (kind) {
    case FileKind::Unknown:       return "Unknown";
    case FileKind::BinaryUnknown: return "Binary (unknown)";
    case FileKind::Ast:           return "AST/BGFA container";
    case FileKind::Dds:           return "DDS texture";
    case FileKind::P3r:           return "P3R (EA DDS wrapper)";
    case FileKind::Xpr2:          return "XPR2 (Xbox 360 texture)";
    case FileKind::Xpr:           return "XPR (Xbox texture)";
    case FileKind::Apt:           return "APT (EA UI)";
    case FileKind::AptConst:      return "APT CONST";
    case FileKind::Dat:           return "DAT (EA geometry)";
    case FileKind::Rsf:           return "RSF (EA material)";
    case FileKind::Xml:           return "XML";
    case FileKind::Json:          return "JSON";
    case FileKind::IniLike:       return "INI/config";
    case FileKind::TextPlain:     return "Plain text";
    case FileKind::ImageGeneric:  return "Image";
    default:                      return "Unknown";
  }
}

// ── detectFileKind ────────────────────────────────────────────────────────────

FileDetectionResult detectFileKind(
    std::span<const std::uint8_t>   headerBytes,
    std::optional<std::string_view> filename) noexcept {

  const std::string ext = filename ? extractExtLower(*filename) : std::string{};
  FileDetectionResult r;

  // ── Priority 1: Strong unambiguous magic bytes ────────────────────────────

  // BGFA / AST container
  if (headerBytes.size() >= 4 &&
      byteAt(headerBytes, 0) == 'B' && byteAt(headerBytes, 1) == 'G' &&
      byteAt(headerBytes, 2) == 'F' && byteAt(headerBytes, 3) == 'A') {
    r.kind = FileKind::Ast;
    r.confidence = 99;
    r.reason = "BGFA magic";
    r.isBinary = true;
    r.recommendedViewer = kViewerHex;
    return r;
  }

  // DDS surface
  if (matchMagic(headerBytes, 0, "DDS ", 4)) {
    r.kind = FileKind::Dds;
    r.confidence = 99;
    r.reason = "DDS magic";
    r.isBinary = true;
    r.recommendedViewer = kViewerTexture;
    return r;
  }

  // XPR2 (Xbox 360)
  if (matchMagic(headerBytes, 0, "XPR2", 4)) {
    r.kind = FileKind::Xpr2;
    r.confidence = 99;
    r.reason = "XPR2 magic";
    r.isBinary = true;
    r.recommendedViewer = kViewerTexture;
    return r;
  }

  // XPR (original Xbox) — must check before generic 'X' patterns
  if (matchMagic(headerBytes, 0, "XPR\0", 4)) {
    r.kind = FileKind::Xpr;
    r.confidence = 99;
    r.reason = "XPR magic";
    r.isBinary = true;
    r.recommendedViewer = kViewerTexture;
    return r;
  }

  // EA P3R / P3r texture wrapper ("P3Rx" or "p3Rx" — case may vary)
  if (headerBytes.size() >= 3) {
    const auto b0 = byteAt(headerBytes, 0);
    const auto b1 = byteAt(headerBytes, 1);
    const auto b2 = byteAt(headerBytes, 2);
    const bool p3rMagic = (b0 == 'P' || b0 == 'p') && b1 == '3' && (b2 == 'R' || b2 == 'r');
    if (p3rMagic) {
      r.kind = FileKind::P3r;
      r.confidence = 95;
      r.reason = "P3R magic";
      r.isBinary = true;
      r.recommendedViewer = kViewerTexture;
      return r;
    }
  }

  // EA RSF material — BINARY form: "RSF" followed by a null byte or version byte.
  // IMPORTANT: XML-like RSF state files (ASCII) MUST NOT match here.
  // We require the 4th byte to be a binary value (0x00..0x08) to distinguish
  // binary RSF from a hypothetical plaintext file that happens to start with "RSF".
  if (headerBytes.size() >= 4 &&
      byteAt(headerBytes, 0) == 'R' &&
      byteAt(headerBytes, 1) == 'S' &&
      byteAt(headerBytes, 2) == 'F' &&
      byteAt(headerBytes, 3) <= 0x08) {  // binary version byte, not printable ASCII
    r.kind = FileKind::Rsf;
    r.confidence = 99;
    r.reason = "RSF binary magic";
    r.isBinary = true;
    r.recommendedViewer = kViewerRsf;
    return r;
  }

  // EA APT CONST companion: "Apt cons…"
  if (matchMagic(headerBytes, 0, "Apt cons", 8)) {
    r.kind = FileKind::AptConst;
    r.confidence = 99;
    r.reason = "APT CONST magic";
    r.isBinary = true;
    r.recommendedViewer = kViewerApt;
    return r;
  }

  // EA APT UI file: "Apt\x01" or "APT " style magic
  if (headerBytes.size() >= 4 &&
      byteAt(headerBytes, 0) == 'A' &&
      byteAt(headerBytes, 1) == 'p' &&
      byteAt(headerBytes, 2) == 't') {
    r.kind = FileKind::Apt;
    r.confidence = 95;
    r.reason = "APT magic prefix";
    r.isBinary = true;
    r.recommendedViewer = kViewerApt;
    return r;
  }

  // PNG
  if (headerBytes.size() >= 4 &&
      byteAt(headerBytes, 0) == 0x89 && byteAt(headerBytes, 1) == 'P' &&
      byteAt(headerBytes, 2) == 'N'  && byteAt(headerBytes, 3) == 'G') {
    r.kind = FileKind::ImageGeneric;
    r.confidence = 99;
    r.reason = "PNG magic";
    r.isBinary = true;
    r.recommendedViewer = kViewerTexture;
    return r;
  }

  // BMP
  if (headerBytes.size() >= 2 &&
      byteAt(headerBytes, 0) == 'B' && byteAt(headerBytes, 1) == 'M') {
    r.kind = FileKind::ImageGeneric;
    r.confidence = 90;
    r.reason = "BMP magic";
    r.isBinary = true;
    r.recommendedViewer = kViewerTexture;
    return r;
  }

  // JPEG
  if (headerBytes.size() >= 3 &&
      byteAt(headerBytes, 0) == 0xFF && byteAt(headerBytes, 1) == 0xD8 &&
      byteAt(headerBytes, 2) == 0xFF) {
    r.kind = FileKind::ImageGeneric;
    r.confidence = 99;
    r.reason = "JPEG magic";
    r.isBinary = true;
    r.recommendedViewer = kViewerTexture;
    return r;
  }

  // ── Priority 2: XML / JSON / text content sniff (before extension) ─────────

  // XML declaration: "<?xml" or starting tag "<"
  if (headerBytes.size() >= 5 &&
      byteAt(headerBytes, 0) == '<' &&
      byteAt(headerBytes, 1) == '?' &&
      byteAt(headerBytes, 2) == 'x' &&
      byteAt(headerBytes, 3) == 'm' &&
      byteAt(headerBytes, 4) == 'l') {
    r.kind = FileKind::Xml;
    r.confidence = 99;
    r.reason = "<?xml declaration";
    r.isBinary = false;
    r.recommendedViewer = kViewerText;
    return r;
  }

  // XML-like: starts with '<' followed by a letter (opening tag or comment/CDATA)
  if (headerBytes.size() >= 2 &&
      byteAt(headerBytes, 0) == '<' &&
      (std::isalpha(static_cast<unsigned char>(byteAt(headerBytes, 1))) ||
       byteAt(headerBytes, 1) == '!' || byteAt(headerBytes, 1) == '?')) {
    r.kind = FileKind::Xml;
    r.confidence = 85;
    r.reason = "XML-like opening tag";
    r.isBinary = false;
    r.recommendedViewer = kViewerText;
    return r;
  }

  // JSON: starts with '{' or '[', accounting for leading whitespace
  {
    std::size_t pos = 0;
    while (pos < headerBytes.size() && std::isspace(static_cast<unsigned char>(byteAt(headerBytes, pos)))) ++pos;
    if (pos < headerBytes.size()) {
      const auto first = byteAt(headerBytes, pos);
      if (first == '{' || first == '[') {
        r.kind = FileKind::Json;
        r.confidence = 70;
        r.reason = "JSON-like opening character";
        r.isBinary = false;
        r.recommendedViewer = kViewerText;
        return r;
      }
    }
  }

  // ── Priority 3: Extension-based detection (no magic matched yet) ───────────

  if (!ext.empty()) {
    // Texture extensions
    if (ext == "dds") {
      r.kind = FileKind::Dds;
      r.confidence = 70;
      r.reason = ".dds extension (no magic)";
      r.isBinary = true;
      r.recommendedViewer = kViewerTexture;
      return r;
    }
    if (ext == "xpr2" || ext == "xpr") {
      r.kind = FileKind::Xpr2;
      r.confidence = 70;
      r.reason = ".xpr2/.xpr extension (no magic)";
      r.isBinary = true;
      r.recommendedViewer = kViewerTexture;
      return r;
    }
    if (ext == "tga" || ext == "png" || ext == "bmp" || ext == "jpg" || ext == "jpeg") {
      r.kind = FileKind::ImageGeneric;
      r.confidence = 65;
      r.reason = "Image extension (no magic)";
      r.isBinary = true;
      r.recommendedViewer = kViewerTexture;
      return r;
    }

    // Format extensions
    if (ext == "apt" || ext == "apt1") {
      r.kind = FileKind::Apt;
      r.confidence = 70;
      r.reason = ".apt extension (no magic)";
      r.isBinary = true;
      r.recommendedViewer = kViewerApt;
      return r;
    }
    if (ext == "const") {
      r.kind = FileKind::AptConst;
      r.confidence = 70;
      r.reason = ".const extension (no magic)";
      r.isBinary = true;
      r.recommendedViewer = kViewerApt;
      return r;
    }
    if (ext == "dat") {
      r.kind = FileKind::Dat;
      r.confidence = 60;  // DAT has no strong magic; extension alone is weak
      r.reason = ".dat extension (no magic)";
      r.isBinary = true;
      r.recommendedViewer = kViewerDat;
      return r;
    }
    // RSF by extension — allowed, but confidence is moderate.
    // XML-format RSF state files are still RSF by ext; route them to RSF viewer.
    if (ext == "rsf" || ext == "rsg") {
      r.kind = FileKind::Rsf;
      r.confidence = 75;
      r.reason = ".rsf/.rsg extension (no binary magic)";
      // isBinary depends on content — check below
      r.isBinary = !looksLikeText(headerBytes);
      r.recommendedViewer = kViewerRsf;
      return r;
    }
    if (ext == "ast") {
      r.kind = FileKind::Ast;
      r.confidence = 70;
      r.reason = ".ast extension (no BGFA magic)";
      r.isBinary = true;
      r.recommendedViewer = kViewerHex;
      return r;
    }

    // Text extensions
    if (ext == "xml" || ext == "xsd" || ext == "xsl" || ext == "xslt" || ext == "svg") {
      r.kind = FileKind::Xml;
      r.confidence = 75;
      r.reason = ".xml extension";
      r.isBinary = false;
      r.recommendedViewer = kViewerText;
      return r;
    }
    if (ext == "json") {
      r.kind = FileKind::Json;
      r.confidence = 75;
      r.reason = ".json extension";
      r.isBinary = false;
      r.recommendedViewer = kViewerText;
      return r;
    }
    if (ext == "ini" || ext == "cfg" || ext == "conf" || ext == "config" || ext == "properties") {
      r.kind = FileKind::IniLike;
      r.confidence = 65;
      r.reason = "INI/config extension";
      r.isBinary = false;
      r.recommendedViewer = kViewerText;
      return r;
    }
    if (ext == "txt" || ext == "text" || ext == "log" || ext == "md" ||
        ext == "csv" || ext == "tsv" || ext == "lua" || ext == "js" ||
        ext == "css" || ext == "html" || ext == "htm" || ext == "py" ||
        ext == "yaml" || ext == "yml") {
      r.kind = FileKind::TextPlain;
      r.confidence = 65;
      r.reason = "Text file extension";
      r.isBinary = false;
      r.recommendedViewer = kViewerText;
      return r;
    }
  }

  // ── Priority 4: Content sniffing for text vs binary ────────────────────────

  if (!headerBytes.empty()) {
    if (looksLikeText(headerBytes, 0.90)) {
      r.kind = FileKind::TextPlain;
      r.confidence = 50;
      r.reason = "Content sniff: high printable ratio";
      r.isBinary = false;
      r.recommendedViewer = kViewerText;
      return r;
    }

    // Null byte at position 0 is a strong binary signal.
    if (byteAt(headerBytes, 0) == 0x00 || !looksLikeText(headerBytes, 0.50)) {
      r.kind = FileKind::BinaryUnknown;
      r.confidence = 40;
      r.reason = "Content sniff: binary signature";
      r.isBinary = true;
      r.recommendedViewer = kViewerInspector;
      return r;
    }
  }

  // ── Priority 5: Unknown ────────────────────────────────────────────────────

  r.kind = FileKind::Unknown;
  r.confidence = 0;
  r.reason = "No detection signal";
  r.isBinary = false;
  r.recommendedViewer = kViewerInspector;
  return r;
}

} // namespace gf::core
