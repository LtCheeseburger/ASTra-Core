#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// gf::textures::texture_replace
//
// TGA-first, container-aware texture replacement pipeline.
//
// Design contract
// ───────────────
// The canonical *import* format is TGA (raw RGBA pixels).  DDS is accepted as
// an "advanced override" but must exactly match the target container's format
// contract (format, dimensions, mip count, no DX10 extension).
//
// The pipeline:
//   User TGA (or DDS advanced)
//     ↓ load raw RGBA pixels
//     ↓ validate dimensions vs original texture
//     ↓ determine required output format (BC1/BC3/…)
//     ↓ generate full mip chain
//     ↓ compress to BC
//     ↓ apply platform layout (swizzle/tile/endian for XPR2)
//     ↓ rebuild container (DDS / P3R / XPR2)
//     ↓ return bytes ready for AST entry replacement
//
// All failure paths return nullopt + fill *err with a human-readable diagnostic.
// The caller must never silently write a corrupt archive — block on any failure.
// ─────────────────────────────────────────────────────────────────────────────

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace gf::textures {

// ── Container kind ───────────────────────────────────────────────────────────

enum class TexContainerKind {
    Unknown,
    DDS,   // plain DDS (PC / PS3 EA)
    P3R,   // EA P3R wrapper (DDS body with first 4 bytes = "P3Rx")
    XPR2,  // Xbox 360 XPR2 (tiled + swap16)
    XPR,   // Xbox original XPR (treated as opaque passthrough)
};

// Detect the container kind from the first few bytes of an AST payload.
TexContainerKind detect_container_kind(std::span<const std::uint8_t> payload) noexcept;

// ── Import source descriptor ─────────────────────────────────────────────────

enum class TexImportFormat {
    TGA,         // recommended: raw pixels, pipeline handles everything
    DDS_Advanced // caller-supplied DDS that must already match the contract
};

// ── Original-texture info ────────────────────────────────────────────────────
// Extracted from the existing AST entry before replacement, used to drive the
// rebuild (format, dimensions, mip count, P3R version byte, etc.).

struct OriginalTextureInfo {
    TexContainerKind container = TexContainerKind::Unknown;

    std::uint32_t width    = 0;
    std::uint32_t height   = 0;
    std::uint32_t mipCount = 0;

    // BC format code in a common enum subset (mirrors DdsFormat / Xpr2Info).
    // 0x01 = DXT1/BC1, 0x03 = DXT3/BC2, 0x05 = DXT5/BC3,
    // 0x04 = BC4/ATI1, 0x06 = BC5/ATI2.
    // For XPR2 this is mapped from the raw EA fmt_code.
    std::uint8_t bcFormatCode = 0x01; // default BC1

    // P3R-specific: the version byte at offset 3 ("P3R<ver>").
    std::uint8_t p3rVersionByte = 0x02;

    // XPR2 raw fmt_code — kept verbatim for the xpr2_rebuild path.
    std::uint8_t xpr2RawFmtCode = 0x52;

    [[nodiscard]] bool valid() const noexcept {
        return width > 0 && height > 0 && container != TexContainerKind::Unknown;
    }
};

// Parse OriginalTextureInfo from a raw AST entry payload.
// `astFlags` is the per-entry flags word from the AST container (used for EA
// header format disambiguation when needed).
std::optional<OriginalTextureInfo> parse_original_texture_info(
    std::span<const std::uint8_t> payload,
    std::uint32_t                  astFlags = 0,
    std::string*                   err      = nullptr);

// ── Replacement result ────────────────────────────────────────────────────────

struct TextureReplaceResult {
    // The rebuilt container bytes, ready to be written into the AST entry.
    std::vector<std::uint8_t> containerBytes;

    // Human-readable summary for the confirmation dialog.
    std::string summary;

    // True when the import was DDS-advanced and passed validation.
    bool usedDdsAdvanced = false;
};

// ── Main entry point ─────────────────────────────────────────────────────────

// Replace the texture described by `originalPayload` with a new image supplied
// as `importBytes` in `importFormat`.
//
// Steps performed internally:
//   1. Decode source pixels (TGA reader or DDS decoder).
//   2. Parse original container metadata.
//   3. Validate dimensions + format compatibility.
//   4. Generate mip chain.
//   5. Compress to target BC format.
//   6. Apply platform layout (XPR2: retile + swap16).
//   7. Rebuild container (DDS / P3R header swap / XPR2 patch).
//
// Returns nullopt and fills *err on any failure.  Callers MUST treat a nullopt
// result as a hard failure and abort the replacement.
std::optional<TextureReplaceResult> replace_texture(
    std::span<const std::uint8_t> originalPayload,
    std::span<const std::uint8_t> importBytes,
    TexImportFormat                importFormat,
    std::uint32_t                  astFlags = 0,
    std::string*                   err      = nullptr);

// ── Validation helper (for UI "preview" before committing) ───────────────────
// Returns a non-empty error string if the import is definitively incompatible,
// or an empty string if it looks acceptable.
// Does NOT perform full BC compression — cheap enough to call from the dialog.
std::string validate_texture_import(
    std::span<const std::uint8_t> originalPayload,
    std::span<const std::uint8_t> importBytes,
    TexImportFormat                importFormat,
    std::uint32_t                  astFlags = 0);

} // namespace gf::textures
