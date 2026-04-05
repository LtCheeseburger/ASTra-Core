#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// gf::core::file_detection
//
// Central, reusable file kind detection for ASTra Core.
//
// Design rules
// ────────────
//  • Detection is purely std — no Qt, no platform-specific code.
//  • Callers pass raw bytes (a leading slice is sufficient; 64 bytes is ideal,
//    but the function handles smaller buffers safely).
//  • An optional filename/extension hint improves accuracy when magic bytes are
//    absent or ambiguous, but is never the sole basis for confident detection.
//  • RSF is ONLY flagged when binary RSF magic ("RSF\0") is present or when
//    the file extension is explicitly ".rsf".  XML-like content alone MUST NOT
//    result in FileKind::Rsf.
//  • Unknown/binary files should route to an inspector, NOT a wrong editor.
// ─────────────────────────────────────────────────────────────────────────────

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace gf::core {

// ── File kind enum ────────────────────────────────────────────────────────────

enum class FileKind {
  // ── Completely unrecognised ──────────────────────────────────────────────
  Unknown,        // detection produced no useful result at all
  BinaryUnknown,  // clearly binary, format unrecognised → show inspector

  // ── Structured binary formats ────────────────────────────────────────────
  Ast,            // BGFA container (AST archive, embedded or standalone)
  Dds,            // Direct Draw Surface (plain DDS or EA-wrapped DDS body)
  P3r,            // EA P3R wrapper ("P3Rx" magic)
  Xpr2,           // Xbox 360 XPR2 tiled texture
  Xpr,            // Original Xbox XPR (opaque passthrough)
  Apt,            // EA APT UI file
  AptConst,       // EA APT companion CONST file ("Apt cons…" magic)
  Dat,            // EA DAT geometry file
  Rsf,            // EA RSF material file (binary; "RSF\0" magic or .rsf ext)

  // ── Text / markup formats ────────────────────────────────────────────────
  Xml,            // XML document (<?xml … or starts with '<')
  Json,           // JSON document (starts with '{' or '[')
  IniLike,        // INI / CFG style key=value text
  TextPlain,      // Plain text (UTF-8 / ASCII printable)

  // ── Generic image ────────────────────────────────────────────────────────
  ImageGeneric,   // PNG / BMP / TGA / JPG — recognised by magic but no editor

  // ── EA audio formats ─────────────────────────────────────────────────────
  Sbkr,           // EA SBKR sound-bank descriptor ("SBKR" magic)
  Sbbe,           // EA SBbe sample container ("SBbe" magic)
};

// ── Confidence levels ─────────────────────────────────────────────────────────
// 0  = completely uncertain
// 40 = content sniff or extension-only guess
// 70 = single strong signal (magic OR extension matching a known binary format)
// 90 = multiple corroborating signals
// 99 = definitive (unambiguous magic)

// ── Recommended viewer string constants ──────────────────────────────────────
inline constexpr std::string_view kViewerHex       = "hex";
inline constexpr std::string_view kViewerText      = "text";
inline constexpr std::string_view kViewerTexture   = "texture";
inline constexpr std::string_view kViewerApt       = "apt";
inline constexpr std::string_view kViewerDat       = "dat";
inline constexpr std::string_view kViewerRsf       = "rsf";
inline constexpr std::string_view kViewerAudio     = "audio";
inline constexpr std::string_view kViewerInspector = "inspector";

// ── Detection result ──────────────────────────────────────────────────────────

struct FileDetectionResult {
  FileKind    kind              = FileKind::Unknown;
  std::uint8_t confidence       = 0;   // 0-99, see levels above
  std::string reason;                  // human-readable explanation for logging
  bool        isBinary          = false;
  std::string recommendedViewer;       // use kViewer* constants above

  // Convenience helpers
  [[nodiscard]] bool confident()   const noexcept { return confidence >= 70; }
  [[nodiscard]] bool highConfident() const noexcept { return confidence >= 90; }
};

// ── Main detection function ───────────────────────────────────────────────────
//
// `headerBytes` — ideally the first 64+ bytes of the file content; the function
//   handles empty/short spans safely.
// `filename`    — optional file name or extension (with or without leading dot).
//   If provided, the function uses the extension as a secondary signal.
//
// The function never throws.  A result with confidence == 0 means detection
// failed completely; callers should route to the generic inspector.
FileDetectionResult detectFileKind(
    std::span<const std::uint8_t>   headerBytes,
    std::optional<std::string_view> filename = std::nullopt) noexcept;

// ── Utility helpers ───────────────────────────────────────────────────────────

// Returns the canonical viewer string for a FileKind (may be empty for Unknown).
std::string_view recommendedViewerForKind(FileKind kind) noexcept;

// Returns a short human-readable name for the kind (e.g. "RSF", "DDS", "XML").
std::string_view kindDisplayName(FileKind kind) noexcept;

// Heuristic: is the byte span mostly printable ASCII/UTF-8?
// threshold = minimum ratio of printable bytes (0.0–1.0) to classify as text.
bool looksLikeText(std::span<const std::uint8_t> bytes, double threshold = 0.85) noexcept;

} // namespace gf::core
