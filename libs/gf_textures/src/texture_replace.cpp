// texture_replace.cpp
//
// TGA-first, container-aware texture replacement pipeline for ASTra.
//
// Architecture overview:
//
//   User TGA (or DDS advanced)
//     ↓  load_source_rgba()              — decode TGA → RGBA
//     ↓  parse_original_texture_info()   — read container metadata
//     ↓  validate dimensions/format
//     ↓  build_dds_payload()             — mip chain + BC compress (once)
//     ↓  rebuild_container()             — DDS / P3R header swap / XPR2 patch
//     ↓  TextureReplaceResult + TextureReplaceReport
//
// Failure policy: every sub-step returns std::nullopt on error and fills *err.
// replace_texture() propagates the first error and never writes partial data.
// TextureReplaceReport is filled even on failure for diagnostic purposes.

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

// Map bcFormatCode to DdsBuildFormat.
// Returns nullopt for formats with no encoder (DXT3) or unknown codes.
static std::optional<DdsBuildFormat> bc_code_to_build_format(std::uint8_t code) noexcept {
    switch (code) {
    case 0x01: return DdsBuildFormat::DXT1;
    case 0x05: return DdsBuildFormat::DXT5;
    case 0x04: return DdsBuildFormat::BC4;
    case 0x06: return DdsBuildFormat::BC5;
    // 0x03 = DXT3/BC2: explicit alpha (not interpolated).  No encoder in this
    // build.  Fail with a clear message rather than silently writing DXT5 data
    // with the wrong block structure.
    default:   return std::nullopt;
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

// Compute the expected compressed size (bytes) for one mip level.
static std::size_t mip_linear_bytes(std::uint32_t w, std::uint32_t h,
                                    std::uint8_t bcCode) noexcept {
    const std::uint32_t bw = std::max(1u, (w + 3u) / 4u);
    const std::uint32_t bh = std::max(1u, (h + 3u) / 4u);
    const std::size_t bpb  = (bcCode == 0x01 || bcCode == 0x04) ? 8u : 16u;
    return static_cast<std::size_t>(bw) * bh * bpb;
}

// Fill per-mip diagnostics for a known mip chain (no actual data needed).
static void fill_mip_diagnostics(TextureReplaceReport& rpt,
                                  std::uint32_t w, std::uint32_t h,
                                  std::uint32_t mipCount,
                                  std::uint8_t bcCode,
                                  bool platformPacked) {
    rpt.mips.clear();
    rpt.expectedPayloadBytes = 0;
    std::uint32_t mw = w, mh = h;
    for (std::uint32_t m = 0; m < mipCount; ++m) {
        MipLevelDiagnostic d;
        d.level        = m;
        d.width        = mw;
        d.height       = mh;
        d.widthBlocks  = std::max(1u, (mw + 3u) / 4u);
        d.heightBlocks = std::max(1u, (mh + 3u) / 4u);
        d.linearBytes  = mip_linear_bytes(mw, mh, bcCode);
        d.packedBytes  = platformPacked ? 0 : 0; // XPR2 pads; set to 0 here
        rpt.mips.push_back(d);
        rpt.expectedPayloadBytes += d.linearBytes;
        if (mw == 1u && mh == 1u) break;
        mw = std::max(1u, mw >> 1u);
        mh = std::max(1u, mh >> 1u);
    }
}

// Try to rebuild a raw EA-wrapped payload into a standard DDS blob.
static std::vector<std::uint8_t> try_rebuild_ea_dds(
    std::span<const std::uint8_t> bytes, std::uint32_t astFlags)
{
    if (bytes.size() >= 4 && bytes[0]=='D' && bytes[1]=='D' && bytes[2]=='S' && bytes[3]==' ')
        return std::vector<std::uint8_t>(bytes.begin(), bytes.end());
    EaDdsInfo info{};
    if (auto r = rebuild_ea_dds(bytes, astFlags, &info); r && r->size() >= 4)
        return std::move(*r);
    return {};
}

// Decode any source image (TGA) to RGBA pixels.
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

    // DDS advanced: decode mip-0 to RGBA for dimension validation only.
    auto img = decode_dds_mip_rgba(importBytes, 0);
    if (!img || img->rgba.empty()) {
        if (err) *err = "DDS import: could not decode mip-0 to RGBA. "
                        "Ensure the DDS is a standard BC1/BC3 file without DX10 extension.";
        return std::nullopt;
    }
    return RgbaSource{img->width, img->height, std::move(img->rgba)};
}

// Build a DDS from RGBA using the original texture's format/dimensions/mips.
// Returns nullopt and sets *err if the format is unsupported or build fails.
static std::optional<std::vector<std::uint8_t>> build_dds_payload(
    const RgbaSource&          src,
    const OriginalTextureInfo& orig,
    std::string*               err)
{
    auto fmtOpt = bc_code_to_build_format(orig.bcFormatCode);
    if (!fmtOpt) {
        if (err) {
            std::ostringstream os;
            os << "Format " << bc_code_name(orig.bcFormatCode)
               << " (code 0x" << std::hex << static_cast<int>(orig.bcFormatCode) << std::dec
               << ") has no encoder in this build. "
               << "Use DDS-advanced import with a pre-compressed DDS file that exactly "
               << "matches the original format, dimensions, and mip count.";
            *err = os.str();
        }
        return std::nullopt;
    }

    DdsBuildParams p;
    p.format    = *fmtOpt;
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

static bool is_power_of_two(std::uint32_t v) noexcept {
    return v > 0 && (v & (v - 1)) == 0;
}

static std::string check_dimensions(const RgbaSource& src,
                                     const OriginalTextureInfo& orig) {
    if (src.width != orig.width || src.height != orig.height) {
        std::ostringstream os;
        os << "Source dimensions " << src.width << "x" << src.height
           << " do not match original " << orig.width << "x" << orig.height << ".";
        return os.str();
    }
    if ((orig.width % 4 != 0) || (orig.height % 4 != 0)) {
        std::ostringstream os;
        os << "BC-compressed textures require width and height to be multiples of 4. "
           << "Original dimensions are " << orig.width << "x" << orig.height
           << " — replacement would produce an invalid texture.";
        return os.str();
    }
    if (orig.container == TexContainerKind::XPR2) {
        if (!is_power_of_two(orig.width) || !is_power_of_two(orig.height)) {
            std::ostringstream os;
            os << "XPR2 (Xbox 360) textures require power-of-2 dimensions. "
               << "Original dimensions are " << orig.width << "x" << orig.height << ".";
            return os.str();
        }
    }
    return {};
}

static std::string check_dds_contract(
    std::span<const std::uint8_t> ddsBytes,
    const OriginalTextureInfo&    orig)
{
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
    if (info->width % 4 != 0 || info->height % 4 != 0)
        issues << "DDS dimensions (" << info->width << "x" << info->height
               << ") are not multiples of 4 as required by BC block compression. ";
    if (orig.container == TexContainerKind::XPR2) {
        if (!is_power_of_two(info->width) || !is_power_of_two(info->height))
            issues << "XPR2 target requires power-of-2 dimensions; DDS reports "
                   << info->width << "x" << info->height << ". ";
    }
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
        auto xprInfo = parse_xpr2_info(payload);
        if (!xprInfo) {
            if (err) *err = "Could not parse XPR2 metadata.";
            return std::nullopt;
        }
        info.width          = xprInfo->width;
        info.height         = xprInfo->height;
        info.mipCount       = xprInfo->mip_count;
        info.bcFormatCode   = xpr2_fmt_to_bc_code(xprInfo->fmt_code);
        info.xpr2RawFmtCode = xprInfo->fmt_code;
        if (!info.valid()) {
            if (err) *err = "XPR2 reports zero or invalid dimensions.";
            return std::nullopt;
        }
        return info;
    }

    // ── P3R ─────────────────────────────────────────────────────────────────
    if (info.container == TexContainerKind::P3R) {
        info.p3rVersionByte = (payload.size() >= 4) ? payload[3] : 0x02;

        std::vector<std::uint8_t> tmp(payload.begin(), payload.end());
        if (tmp.size() >= 4) { tmp[0]='D'; tmp[1]='D'; tmp[2]='S'; tmp[3]=' '; }
        auto ddsInfo = parse_dds_info(std::span<const std::uint8_t>(tmp.data(), tmp.size()));
        if (!ddsInfo) {
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
                info.container    = TexContainerKind::DDS;
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

    if (importFormat == TexImportFormat::DDS_Advanced)
        return check_dds_contract(importBytes, *origInfo);

    // TGA: check dimensions and that the format has an encoder.
    std::string tgaErr;
    auto img = read_tga(importBytes, &tgaErr);
    if (!img) return "TGA parse error: " + tgaErr;

    const std::string dimIssue =
        check_dimensions(RgbaSource{img->width, img->height, {}}, *origInfo);
    if (!dimIssue.empty()) return dimIssue;

    // Surface the "no encoder" problem at validation time, not during replace.
    if (!bc_code_to_build_format(origInfo->bcFormatCode)) {
        std::ostringstream os;
        os << "Format " << bc_code_name(origInfo->bcFormatCode)
           << " cannot be rebuilt from TGA — no encoder available. "
           << "Use DDS-advanced import instead.";
        return os.str();
    }

    return {};
}

// ── Public: replace_texture ───────────────────────────────────────────────────

std::optional<TextureReplaceResult> replace_texture(
    std::span<const std::uint8_t> originalPayload,
    std::span<const std::uint8_t> importBytes,
    TexImportFormat                importFormat,
    std::uint32_t                  astFlags,
    std::string*                   err,
    TextureReplaceReport*          report)
{
    // Helper: fill report + err and return nullopt.
    auto fail = [&](const std::string& msg,
                    const std::string& detail = {}) -> std::optional<TextureReplaceResult> {
        if (err) *err = msg;
        if (report) {
            report->success      = false;
            report->shortReason  = msg;
            report->detailReason = detail.empty() ? msg : detail;
        }
        return std::nullopt;
    };

    // ── Step 1: Parse original container metadata ─────────────────────────
    std::string origErr;
    auto origInfo = parse_original_texture_info(originalPayload, astFlags, &origErr);
    if (!origInfo)
        return fail("Cannot read original texture metadata: " + origErr);

    // Populate report fields from origInfo immediately.
    if (report) {
        report->containerKind    = origInfo->container;
        report->origWidth        = origInfo->width;
        report->origHeight       = origInfo->height;
        report->origMipCount     = origInfo->mipCount;
        report->origBcFormatCode = origInfo->bcFormatCode;
        report->origFormatName   = bc_code_name(origInfo->bcFormatCode);
    }

    // ── Step 2: DDS-advanced path ─────────────────────────────────────────
    if (importFormat == TexImportFormat::DDS_Advanced) {
        const std::string contractIssues = check_dds_contract(importBytes, *origInfo);
        if (!contractIssues.empty())
            return fail("DDS import contract violation: " + contractIssues);

        std::vector<std::uint8_t> ddsBlob(importBytes.begin(), importBytes.end());

        if (report) {
            report->rebuiltBcFormatCode    = origInfo->bcFormatCode;
            report->rebuiltFormatName      = bc_code_name(origInfo->bcFormatCode);
            report->rebuiltMipCount        = origInfo->mipCount;
            fill_mip_diagnostics(*report, origInfo->width, origInfo->height,
                                 origInfo->mipCount, origInfo->bcFormatCode,
                                 origInfo->container == TexContainerKind::XPR2);
        }

        TextureReplaceResult result;
        result.usedDdsAdvanced = true;

        switch (origInfo->container) {
        case TexContainerKind::P3R: {
            if (ddsBlob.size() >= 4) {
                ddsBlob[0] = 0x70; // 'p' lowercase — required by PS3 EA loader
                ddsBlob[1] = '3';
                ddsBlob[2] = 'R';
                ddsBlob[3] = origInfo->p3rVersionByte;
            }
            result.containerBytes = std::move(ddsBlob);
            result.summary = "DDS → P3R (advanced import, format preserved)";
            break;
        }
        case TexContainerKind::DDS: {
            result.containerBytes = std::move(ddsBlob);
            result.summary = "DDS → DDS (advanced import, format preserved)";
            break;
        }
        case TexContainerKind::XPR2: {
            std::string xprErr;
            auto patched = dds_to_xpr2_patch(originalPayload,
                std::span<const std::uint8_t>(ddsBlob.data(), ddsBlob.size()), &xprErr);
            if (!patched || patched->empty())
                return fail("XPR2 container rebuild failed: " + xprErr);
            result.containerBytes = std::move(*patched);
            result.summary = "DDS → XPR2 (advanced import)";
            if (report) report->platformPackingApplied = true;
            break;
        }
        default:
            return fail("DDS-advanced import is not supported for this container type.");
        }

        if (report) {
            report->builtPayloadBytes = result.containerBytes.size();
            report->success = true;
            report->shortReason = result.summary;
        }
        return result;
    }

    // ── Step 3: TGA path — decode source pixels ───────────────────────────
    std::string srcErr;
    auto src = load_source_rgba(importBytes, importFormat, &srcErr);
    if (!src) return fail(srcErr);

    if (report) {
        report->importedWidth  = src->width;
        report->importedHeight = src->height;
    }

    // ── Step 4: Validate dimensions ───────────────────────────────────────
    const std::string dimIssue = check_dimensions(*src, *origInfo);
    if (!dimIssue.empty()) return fail(dimIssue);

    // ── Step 4b: Check that the original format has an encoder ────────────
    if (!bc_code_to_build_format(origInfo->bcFormatCode)) {
        std::ostringstream short_msg;
        short_msg << "Format " << bc_code_name(origInfo->bcFormatCode)
                  << " cannot be rebuilt from TGA.";
        std::ostringstream detail;
        detail << short_msg.str() << "\n\n"
               << "No TGA-based encoder exists for "
               << bc_code_name(origInfo->bcFormatCode) << " in this build. "
               << "Use DDS-advanced import with a pre-compressed DDS file that exactly "
               << "matches the original: format=" << bc_code_name(origInfo->bcFormatCode)
               << ", dimensions=" << origInfo->width << "x" << origInfo->height
               << ", mips=" << origInfo->mipCount << ".";
        return fail(short_msg.str(), detail.str());
    }

    // ── Step 5 & 6: Build DDS (mip chain + BC compression, once) ─────────
    std::string buildErr;
    auto dds = build_dds_payload(*src, *origInfo, &buildErr);
    if (!dds) return fail(buildErr);

    // Determine the actual mip count that was built.
    const std::uint32_t builtMips = [&]() -> std::uint32_t {
        if (dds->size() < 128) return 1;
        const auto* hdr = dds->data();
        const std::uint32_t m = static_cast<std::uint32_t>(hdr[28]) |
                                (static_cast<std::uint32_t>(hdr[29]) << 8) |
                                (static_cast<std::uint32_t>(hdr[30]) << 16) |
                                (static_cast<std::uint32_t>(hdr[31]) << 24);
        return std::max(1u, m);
    }();

    if (report) {
        report->rebuiltBcFormatCode = origInfo->bcFormatCode;
        report->rebuiltFormatName   = bc_code_name(origInfo->bcFormatCode);
        report->rebuiltMipCount     = builtMips;
        fill_mip_diagnostics(*report, origInfo->width, origInfo->height,
                             builtMips, origInfo->bcFormatCode,
                             origInfo->container == TexContainerKind::XPR2);
        report->builtPayloadBytes = dds->size() >= 128
                                        ? dds->size() - 128 : 0;
        report->payloadSizeMatch  = (report->builtPayloadBytes == report->expectedPayloadBytes);
    }

    // ── Step 7: Rebuild container ─────────────────────────────────────────
    TextureReplaceResult result;

    auto make_summary = [&](const char* prefix) -> std::string {
        std::ostringstream os;
        os << prefix << " (" << bc_code_name(origInfo->bcFormatCode)
           << " " << origInfo->width << "x" << origInfo->height
           << " mips=" << builtMips << ")";
        return os.str();
    };

    switch (origInfo->container) {
    case TexContainerKind::DDS: {
        result.containerBytes = std::move(*dds);
        result.summary = make_summary("TGA → DDS");
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
        result.summary = make_summary("TGA → P3R");
        break;
    }
    case TexContainerKind::XPR2: {
        // Feed the freshly-built DDS into dds_to_xpr2_patch:
        //   linear DXT → XGAddress retile → swap16 → XPR2 payload
        std::string xprErr;
        auto patched = dds_to_xpr2_patch(
            originalPayload,
            std::span<const std::uint8_t>(dds->data(), dds->size()),
            &xprErr);
        if (!patched || patched->empty())
            return fail("XPR2 container rebuild failed: " + xprErr, xprErr);
        result.containerBytes = std::move(*patched);
        result.summary = make_summary("TGA → XPR2");
        if (report) report->platformPackingApplied = true;
        break;
    }
    default:
        return fail("Container kind is not supported for TGA import: " +
                    std::to_string(static_cast<int>(origInfo->container)));
    }

    // ── Step 8: Final sanity — must not produce an empty payload ──────────
    if (result.containerBytes.empty())
        return fail("Container rebuild produced empty output — replacement aborted.");

    if (report) {
        report->builtPayloadBytes = result.containerBytes.size();
        report->success           = true;
        report->shortReason       = result.summary;
    }

    return result;
}

} // namespace gf::textures
