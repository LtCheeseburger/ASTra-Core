// texture_replace.cpp
//
// TGA-first, container-aware texture replacement pipeline for ASTra.
//
// Architecture overview:
//
//   User TGA (or DDS advanced)
//     ↓  load_source_rgba()        — decode TGA → RGBA, or decode DDS mip-0 → RGBA
//     ↓  parse_original_texture_info()  — read container metadata
//     ↓  validate_source_vs_original()  — dimensions, format compat
//     ↓  build_mip_chain()         — dds_build path (box downsample + BC compress)
//     ↓  rebuild_container()       — DDS / P3R header swap / XPR2 patch
//     ↓  TextureReplaceResult
//
// Failure policy: every sub-step returns std::nullopt on error and fills *err.
// replace_texture() propagates the first error and never writes partial data.

#include <gf/textures/texture_replace.hpp>

#include <gf/textures/dds_build.hpp>
#include <gf/textures/dds_decode.hpp>
#include <gf/textures/dds_validate.hpp>
#include <gf/textures/ea_dds_rebuild.hpp>
#include <gf/textures/tga_reader.hpp>
#include <gf/textures/xpr2_rebuild.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

namespace gf::textures {

// ── Internal helpers ─────────────────────────────────────────────────────────

namespace {

// Map DdsFormat to our internal bcFormatCode byte.
static std::uint8_t dds_format_to_bc_code(DdsFormat f) noexcept {
    switch (f) {
    case DdsFormat::DXT1:  return 0x01;
    case DdsFormat::DXT3:  return 0x03;
    case DdsFormat::DXT5:  return 0x05;
    case DdsFormat::ATI1:  return 0x04;
    case DdsFormat::ATI2:  return 0x06;
    default:               return 0x01;
    }
}

// Map XPR2 raw EA fmt_code to our bcFormatCode byte.
static std::uint8_t xpr2_fmt_to_bc_code(std::uint8_t xpr2Fmt) noexcept {
    switch (xpr2Fmt) {
    case 0x52: return 0x01; // DXT1
    case 0x7C: return 0x01; // DXT1 (normal map variant)
    case 0x53: return 0x03; // DXT3
    case 0x54: return 0x05; // DXT5
    case 0x71: return 0x06; // ATI2 / BC5
    default:   return 0x01;
    }
}

// Map bcFormatCode to DdsBuildFormat (supports DXT1 + DXT5; all others
// are mapped conservatively to DXT5 because DXT5 is a superset).
static DdsBuildFormat bc_code_to_build_format(std::uint8_t code) noexcept {
    switch (code) {
    case 0x01: return DdsBuildFormat::DXT1;
    default:   return DdsBuildFormat::DXT5; // DXT3/BC4/BC5 — use DXT5 as safe superset
    }
}

static const char* bc_code_name(std::uint8_t code) noexcept {
    switch (code) {
    case 0x01: return "BC1/DXT1";
    case 0x03: return "BC2/DXT3";
    case 0x05: return "BC3/DXT5";
    case 0x04: return "BC4/ATI1";
    case 0x06: return "BC5/ATI2";
    default:   return "Unknown";
    }
}

// Try to rebuild a raw EA-wrapped payload into a standard DDS blob.
static std::vector<std::uint8_t> try_rebuild_ea_dds(
    std::span<const std::uint8_t> bytes, std::uint32_t astFlags)
{
    // Already a DDS?
    if (bytes.size() >= 4 && bytes[0]=='D' && bytes[1]=='D' && bytes[2]=='S' && bytes[3]==' ')
        return std::vector<std::uint8_t>(bytes.begin(), bytes.end());
    EaDdsInfo info{};
    if (auto r = rebuild_ea_dds(bytes, astFlags, &info); r && r->size() >= 4)
        return std::move(*r);
    return {};
}

// Decode any source image (TGA or DDS mip-0) to RGBA pixels.
// Returns empty on failure and fills *err.
struct RgbaSource {
    std::uint32_t width  = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> rgba;
};

static std::optional<RgbaSource> load_source_rgba(
    std::span<const std::uint8_t> importBytes,
    TexImportFormat                importFormat,
    std::string*                   err)
{
    if (importFormat == TexImportFormat::TGA) {
        std::string tgaErr;
        auto img = read_tga(importBytes, &tgaErr);
        if (!img) {
            if (err) *err = "TGA parse error: " + tgaErr;
            return std::nullopt;
        }
        if (img->width == 0 || img->height == 0 || img->rgba.empty()) {
            if (err) *err = "TGA reports zero dimensions.";
            return std::nullopt;
        }
        return RgbaSource{img->width, img->height, std::move(img->rgba)};
    }

    // DDS advanced: decode mip-0 to RGBA for pixel comparison / validation.
    // We still use the raw DDS bytes for the rebuild — we just need RGBA to
    // verify dimensions are consistent.
    auto img = decode_dds_mip_rgba(importBytes, 0);
    if (!img || img->rgba.empty()) {
        if (err) *err = "DDS import: could not decode mip-0 to RGBA. "
                        "Ensure the DDS is a standard BC1/BC3 file without DX10 extension.";
        return std::nullopt;
    }
    return RgbaSource{img->width, img->height, std::move(img->rgba)};
}

// Rebuild a minimal DDS blob from RGBA for use with the P3R / plain-DDS paths.
// Uses the OriginalTextureInfo to select the right BC format and mip count.
static std::optional<std::vector<std::uint8_t>> build_dds_payload(
    const RgbaSource&          src,
    const OriginalTextureInfo& orig,
    std::string*               err)
{
    DdsBuildParams p;
    p.format    = bc_code_to_build_format(orig.bcFormatCode);
    p.target_w  = orig.width;
    p.target_h  = orig.height;
    p.mip_count = orig.mipCount; // 0 = full chain

    std::string buildErr;
    auto dds = build_dds_from_rgba(
        std::span<const std::uint8_t>(src.rgba.data(), src.rgba.size()),
        src.width, src.height, p, &buildErr);
    if (dds.empty()) {
        if (err) *err = "DDS build error: " + buildErr;
        return std::nullopt;
    }
    return dds;
}

// Validate that source dimensions are acceptable for the target.
// Strict mode: must be identical.
static std::string check_dimensions(const RgbaSource& src, const OriginalTextureInfo& orig) {
    if (src.width != orig.width || src.height != orig.height) {
        std::ostringstream os;
        os << "Source dimensions " << src.width << "x" << src.height
           << " do not match original " << orig.width << "x" << orig.height << ".";
        return os.str();
    }
    return {};
}

// Check that a DDS-advanced import satisfies the format contract for the original.
static std::string check_dds_contract(
    std::span<const std::uint8_t> ddsBytes,
    const OriginalTextureInfo&    orig)
{
    // Must start with DDS magic.
    if (ddsBytes.size() < 4 || std::memcmp(ddsBytes.data(), "DDS ", 4) != 0)
        return "Replacement file is not a DDS file.";

    const auto validation = inspect_dds(ddsBytes);
    if (validation.status == DdsValidationStatus::Invalid)
        return "DDS file fails validation: " + validation.summary;
    if (validation.dx10HeaderPresent)
        return "DX10 extended DDS headers are not accepted for texture replacement. "
               "Re-export the texture without the DX10 extension.";

    auto info = parse_dds_info(ddsBytes);
    if (!info) return "Cannot parse DDS header.";
    if (info->width == 0 || info->height == 0) return "DDS reports zero dimensions.";

    std::ostringstream issues;
    if (info->width  != orig.width)
        issues << "Width mismatch: DDS=" << info->width << " original=" << orig.width << ". ";
    if (info->height != orig.height)
        issues << "Height mismatch: DDS=" << info->height << " original=" << orig.height << ". ";
    if (orig.mipCount > 0 && info->mipCount != orig.mipCount)
        issues << "Mip count mismatch: DDS=" << info->mipCount
               << " original=" << orig.mipCount << ". ";
    const std::uint8_t importCode = dds_format_to_bc_code(info->format);
    if (importCode != orig.bcFormatCode)
        issues << "Format mismatch: DDS=" << bc_code_name(importCode)
               << " original=" << bc_code_name(orig.bcFormatCode) << ". ";

    return issues.str();
}

} // anon namespace

// ── Public: detect_container_kind ────────────────────────────────────────────

TexContainerKind detect_container_kind(std::span<const std::uint8_t> p) noexcept {
    if (p.size() < 4) return TexContainerKind::Unknown;
    if (std::memcmp(p.data(), "XPR2", 4) == 0) return TexContainerKind::XPR2;
    if (std::memcmp(p.data(), "XPR0", 4) == 0 ||
        std::memcmp(p.data(), "XPR1", 4) == 0) return TexContainerKind::XPR;
    if ((p[0]=='P'||p[0]=='p') && p[1]=='3' && (p[2]=='R'||p[2]=='r'))
        return TexContainerKind::P3R;
    if (std::memcmp(p.data(), "DDS ", 4) == 0) return TexContainerKind::DDS;
    // EA-wrapped DDS (no standard magic at offset 0): treat as DDS family.
    if (p.size() >= 8) {
        // A non-zero width/height in the EA header region is a reasonable heuristic,
        // but the safest fallback is Unknown — let ea_dds_rebuild handle it.
    }
    return TexContainerKind::Unknown;
}

// ── Public: parse_original_texture_info ──────────────────────────────────────

std::optional<OriginalTextureInfo> parse_original_texture_info(
    std::span<const std::uint8_t> payload,
    std::uint32_t                  astFlags,
    std::string*                   err)
{
    if (payload.empty()) {
        if (err) *err = "Original texture payload is empty.";
        return std::nullopt;
    }

    OriginalTextureInfo info;
    info.container = detect_container_kind(payload);

    // ── XPR2 ────────────────────────────────────────────────────────────────
    if (info.container == TexContainerKind::XPR2) {
        std::string xprErr;
        auto xprInfo = parse_xpr2_info(payload);
        if (!xprInfo) {
            if (err) *err = "Could not parse XPR2 metadata.";
            return std::nullopt;
        }
        info.width         = xprInfo->width;
        info.height        = xprInfo->height;
        info.mipCount      = xprInfo->mip_count;
        info.bcFormatCode  = xpr2_fmt_to_bc_code(xprInfo->fmt_code);
        info.xpr2RawFmtCode = xprInfo->fmt_code;
        if (!info.valid()) {
            if (err) *err = "XPR2 reports zero or invalid dimensions.";
            return std::nullopt;
        }
        return info;
    }

    // ── P3R ─────────────────────────────────────────────────────────────────
    if (info.container == TexContainerKind::P3R) {
        // Preserve the version byte for round-trip.
        info.p3rVersionByte = (payload.size() >= 4) ? payload[3] : 0x02;

        // Reconstruct DDS header to extract metadata.
        // Both 'p' (0x70) and 'P' (0x50) variants are found in the wild;
        // we just need "DDS " to satisfy parse_dds_info.
        std::vector<std::uint8_t> tmp(payload.begin(), payload.end());
        if (tmp.size() >= 4) { tmp[0]='D'; tmp[1]='D'; tmp[2]='S'; tmp[3]=' '; }
        auto ddsInfo = parse_dds_info(std::span<const std::uint8_t>(tmp.data(), tmp.size()));
        if (!ddsInfo) {
            // Try the ea_dds_rebuild path (some P3R have EA headers, not raw DDS bodies).
            auto rebuilt = try_rebuild_ea_dds(payload, astFlags);
            if (!rebuilt.empty())
                ddsInfo = parse_dds_info(std::span<const std::uint8_t>(rebuilt.data(), rebuilt.size()));
        }
        if (!ddsInfo) {
            if (err) *err = "Could not parse P3R texture metadata (DDS header unreadable).";
            return std::nullopt;
        }
        info.width        = ddsInfo->width;
        info.height       = ddsInfo->height;
        info.mipCount     = ddsInfo->mipCount;
        info.bcFormatCode = dds_format_to_bc_code(ddsInfo->format);
        if (!info.valid()) {
            if (err) *err = "P3R reports zero or invalid dimensions.";
            return std::nullopt;
        }
        return info;
    }

    // ── Plain DDS ────────────────────────────────────────────────────────────
    if (info.container == TexContainerKind::DDS) {
        auto ddsInfo = parse_dds_info(payload);
        if (!ddsInfo) {
            if (err) *err = "Could not parse plain DDS texture metadata.";
            return std::nullopt;
        }
        info.width        = ddsInfo->width;
        info.height       = ddsInfo->height;
        info.mipCount     = ddsInfo->mipCount;
        info.bcFormatCode = dds_format_to_bc_code(ddsInfo->format);
        if (!info.valid()) {
            if (err) *err = "DDS reports zero or invalid dimensions.";
            return std::nullopt;
        }
        return info;
    }

    // ── EA-wrapped DDS (no standard magic) ───────────────────────────────────
    {
        auto rebuilt = try_rebuild_ea_dds(payload, astFlags);
        if (!rebuilt.empty()) {
            auto ddsInfo = parse_dds_info(std::span<const std::uint8_t>(rebuilt.data(), rebuilt.size()));
            if (ddsInfo && ddsInfo->width > 0 && ddsInfo->height > 0) {
                info.container    = TexContainerKind::DDS; // treat as DDS after rebuild
                info.width        = ddsInfo->width;
                info.height       = ddsInfo->height;
                info.mipCount     = ddsInfo->mipCount;
                info.bcFormatCode = dds_format_to_bc_code(ddsInfo->format);
                return info;
            }
        }
    }

    if (err) *err = "Unrecognised or unsupported texture container (not DDS/P3R/XPR2).";
    return std::nullopt;
}

// ── Public: validate_texture_import ──────────────────────────────────────────

std::string validate_texture_import(
    std::span<const std::uint8_t> originalPayload,
    std::span<const std::uint8_t> importBytes,
    TexImportFormat                importFormat,
    std::uint32_t                  astFlags)
{
    std::string parseErr;
    auto origInfo = parse_original_texture_info(originalPayload, astFlags, &parseErr);
    if (!origInfo)
        return "Cannot read original texture metadata: " + parseErr;

    if (importFormat == TexImportFormat::DDS_Advanced) {
        return check_dds_contract(importBytes, *origInfo);
    }

    // TGA: quick dimension check only (no compression work).
    std::string tgaErr;
    auto img = read_tga(importBytes, &tgaErr);
    if (!img) return "TGA parse error: " + tgaErr;

    return check_dimensions(RgbaSource{img->width, img->height, {}}, *origInfo);
}

// ── Public: replace_texture ───────────────────────────────────────────────────

std::optional<TextureReplaceResult> replace_texture(
    std::span<const std::uint8_t> originalPayload,
    std::span<const std::uint8_t> importBytes,
    TexImportFormat                importFormat,
    std::uint32_t                  astFlags,
    std::string*                   err)
{
    auto fail = [&](const std::string& msg) -> std::optional<TextureReplaceResult> {
        if (err) *err = msg;
        return std::nullopt;
    };

    // ── Step 1: Parse original container metadata ─────────────────────────
    std::string origErr;
    auto origInfo = parse_original_texture_info(originalPayload, astFlags, &origErr);
    if (!origInfo)
        return fail("Cannot read original texture metadata: " + origErr);

    // ── Step 2: DDS-advanced path ─────────────────────────────────────────
    // The caller has already supplied a fully-formed DDS. We validate its
    // contract and then feed it directly into the container rebuild step.
    if (importFormat == TexImportFormat::DDS_Advanced) {
        const std::string contractIssues = check_dds_contract(importBytes, *origInfo);
        if (!contractIssues.empty())
            return fail("DDS import contract violation: " + contractIssues);

        // The raw DDS blob is the source for the rebuild.
        std::vector<std::uint8_t> ddsBlob(importBytes.begin(), importBytes.end());

        TextureReplaceResult result;
        result.usedDdsAdvanced = true;

        switch (origInfo->container) {
        case TexContainerKind::P3R: {
            // Swap magic back to P3R<ver> using lowercase 'p' (0x70).
            // EASE's DDStoP3R hardcodes data[0]=112 (0x70). The PS3 game loader
            // only accepts the lowercase variant — uppercase 'P' causes rejection.
            if (ddsBlob.size() >= 4) {
                ddsBlob[0] = 0x70; // 'p' lowercase
                ddsBlob[1] = '3';
                ddsBlob[2] = 'R';
                ddsBlob[3] = origInfo->p3rVersionByte;
            }
            result.containerBytes = std::move(ddsBlob);
            result.summary = "DDS → P3R (advanced import, format preserved)";
            return result;
        }
        case TexContainerKind::DDS: {
            result.containerBytes = std::move(ddsBlob);
            result.summary = "DDS → DDS (advanced import, format preserved)";
            return result;
        }
        case TexContainerKind::XPR2: {
            std::string xprErr;
            auto patched = dds_to_xpr2_patch(originalPayload,
                std::span<const std::uint8_t>(ddsBlob.data(), ddsBlob.size()), &xprErr);
            if (!patched || patched->empty())
                return fail("XPR2 container rebuild failed: " + xprErr);
            result.containerBytes = std::move(*patched);
            result.summary = "DDS → XPR2 (advanced import)";
            return result;
        }
        default:
            return fail("DDS-advanced import is not supported for this container type.");
        }
    }

    // ── Step 3: TGA path — decode source pixels ───────────────────────────
    std::string srcErr;
    auto src = load_source_rgba(importBytes, importFormat, &srcErr);
    if (!src) return fail(srcErr);

    // ── Step 4: Validate dimensions ───────────────────────────────────────
    const std::string dimIssue = check_dimensions(*src, *origInfo);
    if (!dimIssue.empty()) return fail(dimIssue);

    // ── Step 5 & 6: Build DDS (mip chain + BC compression) ───────────────
    std::string buildErr;
    auto dds = build_dds_payload(*src, *origInfo, &buildErr);
    if (!dds) return fail(buildErr);

    // ── Step 7: Rebuild container ─────────────────────────────────────────
    TextureReplaceResult result;

    switch (origInfo->container) {
    case TexContainerKind::DDS: {
        result.containerBytes = std::move(*dds);
        result.summary = "TGA → DDS (" + std::string(bc_code_name(origInfo->bcFormatCode)) +
                         " " + std::to_string(origInfo->width) + "x" + std::to_string(origInfo->height) +
                         " mips=" + std::to_string(origInfo->mipCount) + ")";
        break;
    }
    case TexContainerKind::P3R: {
        // DDS body with first 4 bytes replaced by P3R magic.
        // MUST use lowercase 'p' (0x70) — this is what EASE's DDStoP3R writes
        // and what the PS3 EA game loader expects. Uppercase 'P' causes the
        // texture manager to reject the header and write 0x7FFFFFFF into the
        // RSX command stream, crashing the emulator.
        auto& b = *dds;
        if (b.size() >= 4) {
            b[0] = 0x70; // 'p' lowercase
            b[1] = '3';
            b[2] = 'R';
            b[3] = origInfo->p3rVersionByte;
        }
        result.containerBytes = std::move(b);
        result.summary = "TGA → P3R (" + std::string(bc_code_name(origInfo->bcFormatCode)) +
                         " " + std::to_string(origInfo->width) + "x" + std::to_string(origInfo->height) +
                         " mips=" + std::to_string(origInfo->mipCount) +
                         " ver=0x" + [&](){
                             char buf[4]; std::snprintf(buf, sizeof(buf), "%02X", origInfo->p3rVersionByte);
                             return std::string(buf); }() + ")";
        break;
    }
    case TexContainerKind::XPR2: {
        // Feed the freshly-built DDS into dds_to_xpr2_patch, which handles
        // retiling and swap16 to produce the correct Xbox 360 container.
        std::string xprErr;
        auto patched = dds_to_xpr2_patch(
            originalPayload,
            std::span<const std::uint8_t>(dds->data(), dds->size()),
            &xprErr);
        if (!patched || patched->empty())
            return fail("XPR2 container rebuild failed: " + xprErr);
        result.containerBytes = std::move(*patched);
        result.summary = "TGA → XPR2 (" + std::string(bc_code_name(origInfo->bcFormatCode)) +
                         " " + std::to_string(origInfo->width) + "x" + std::to_string(origInfo->height) +
                         " mips=" + std::to_string(origInfo->mipCount) + ")";
        break;
    }
    default:
        return fail("Container kind is not supported for TGA import: " +
                    std::to_string(static_cast<int>(origInfo->container)));
    }

    // ── Step 8: Final sanity check — must not produce an empty payload ─────
    if (result.containerBytes.empty())
        return fail("Container rebuild produced empty output — replacement aborted.");

    return result;
}

} // namespace gf::textures
