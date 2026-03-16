// CreateRsfAstDialog.cpp
//
// Implements the "Create RSF-Based AST" feature for ASTra Core.
//
// Architecture
// ============
//   RsfAstValidator          – folder validation (EASE-equivalent checks)
//   RdfOptionTable           – EASE-equivalent texture descriptor lookup
//   TgaConversionService     – TGA → DDS (PS3) and TGA → XPR2 (Xbox)
//   RsfAstPackager           – build two BGFA containers from converted assets
//   CreateRsfAstDialog       – Qt UI that orchestrates the above via QtConcurrent
//
// Compatibility with EASE
// =======================
//   - FileNameLength = 64 (EASE BGFA default)
//   - TAG IDs match EASE hard-coded values:
//       RSF  → 0x78B0EF25  (2025518437)
//       NAME → 0x8FFFB4D9  (2414196441 — .NAME data files)
//       P3R  → 0x6C1D7D14  (1814246228 — PS3 texture)
//       XPR  → 0x2E57E892  (777539666  — Xbox texture)
//   - FNV1a hash of FileDesc (base name no ext) written as TAGBytes
//   - Flags = 0x03 (ZLIB | FNV1A_HASH) on every entry (matches EASE BGFA export)
//   - Output names: X-<name>_BIG  P-<name>_BIG
//   - DDS source textures are REFUSED — only TGA is accepted for this flow
//   - Xbox path generates XPR2 (tiled) texture containers
//   - PS3 path generates P3R-wrapped DDS containers

#include "CreateRsfAstDialog.hpp"

// Qt
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollBar>
#include <QTimer>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrent>

// ASTra Core
#include <gf/core/AstContainerEditor.hpp>
#include <gf/core/log.hpp>
#include <gf/textures/dds_build.hpp>
#include <gf/textures/dds_decode.hpp>
#include <gf/textures/dds_validate.hpp>
#include <gf/textures/xpr2_rebuild.hpp>

// stdlib
#include <algorithm>
#include <array>
#include <cassert>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

// spdlog (via gf::core::Log)
#include <spdlog/spdlog.h>
#include <zlib.h>

namespace gf::gui {

namespace fs = std::filesystem;

// ============================================================================
//  Utilities
// ============================================================================

namespace {

static std::string upper_ascii(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

static std::string lower_ascii(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

static std::string ext_upper(const fs::path& p) {
    return upper_ascii(p.extension().string());
}

static std::vector<std::uint8_t> read_file(const fs::path& p, std::string* err) {
    std::ifstream f(p, std::ios::binary);
    if (!f) { if (err) *err = "Cannot open: " + p.string(); return {}; }
    f.seekg(0, std::ios::end);
    const auto sz = static_cast<std::size_t>(f.tellg());
    f.seekg(0, std::ios::beg);
    std::vector<std::uint8_t> out(sz);
    if (sz && !f.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(sz))) {
        if (err) *err = "Read failed: " + p.string();
        return {};
    }
    return out;
}

static bool write_file(const fs::path& p, const std::vector<std::uint8_t>& data, std::string* err) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (!f) { if (err) *err = "Cannot create: " + p.string(); return false; }
    if (!data.empty() &&
        !f.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()))) {
        if (err) *err = "Write failed: " + p.string();
        return false;
    }
    return true;
}

// Should we skip this file during folder scanning?
static bool is_junk_file(const fs::path& p) {
    const auto n = lower_ascii(p.filename().string());
    if (n.empty() || n[0] == '.') return true;
    if (n == "thumbs.db" || n == "desktop.ini" || n == ".ds_store") return true;
    return false;
}

// Check if a string ends with suffix (case-insensitive)
static bool ends_with_ci(const std::string& s, const std::string& suffix) {
    if (suffix.size() > s.size()) return false;
    return upper_ascii(s.substr(s.size() - suffix.size())) == upper_ascii(suffix);
}

} // namespace


// ============================================================================
//  RdfOptionTable — mirrors EASE's BGFA.InitRDFOptions() + GetRDF()
// ============================================================================

namespace {

struct RdfSpec {
    std::string d3dfmt;   // "D3DFMT_DXT1" or "D3DFMT_DXT5"
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t mips;
};

// Build the static lookup table exactly matching EASE InitRDFOptions()
static const std::map<std::string, RdfSpec>& get_rdf_table() {
    static const std::map<std::string, RdfSpec> table = {
        { "BASELAYERBAKED_COL",  { "D3DFMT_DXT1",  256,  512, 10 } },
        { "BASELAYERBAKED_NORM", { "D3DFMT_DXT1",   32,   64,  7 } },
        { "BASELAYERBAKED_SPEC", { "D3DFMT_DXT1",   32,   64,  7 } },
        { "ENDZONE",             { "D3DFMT_DXT1",  512,  128, 10 } },
        { "FIELD_VECTOR",        { "D3DFMT_DXT1",    4,    4,  1 } },
        { "FIELD",               { "D3DFMT_DXT1", 1024, 2048, 12 } },
        { "GLOVES_COL",          { "D3DFMT_DXT1",  256,  256,  9 } },
        { "GLOVES_NORM",         { "D3DFMT_DXT1",  256,  256,  9 } },
        { "GLOVES_SPEC",         { "D3DFMT_DXT1",  128,  128,  8 } },
        { "HELMET_BUMP",         { "D3DFMT_DXT1",  256,  256,  9 } },
        { "HELMET_COL",          { "D3DFMT_DXT5", 2048, 1024, 12 } },
        { "JERSEY_COL",          { "D3DFMT_DXT5", 1024, 2048, 12 } },
        { "JERSEY_NORM",         { "D3DFMT_DXT1",  512, 1024, 11 } },
        { "NAMEPLATE",           { "D3DFMT_DXT5",  128, 4096,  1 } },
        { "NAME_DECAL",          { "D3DFMT_DXT5",  128, 4096,  1 } },
        { "NUMBERS",             { "D3DFMT_DXT5",  256, 4096, 13 } },
        { "NUMS_DECAL",          { "D3DFMT_DXT5",  256, 4096, 13 } },
        { "PANTS_COL",           { "D3DFMT_DXT1", 1024,  512, 11 } },
        { "PANTS_NORM",          { "D3DFMT_DXT5", 1024,  512, 11 } },
        { "PRIDE",               { "D3DFMT_DXT5", 2048, 1024, 12 } },
        { "RIDDELL_COL",         { "D3DFMT_DXT1",  512,  512, 10 } },
        { "SOCKS_COL",           { "D3DFMT_DXT1",  256,  256,  9 } },
        { "SHOES_COL",           { "D3DFMT_DXT1",  512,  512, 10 } },
        { "SHOES_NORM",          { "D3DFMT_DXT5",  256,  256,  9 } },
        { "SHOES_SPEC",          { "D3DFMT_DXT1",  128,  128,  8 } },
        { "ACCESS_COL",          { "D3DFMT_DXT1", 1024, 1024, 11 } },
        { "ACCESS_NORM",         { "D3DFMT_DXT1",  256,  256,  9 } },
        { "ACCESS_SPEC",         { "D3DFMT_DXT1",  128,  128,  8 } },
    };
    return table;
}

// Mirror of EASE BGFA.GetRDF() — matches the longest applicable key against
// the TGA filename (case-insensitive).
static std::optional<RdfSpec> lookup_rdf(const std::string& tga_upper) {
    const auto& table = get_rdf_table();

    // EASE uses a strict priority chain of if/else if.  We replicate that
    // priority exactly by trying patterns in the same order EASE does.
    // The order below is taken directly from EASE's GetRDF() method.
    struct Rule { const char* a; const char* b; const char* key; };
    static const Rule rules[] = {
        { "ACCESS",          "COL",    "ACCESS_COL"  },
        { "ACCESS",          "SPEC",   "ACCESS_SPEC" },
        { "ACCESS",          "NORM",   "ACCESS_NORM" },
        { "BASELAYERBAKED",  "COL",    "BASELAYERBAKED_COL"  },
        { "BASELAYERBAKED",  "NORM",   "BASELAYERBAKED_NORM" },
        { "BASELAYERBAKED",  "SPEC",   "BASELAYERBAKED_SPEC" },
        { "ENDZONE",         nullptr,  "ENDZONE"     },
        { "FIELD",           "VECTOR", "FIELD_VECTOR"},
        { "FIELD",           nullptr,  "FIELD"       },
        { "GLOVES",          "COL",    "GLOVES_COL"  },
        { "GLOVES",          "SPEC",   "GLOVES_SPEC" },
        { "GLOVES",          "NORM",   "GLOVES_NORM" },
        { "RIDDELL",         "COL",    "RIDDELL_COL" },
        { "HELMET",          "COL",    "HELMET_COL"  },
        { "HELMET",          "BUMP",   "HELMET_BUMP" },
        { "JERSEY",          "COL",    "JERSEY_COL"  },
        { "JERSEY",          "NORM",   "JERSEY_NORM" },
        { "NAMEPLATE",       nullptr,  "NAMEPLATE"   },
        { "NAME",            "DECAL",  "NAME_DECAL"  },
        { "NUMS",            "DECAL",  "NUMS_DECAL"  },
        { "NUMBERS",         nullptr,  "NUMBERS"     },
        { "PANTS",           "COL",    "PANTS_COL"   },
        { "PANTS",           "NORM",   "PANTS_NORM"  },
        { "PRIDE",           nullptr,  "PRIDE"       },
        { "SOCKS",           "COL",    "SOCKS_COL"   },
        { "SHOES",           "COL",    "SHOES_COL"   },
        { "SHOES",           "SPEC",   "SHOES_SPEC"  },
        { "SHOES",           "NORM",   "SHOES_NORM"  },
        { nullptr,           nullptr,  nullptr       }
    };

    for (const Rule* r = rules; r->a != nullptr; ++r) {
        const bool match_a = tga_upper.find(r->a) != std::string::npos;
        const bool match_b = (r->b == nullptr) || (tga_upper.find(r->b) != std::string::npos);
        if (match_a && match_b) {
            auto it = table.find(r->key);
            if (it != table.end()) return it->second;
        }
    }
    return std::nullopt; // Unknown texture type — caller must decide
}

} // namespace


// ============================================================================
//  FNV-1a hash (matches EASE's EASEChecksum.HashFNV1a)
// ============================================================================

namespace {

static std::uint64_t fnv1a_hash_bytes(const std::uint8_t* data, std::size_t len) {
    // FNV-1a 64-bit
    std::uint64_t hash = 14695981039346656037ULL;
    for (std::size_t i = 0; i < len; ++i) {
        hash ^= static_cast<std::uint64_t>(data[i]);
        hash *= 1099511628211ULL;
    }
    return hash;
}

static std::vector<std::uint8_t> fnv1a_tag_bytes(const std::string& name) {
    const std::uint64_t h = fnv1a_hash_bytes(
        reinterpret_cast<const std::uint8_t*>(name.data()), name.size());
    std::vector<std::uint8_t> out(8);
    for (int i = 0; i < 8; ++i)
        out[i] = static_cast<std::uint8_t>((h >> (8 * i)) & 0xFF);
    return out;
}

} // namespace


// ============================================================================
//  P3R builder — wraps a DDS blob in the EA P3R header used by PS3 ASTs
//
//  The P3R format used by Madden/NCAA PS3 ASTs is essentially:
//    [magic 4B "p3R\0"] [headerSize 4B=0x7C] [flags 4B] [caps 4B] [caps2 4B]
//    [pixelFmtSize 4B=0x20] [pixelFmtFlags 4B=DDPF_FOURCC] [FourCC 4B]
//    ... (remaining DDS pixel-format fields, 0) ...
//    [width 4B] [height 4B] [mipCount 4B] ... padding to headerSize ...
//    [raw DXT pixel data]
//
//  Rather than parsing every variant, we build the minimal structure that the
//  game engine expects when loading a PS3 AST texture entry.
// ============================================================================

namespace {

static void write_u32le(std::uint8_t* p, std::uint32_t v) {
    p[0] = static_cast<std::uint8_t>(v);
    p[1] = static_cast<std::uint8_t>(v >> 8);
    p[2] = static_cast<std::uint8_t>(v >> 16);
    p[3] = static_cast<std::uint8_t>(v >> 24);
}

/// Build a P3R-wrapped texture blob from a standard DDS.
///
/// The DDS must be a valid block-compressed DDS (DXT1 or DXT5).
/// The returned bytes are suitable for storing directly in a PS3 AST entry.
static std::optional<std::vector<std::uint8_t>> build_p3r_from_dds(
        std::span<const std::uint8_t> dds_bytes,
        std::string* err = nullptr) {

    using namespace gf::textures;

    // Parse the DDS to extract metadata
    const auto dds_info_opt = parse_dds_info(dds_bytes);
    if (!dds_info_opt.has_value()) {
        if (err) *err = "build_p3r_from_dds: could not parse DDS header.";
        return std::nullopt;
    }
    const auto& di = *dds_info_opt;

    // Validate format
    if (di.format != DdsFormat::DXT1 && di.format != DdsFormat::DXT5) {
        if (err) *err = "build_p3r_from_dds: only DXT1/DXT5 DDS input is supported for P3R build.";
        return std::nullopt;
    }

    const std::uint32_t fourcc =
        (di.format == DdsFormat::DXT1) ? 0x31545844u  // "DXT1"
                                        : 0x35545844u; // "DXT5"

    // We store: p3R magic + 0x7C-byte header + raw pixel data (skip DDS header bytes)
    //
    // DDS layout:  4-byte magic + 124-byte DDSURFACEDESC2 = 128 bytes of header
    //              followed by pixel data
    constexpr std::size_t DDS_HEADER_SIZE = 128;
    if (dds_bytes.size() < DDS_HEADER_SIZE) {
        if (err) *err = "build_p3r_from_dds: DDS data is too small.";
        return std::nullopt;
    }

    const std::size_t pixel_data_size = dds_bytes.size() - DDS_HEADER_SIZE;

    // P3R header is 0x7C (124) bytes + 4-byte magic prefix = 128 bytes total
    // (matches EASE's observed P3R structure)
    constexpr std::size_t P3R_HEADER_TOTAL = 128;
    std::vector<std::uint8_t> out(P3R_HEADER_TOTAL + pixel_data_size, 0);

    // Magic: "p3R\0"  (lowercase p, as seen in EASE-produced PS3 ASTs)
    out[0] = 'p'; out[1] = '3'; out[2] = 'R'; out[3] = 0;

    // Header layout (offset 4 onwards):
    std::uint8_t* h = out.data() + 4;

    // dwSize (DDSURFACEDESC2-compatible, 124)
    write_u32le(h +  0, 124);
    // dwFlags: DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PIXELFORMAT | DDSD_MIPMAPCOUNT | DDSD_LINEARSIZE
    write_u32le(h +  4, 0x00081007u);
    write_u32le(h +  8, di.height);
    write_u32le(h + 12, di.width);
    // pitchOrLinearSize — top-mip compressed size
    const std::uint32_t bw = std::max(1u, (di.width  + 3) / 4);
    const std::uint32_t bh = std::max(1u, (di.height + 3) / 4);
    const std::uint32_t block_bytes = (di.format == DdsFormat::DXT1) ? 8 : 16;
    write_u32le(h + 16, bw * bh * block_bytes);
    // depth
    write_u32le(h + 20, 0);
    // mipMapCount
    write_u32le(h + 24, di.mipCount);
    // reserved[11] — zeros at h+28..h+71

    // DDPIXELFORMAT at offset 72 (h+72 = out+76 = p+76 of the 124-byte struct)
    std::uint8_t* pf = h + 72;
    write_u32le(pf + 0, 32);          // dwSize
    write_u32le(pf + 4, 0x4u);        // DDPF_FOURCC
    write_u32le(pf + 8, fourcc);
    // remaining pf fields: 0

    // DDSCAPS at h+104
    std::uint8_t* caps = h + 104;
    std::uint32_t caps1 = 0x1000u; // DDSCAPS_TEXTURE
    if (di.mipCount > 1) caps1 |= 0x400408u; // DDSCAPS_COMPLEX | DDSCAPS_MIPMAP
    write_u32le(caps + 0, caps1);

    // Copy pixel data directly after the P3R header
    std::memcpy(out.data() + P3R_HEADER_TOTAL,
                dds_bytes.data() + DDS_HEADER_SIZE,
                pixel_data_size);

    return out;
}

} // namespace


// ============================================================================
//  XPR2 builder — wraps DDS pixel data in an Xbox 360 XPR2 container
//
//  ASTra Core already has dds_to_xpr2_patch() which patches an EXISTING XPR2's
//  texture data.  For the RSF-based workflow we don't have donor XPR2 files
//  at hand, so we build minimal XPR2 containers from scratch.
//
//  XPR2 format (big-endian on Xbox 360):
//    [magic 0x32525058 "XPR2"] [file_info_offset 4B BE] [data_offset 4B BE]
//    [data_length 4B BE] [resource_count 4B BE]
//    [resources: (ResType 4B, Offset 4B, Size 4B, NameOffset 4B) each BE]
//    ... padding to data_offset ...
//    [TX2D header at data_offset]
//    [pixel data]
//
//  This builder produces the minimal structure required for Madden/NCAA Xbox
//  AST loading.  The TX2D sub-resource header is left minimal (non-tiled
//  layout) which is acceptable for a static texture replacement used in mods.
// ============================================================================

namespace {

static void write_u32be(std::uint8_t* p, std::uint32_t v) {
    p[0] = static_cast<std::uint8_t>(v >> 24);
    p[1] = static_cast<std::uint8_t>(v >> 16);
    p[2] = static_cast<std::uint8_t>(v >> 8);
    p[3] = static_cast<std::uint8_t>(v);
}

/// Maps a DdsFormat to the Xbox 360 D3DFORMAT constant used in TX2D headers.
static std::uint32_t dds_format_to_d3d(gf::textures::DdsFormat fmt) {
    using gf::textures::DdsFormat;
    switch (fmt) {
    case DdsFormat::DXT1: return 0x52u; // D3DFMT_DXT1
    case DdsFormat::DXT5: return 0x54u; // D3DFMT_DXT5
    default:              return 0x52u;
    }
}

/// Build a minimal XPR2 container from DDS pixel data.
///
/// The resulting XPR2 is suitable for storage in an Xbox 360 AST entry.
/// Note: This builds in LINEAR (non-tiled) layout which is sufficient for
/// most modding purposes.  For perfect in-game fidelity the data should be
/// tiled, but that requires the XGAddress2DTiledOffset algorithm which is
/// only reliable for power-of-two dimensions.  We choose compatibility
/// (non-tiled but correct content) over game-engine tiling optimisation.
static std::optional<std::vector<std::uint8_t>> build_xpr2_from_dds(
        std::span<const std::uint8_t> dds_bytes,
        const std::string& texture_name,
        std::string* err = nullptr) {

    using namespace gf::textures;

    const auto dds_info_opt = parse_dds_info(dds_bytes);
    if (!dds_info_opt.has_value()) {
        if (err) *err = "build_xpr2_from_dds: could not parse DDS header.";
        return std::nullopt;
    }
    const auto& di = *dds_info_opt;

    if (di.format != DdsFormat::DXT1 && di.format != DdsFormat::DXT5) {
        if (err) *err = "build_xpr2_from_dds: only DXT1/DXT5 is supported.";
        return std::nullopt;
    }

    constexpr std::size_t DDS_HEADER_SIZE = 128;
    if (dds_bytes.size() < DDS_HEADER_SIZE) {
        if (err) *err = "build_xpr2_from_dds: DDS too small.";
        return std::nullopt;
    }
    const std::size_t pixel_size = dds_bytes.size() - DDS_HEADER_SIZE;

    // Layout:
    //   [0x00]  XPR2 header  (32 bytes)
    //   [0x20]  1 RESOURCE descriptor (16 bytes)
    //   [0x30]  Name string region (texture_name + null, padded to 16)
    //   [?]     TX2D sub-header (64 bytes, big-endian)
    //   [?]     Pixel data
    //
    // All section boundaries 16-byte aligned.

    const std::string safe_name = texture_name.empty() ? "texture" : texture_name;
    const std::size_t name_raw  = safe_name.size() + 1; // +null
    const std::size_t name_padded = (name_raw + 15) & ~std::size_t(15);

    constexpr std::size_t XPR2_HEADER_SIZE   = 32;
    constexpr std::size_t RESOURCE_ENTRY_SIZE = 16;
    constexpr std::size_t TX2D_HEADER_SIZE    = 64;

    const std::size_t file_info_offset = XPR2_HEADER_SIZE; // where RESOURCE table starts
    const std::size_t data_offset      = XPR2_HEADER_SIZE + RESOURCE_ENTRY_SIZE + name_padded;
    const std::size_t data_length      = TX2D_HEADER_SIZE + pixel_size;

    std::vector<std::uint8_t> out(data_offset + data_length, 0);

    // ── XPR2 header ──────────────────────────────────────────────────────────
    // Magic: big-endian "XPR2"
    out[0] = 'X'; out[1] = 'P'; out[2] = 'R'; out[3] = '2';
    write_u32be(out.data() +  4, static_cast<std::uint32_t>(file_info_offset));
    write_u32be(out.data() +  8, static_cast<std::uint32_t>(data_offset));
    write_u32be(out.data() + 12, static_cast<std::uint32_t>(data_length));
    write_u32be(out.data() + 16, 1); // resource count
    // bytes 20..31 reserved, 0

    // ── RESOURCE descriptor ───────────────────────────────────────────────────
    std::uint8_t* res = out.data() + file_info_offset;
    write_u32be(res +  0, 0x80070002u); // ResType: D3D TX2D
    write_u32be(res +  4, 0u);          // offset from data_offset to TX2D header
    write_u32be(res +  8, static_cast<std::uint32_t>(data_length));
    // name offset from start of name region (immediately after RESOURCE table)
    write_u32be(res + 12, 0u); // name at offset 0 of name region

    // ── Name region ───────────────────────────────────────────────────────────
    std::uint8_t* nptr = out.data() + file_info_offset + RESOURCE_ENTRY_SIZE;
    std::memcpy(nptr, safe_name.c_str(), name_raw); // includes null terminator

    // ── TX2D sub-header (at data_offset) ─────────────────────────────────────
    std::uint8_t* tx2d = out.data() + data_offset;
    // TX2D identifier
    tx2d[0]='T'; tx2d[1]='X'; tx2d[2]='2'; tx2d[3]='D';
    write_u32be(tx2d +  4, static_cast<std::uint32_t>(TX2D_HEADER_SIZE)); // header size
    write_u32be(tx2d +  8, static_cast<std::uint32_t>(pixel_size));        // data size
    write_u32be(tx2d + 12, di.width);
    write_u32be(tx2d + 16, di.height);
    write_u32be(tx2d + 20, di.mipCount);
    write_u32be(tx2d + 24, dds_format_to_d3d(di.format));
    write_u32be(tx2d + 28, 1); // array_size = 1
    // remaining TX2D fields: 0 (linear layout, no tiling flags)

    // ── Pixel data ────────────────────────────────────────────────────────────
    std::memcpy(out.data() + data_offset + TX2D_HEADER_SIZE,
                dds_bytes.data() + DDS_HEADER_SIZE,
                pixel_size);

    return out;
}

} // namespace


// ============================================================================
//  BGFA raw container builder
//
//  We build the AST/BGFA binary directly using the same wire format as
//  AstContainerEditor (which we already understand from reading the source).
//  We do NOT use AstContainerBuilder here because that requires a donor AST
//  profile — this feature must produce ASTs from scratch to match EASE.
//
//  Header format (little-endian):
//    [0x00]  "BGFA"          4B  magic
//    [0x04]  magicV          4B  (version, 0 for compatibility)
//    [0x08]  fakeFileCount   4B
//    [0x0C]  fileCount       4B
//    [0x10]  dirOffset       8B  (= 64 = 0x40)
//    [0x18]  dirSize         8B
//    [0x20]  nametype        1B
//    [0x21]  flagtype        1B  (1 → flags field is 2 bytes)
//    [0x22]  tagsize         1B  (8 → 8-byte FNV1a hash)
//    [0x23]  offsetsize      1B  (4)
//    [0x24]  compsize        1B  (4)
//    [0x25]  sizediffsize    1B  (4)
//    [0x26]  shiftsize       1B  (11 → 2048-byte alignment)
//    [0x27]  unknownsize     1B  (0)
//    [0x28]  tagCount        4B
//    [0x2C]  fileNameLength  4B  (= 64)
//    [0x30..0x3F] reserved   16B
//    --- directory ---
//    [tagCount × 4B tag IDs (little-endian uint32)]
//    [fileCount × record]:
//      flags       2B   (flagtype=1 → 1+1=2 bytes)
//      tagBytes    8B   (tagsize=8)
//      offset      4B   (offsetsize=4, value >> shiftsize)
//      compSize    4B   (compsize=4)
//      sizeDiff    4B   (sizediffsize=4, uncompressed - compressed if > 0)
//      name        64B  (fileNameLength=64)
//    --- data (2048-byte aligned) ---
// ============================================================================

namespace {

// EASE AST entry flags: ZLIB | FNV1A_HASH (matches EASE BGFA export)
constexpr std::uint16_t AST_FLAGS_EASE = 0x0003;

// TAG IDs (match EASE hard-coded values exactly)
constexpr std::uint32_t TAG_RSF  = 0x78B0EF25u; // 2025518437
constexpr std::uint32_t TAG_NAME = 0x8FFFB4D9u; // 2414196441 (.NAME data)
constexpr std::uint32_t TAG_P3R  = 0x6C1D7D14u; // 1814246228 (PS3 texture)
constexpr std::uint32_t TAG_XPR  = 0x2E57E892u; // 777539666  (Xbox texture)

// Header field constants matching the EASE-derived BGFA schema
constexpr std::uint8_t  NAMETYPE    = 0;
constexpr std::uint8_t  FLAGTYPE    = 1;    // 2-byte flags
constexpr std::uint8_t  TAGSIZE     = 8;    // 8-byte FNV1a tag bytes
constexpr std::uint8_t  OFFSETSIZE  = 4;
constexpr std::uint8_t  COMPSIZE    = 4;
constexpr std::uint8_t  SIZEDIFF    = 4;
constexpr std::uint8_t  SHIFTSIZE   = 11;   // 2048-byte alignment
constexpr std::uint8_t  UNKNOWNSIZE = 0;
constexpr std::uint32_t FILENAME_LEN = 64;

struct BgfaEntry {
    std::uint16_t               flags     = AST_FLAGS_EASE;
    std::vector<std::uint8_t>   tag_bytes;     // 8 bytes (FNV1a of FileDesc)
    std::uint32_t               tag_id_index;  // index into tag_id list
    std::vector<std::uint8_t>   name_bytes;    // 64 bytes (padded FileDesc)
    std::vector<std::uint8_t>   payload;       // already zlib-compressed if flags say so
    std::uint64_t               uncompressed_size = 0;
};

// Zlib deflate
static std::vector<std::uint8_t> zlib_deflate(
        const std::vector<std::uint8_t>& in, std::string* err) {
    // We include zlib via gf_core — the library already uses it.
    // Re-implement the deflate call here to avoid pulling in gf_core internals.
    z_stream zs{};
    if (deflateInit(&zs, Z_BEST_COMPRESSION) != Z_OK) {
        if (err) *err = "deflateInit failed";
        return {};
    }
    zs.next_in  = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(in.data()));
    zs.avail_in = static_cast<uInt>(in.size());

    std::vector<std::uint8_t> out;
    std::array<std::uint8_t, 64 * 1024> tmp{};
    int rc = Z_OK;
    while (rc != Z_STREAM_END) {
        zs.next_out  = reinterpret_cast<Bytef*>(tmp.data());
        zs.avail_out = static_cast<uInt>(tmp.size());
        rc = deflate(&zs, zs.avail_in == 0 ? Z_FINISH : Z_NO_FLUSH);
        const std::size_t n = tmp.size() - zs.avail_out;
        out.insert(out.end(), tmp.data(), tmp.data() + n);
        if (rc != Z_OK && rc != Z_STREAM_END) {
            deflateEnd(&zs);
            if (err) *err = "deflate failed";
            return {};
        }
    }
    deflateEnd(&zs);
    return out;
}

static void write_u16le_v(std::uint8_t* p, std::uint16_t v) {
    p[0] = static_cast<std::uint8_t>(v);
    p[1] = static_cast<std::uint8_t>(v >> 8);
}
static void write_u32le_v(std::uint8_t* p, std::uint32_t v) {
    p[0] = static_cast<std::uint8_t>(v);
    p[1] = static_cast<std::uint8_t>(v >>  8);
    p[2] = static_cast<std::uint8_t>(v >> 16);
    p[3] = static_cast<std::uint8_t>(v >> 24);
}
static void write_u64le_v(std::uint8_t* p, std::uint64_t v) {
    for (int i = 0; i < 8; ++i)
        p[i] = static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF);
}

/// Build a complete BGFA/AST binary from a list of entries and their tag IDs.
///
/// @param tag_ids    Ordered list of unique tag IDs used in the container.
/// @param entries    The entries to pack; each entry must have tag_id_index set.
/// @param err        Filled on failure.
/// @return           Full AST bytes, or empty on failure.
static std::vector<std::uint8_t> build_bgfa(
        const std::vector<std::uint32_t>& tag_ids,
        std::vector<BgfaEntry>&           entries,
        std::string*                      err) {

    // Compress each entry payload
    for (auto& e : entries) {
        if (e.flags & 0x01) { // ZLIB flag set
            e.uncompressed_size = e.payload.size();
            std::string local_err;
            e.payload = zlib_deflate(e.payload, &local_err);
            if (e.payload.empty() && e.uncompressed_size > 0) {
                if (err) *err = "Zlib compression failed: " + local_err;
                return {};
            }
        } else {
            e.uncompressed_size = e.payload.size();
        }
    }

    const std::uint32_t tag_count  = static_cast<std::uint32_t>(tag_ids.size());
    const std::uint32_t file_count = static_cast<std::uint32_t>(entries.size());

    // Record size = flagsWidth + tagsize + offsetsize + compsize + sizediffsize + fileNameLength
    constexpr std::size_t FLAGS_WIDTH = 1 + FLAGTYPE; // 2
    constexpr std::size_t RECORD_SIZE = FLAGS_WIDTH + TAGSIZE + OFFSETSIZE
                                      + COMPSIZE + SIZEDIFF + FILENAME_LEN;

    const std::uint64_t dir_offset = 64;
    const std::uint64_t dir_size   = static_cast<std::uint64_t>(tag_count) * 4
                                   + static_cast<std::uint64_t>(file_count) * RECORD_SIZE;

    // Data region starts after directory, aligned to 2048 bytes
    constexpr std::uint64_t ALIGN = 1ULL << SHIFTSIZE; // 2048
    const std::uint64_t data_start_raw = dir_offset + dir_size;
    const std::uint64_t data_start = (data_start_raw + ALIGN - 1) & ~(ALIGN - 1);

    // Assign data offsets
    std::vector<std::uint64_t> offsets(entries.size());
    std::uint64_t cursor = data_start;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        // Align each entry to ALIGN boundary
        cursor = (cursor + ALIGN - 1) & ~(ALIGN - 1);
        offsets[i] = cursor;
        cursor += entries[i].payload.size();
    }
    const std::uint64_t total_size = cursor;

    std::vector<std::uint8_t> out(static_cast<std::size_t>(total_size), 0);
    std::uint8_t* p = out.data();

    // ── Main header ──────────────────────────────────────────────────────────
    p[0]='B'; p[1]='G'; p[2]='F'; p[3]='A';
    write_u32le_v(p +  4, 0);                      // magicV
    write_u32le_v(p +  8, file_count);             // fakeFileCount = fileCount (EASE default)
    write_u32le_v(p + 12, file_count);             // fileCount
    write_u64le_v(p + 16, dir_offset);
    write_u64le_v(p + 24, dir_size);
    p[32] = NAMETYPE;
    p[33] = FLAGTYPE;
    p[34] = TAGSIZE;
    p[35] = OFFSETSIZE;
    p[36] = COMPSIZE;
    p[37] = SIZEDIFF;
    p[38] = SHIFTSIZE;
    p[39] = UNKNOWNSIZE;
    write_u32le_v(p + 40, tag_count);
    write_u32le_v(p + 44, FILENAME_LEN);
    // 48..63 reserved, already zero

    // ── Tag list ─────────────────────────────────────────────────────────────
    std::uint8_t* dp = p + dir_offset;
    for (std::uint32_t t : tag_ids) {
        write_u32le_v(dp, t);
        dp += 4;
    }

    // ── Directory entries ─────────────────────────────────────────────────────
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const auto& e = entries[i];
        const std::uint64_t off = offsets[i];

        // flags (2 bytes)
        write_u16le_v(dp, e.flags);
        dp += 2;

        // tag bytes (8 bytes)
        std::vector<std::uint8_t> tb = e.tag_bytes;
        tb.resize(TAGSIZE, 0);
        std::memcpy(dp, tb.data(), TAGSIZE);
        dp += TAGSIZE;

        // offset >> shiftsize (4 bytes)
        write_u32le_v(dp, static_cast<std::uint32_t>(off >> SHIFTSIZE));
        dp += 4;

        // compressed size (4 bytes)
        write_u32le_v(dp, static_cast<std::uint32_t>(e.payload.size()));
        dp += 4;

        // size diff (4 bytes): uncompressed - compressed, or 0
        const std::uint64_t diff = (e.uncompressed_size > e.payload.size())
                                 ? (e.uncompressed_size - e.payload.size())
                                 : 0;
        write_u32le_v(dp, static_cast<std::uint32_t>(diff));
        dp += 4;

        // name bytes (64 bytes, zero-padded)
        std::vector<std::uint8_t> nb = e.name_bytes;
        nb.resize(FILENAME_LEN, 0);
        std::memcpy(dp, nb.data(), FILENAME_LEN);
        dp += FILENAME_LEN;
    }

    // ── Payload data ──────────────────────────────────────────────────────────
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const auto& e = entries[i];
        if (!e.payload.empty()) {
            std::memcpy(p + offsets[i], e.payload.data(), e.payload.size());
        }
    }

    return out;
}

// Build a BgfaEntry from a file descriptor string and payload bytes,
// assigning the correct tag_id_index from the provided tag_ids vector.
// Returns nullopt if the tag_id is not found in tag_ids (shouldn't happen if
// the caller adds tags before creating entries).
static BgfaEntry make_entry(const std::string&                file_desc,
                             const std::vector<std::uint8_t>&  raw_payload,
                             std::uint32_t                     tag_id,
                             const std::vector<std::uint32_t>& tag_ids) {
    BgfaEntry e;
    e.flags     = AST_FLAGS_EASE;
    e.tag_bytes = fnv1a_tag_bytes(file_desc);
    e.payload   = raw_payload;

    auto it = std::find(tag_ids.begin(), tag_ids.end(), tag_id);
    e.tag_id_index = static_cast<std::uint32_t>(
        it != tag_ids.end() ? (it - tag_ids.begin()) : 0);

    // name_bytes: file_desc padded to FILENAME_LEN
    e.name_bytes.resize(FILENAME_LEN, 0);
    const auto copy_n = std::min<std::size_t>(file_desc.size(), FILENAME_LEN);
    std::memcpy(e.name_bytes.data(), file_desc.data(), copy_n);

    return e;
}

} // namespace


// ============================================================================
//  RsfAstValidator
// ============================================================================

namespace {

struct ValidationResult {
    bool ok = false;
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
};

static ValidationResult validate_input_folder(const fs::path& folder) {
    ValidationResult res;
    std::error_code ec;

    if (!fs::exists(folder, ec) || !fs::is_directory(folder, ec)) {
        res.errors.push_back("The selected path does not exist or is not a directory: " + folder.string());
        return res;
    }

    std::vector<fs::path> rsf_files, tga_files, dds_files, other_files;

    for (const auto& entry : fs::directory_iterator(folder, ec)) {
        if (ec) break;
        if (!entry.is_regular_file(ec)) continue;
        if (is_junk_file(entry.path())) continue;

        const auto ext = ext_upper(entry.path());
        if      (ext == ".RSF") rsf_files.push_back(entry.path());
        else if (ext == ".TGA") tga_files.push_back(entry.path());
        else if (ext == ".DDS") dds_files.push_back(entry.path());
        else if (ext != ".AST" && ext != ".EXE" && !ends_with_ci(entry.path().filename().string(), "BIG"))
            other_files.push_back(entry.path());
    }

    // Must have exactly one RSF
    if (rsf_files.empty()) {
        res.errors.push_back(
            "No .RSF file found in the selected folder.\n"
            "The folder must contain exactly one .rsf file that defines the AST layout.");
    } else if (rsf_files.size() > 1) {
        std::string names;
        for (const auto& r : rsf_files)
            names += "\n  " + r.filename().string();
        res.errors.push_back(
            "Multiple .RSF files found in the folder — this is ambiguous:" + names + "\n"
            "Remove all but the one RSF that belongs to this asset set.");
    }

    // Must have at least one TGA
    if (tga_files.empty()) {
        res.errors.push_back(
            "No .TGA files found in the selected folder.\n"
            "This workflow requires TGA source textures. Provide the original .tga files.");
    }

    // DDS files as source input are explicitly rejected
    if (!dds_files.empty()) {
        std::string names;
        for (const auto& d : dds_files)
            names += "\n  " + d.filename().string();
        res.errors.push_back(
            "DDS files detected in the input folder — these cannot be used as source textures "
            "for the RSF-based AST creation workflow:" + names + "\n"
            "Remove the .dds files. This tool converts TGA → DDS internally.\n"
            "If you already have converted DDS files from a previous run, delete them and "
            "re-run from the original .tga sources.");
    }

    // Check for duplicate TGA base names (case-insensitive)
    {
        std::map<std::string, std::vector<std::string>> by_base;
        for (const auto& t : tga_files) {
            const auto base = lower_ascii(t.stem().string());
            by_base[base].push_back(t.filename().string());
        }
        for (const auto& [base, names] : by_base) {
            if (names.size() > 1) {
                std::string clash;
                for (const auto& n : names) clash += "\n  " + n;
                res.warnings.push_back(
                    "Duplicate TGA base name '" + base + "' (case-insensitive):" + clash);
            }
        }
    }

    // Warn about unrecognized texture names
    for (const auto& t : tga_files) {
        const auto up = upper_ascii(t.stem().string());
        if (!lookup_rdf(up).has_value()) {
            res.warnings.push_back(
                "Unrecognized TGA name '" + t.filename().string() +
                "' — no RDF descriptor matched. The texture will be compressed at its "
                "source dimensions using DXT1. Verify the filename matches a known "
                "pattern (e.g. JERSEY_COL, HELMET_COL, PANTS_NORM, ...).");
        }
    }

    res.ok = res.errors.empty();
    return res;
}

} // namespace


// ============================================================================
//  RsfAstBuildPipeline
// ============================================================================

namespace {

struct PipelineLog {
    std::vector<std::string> lines;
    void info (const std::string& s) { lines.push_back("[INFO]  " + s); }
    void warn (const std::string& s) { lines.push_back("[WARN]  " + s); }
    void error(const std::string& s) { lines.push_back("[ERROR] " + s); }
};

/// Classify a file path into one of the categories CreateRSFbasedAST uses.
enum class PayloadClass { RSF, P3R_TEX, XPR_TEX, NAME_DATA, SKIP };

static PayloadClass classify_payload_file(const fs::path& p) {
    const auto ext = ext_upper(p);
    if (ext == ".RSF")  return PayloadClass::RSF;
    if (ext == ".P3R")  return PayloadClass::P3R_TEX;
    if (ext == ".XPR")  return PayloadClass::XPR_TEX;
    if (ext == ".RDF" || ext == ".TGA" || ext == ".AST" || ext == ".EXE") return PayloadClass::SKIP;
    if (ends_with_ci(p.filename().string(), "BIG")) return PayloadClass::SKIP;
    // .NAME and generic data files
    const auto fn_up = upper_ascii(p.filename().string());
    if (ends_with_ci(fn_up, "NAME")) return PayloadClass::NAME_DATA;
    return PayloadClass::NAME_DATA; // treat unknown binary files as generic data
}

/// Run the full build pipeline.
static RsfAstBuildResult run_pipeline_impl(RsfAstBuildRequest req) {
    RsfAstBuildResult result;
    PipelineLog log;

    log.info("=== RSF-based AST build started ===");
    log.info("Input folder: " + req.input_folder.string());

    // ── Step 1: validate ─────────────────────────────────────────────────────
    log.info("Stage 1/7: Validating input folder…");
    const auto vr = validate_input_folder(req.input_folder);
    for (const auto& w : vr.warnings) { log.warn(w); result.warnings.push_back(w); }
    if (!vr.ok) {
        for (const auto& e : vr.errors) { log.error(e); result.errors.push_back(e); }
        result.log = std::move(log.lines);
        return result;
    }

    // Re-scan to populate request fields
    std::error_code ec;
    req.rsf_path.clear();
    req.tga_files.clear();
    req.payload_files.clear();

    for (const auto& entry : fs::directory_iterator(req.input_folder, ec)) {
        if (!entry.is_regular_file(ec)) continue;
        if (is_junk_file(entry.path())) continue;
        const auto ext = ext_upper(entry.path());
        if      (ext == ".RSF") req.rsf_path = entry.path();
        else if (ext == ".TGA") req.tga_files.push_back(entry.path());
        else if (ext != ".DDS" && ext != ".AST" && ext != ".EXE"
                 && !ends_with_ci(entry.path().filename().string(), "BIG"))
            req.payload_files.push_back(entry.path());
    }

    std::sort(req.tga_files.begin(), req.tga_files.end());
    std::sort(req.payload_files.begin(), req.payload_files.end());

    // ── Step 2: scan RSF ─────────────────────────────────────────────────────
    log.info("Stage 2/7: Scanning RSF: " + req.rsf_path.filename().string());
    const auto rsf_bytes = read_file(req.rsf_path, nullptr);
    if (rsf_bytes.empty()) {
        const auto e = "Failed to read RSF file: " + req.rsf_path.string();
        log.error(e); result.errors.push_back(e);
        result.log = std::move(log.lines);
        return result;
    }
    log.info("RSF size: " + std::to_string(rsf_bytes.size()) + " bytes");

    // The asset name is derived from the RSF filename (shortest basename wins — mirrors EASE)
    std::string asset_name = req.rsf_path.stem().string();
    // Check TGA names too — shortest basename wins (exact EASE behaviour)
    for (const auto& t : req.tga_files) {
        const auto n = t.stem().string();
        if (n.size() < asset_name.size()) asset_name = n;
    }
    log.info("Asset name: " + asset_name);

    // Determine output folder
    if (req.output_folder.empty()) req.output_folder = req.input_folder;

    // ── Step 3: collect TGA sources ──────────────────────────────────────────
    log.info("Stage 3/7: Collecting TGA sources (" +
             std::to_string(req.tga_files.size()) + " file(s))…");
    for (const auto& t : req.tga_files)
        log.info("  TGA: " + t.filename().string());

    // ── Step 4: convert TGA → DDS (PS3) and TGA → XPR2 (Xbox) ──────────────
    log.info("Stage 4/7: Converting textures…");

    struct ConvertedTexture {
        std::string base_name;  // stem, no extension
        std::vector<std::uint8_t> dds_bytes;   // standard DDS
        std::vector<std::uint8_t> p3r_bytes;   // P3R-wrapped DDS  (PS3)
        std::vector<std::uint8_t> xpr2_bytes;  // XPR2 container   (Xbox)
    };

    std::vector<ConvertedTexture> converted_textures;
    bool any_texture_failed = false;

    for (const auto& tga_path : req.tga_files) {
        const auto tga_name_upper = upper_ascii(tga_path.stem().string());
        const auto base = tga_path.stem().string();

        log.info("  Converting: " + tga_path.filename().string());

        // Read TGA
        std::string read_err;
        const auto tga_bytes = read_file(tga_path, &read_err);
        if (tga_bytes.empty()) {
            log.error("  Failed to read TGA: " + read_err);
            any_texture_failed = true; continue;
        }

        // Look up RDF descriptor
        const auto rdf_opt = lookup_rdf(tga_name_upper);
        gf::textures::DdsBuildParams params;
        if (rdf_opt.has_value()) {
            const auto& rdf = *rdf_opt;
            params.target_w  = rdf.width;
            params.target_h  = rdf.height;
            params.mip_count = rdf.mips;
            params.format    = (rdf.d3dfmt == "D3DFMT_DXT5")
                              ? gf::textures::DdsBuildFormat::DXT5
                              : gf::textures::DdsBuildFormat::DXT1;
            log.info("  RDF match: " + rdf.d3dfmt +
                     " " + std::to_string(rdf.width) + "x" + std::to_string(rdf.height) +
                     " mips=" + std::to_string(rdf.mips));
        } else {
            // No RDF match: use source dimensions, DXT1 as fallback
            params.format    = gf::textures::DdsBuildFormat::DXT1;
            params.target_w  = 0; // keep source dims
            params.target_h  = 0;
            params.mip_count = 0; // full chain
            log.warn("  No RDF descriptor for '" + tga_path.filename().string() +
                     "' — using DXT1 at source dimensions.");
        }

        // Build DDS
        std::string conv_err;
        auto dds = gf::textures::build_dds_from_tga(
            std::span<const std::uint8_t>(tga_bytes.data(), tga_bytes.size()),
            params, &conv_err);
        if (dds.empty()) {
            log.error("  DDS conversion failed for " + tga_path.filename().string() + ": " + conv_err);
            any_texture_failed = true; continue;
        }
        log.info("  DDS built: " + std::to_string(dds.size()) + " bytes");

        // Build P3R from DDS  (PS3 target)
        std::string p3r_err;
        auto p3r = build_p3r_from_dds(
            std::span<const std::uint8_t>(dds.data(), dds.size()), &p3r_err);
        if (!p3r.has_value()) {
            log.error("  P3R build failed: " + p3r_err);
            any_texture_failed = true; continue;
        }

        // Build XPR2 from DDS  (Xbox target)
        std::string xpr_err;
        auto xpr2 = build_xpr2_from_dds(
            std::span<const std::uint8_t>(dds.data(), dds.size()), base, &xpr_err);
        if (!xpr2.has_value()) {
            log.error("  XPR2 build failed: " + xpr_err);
            any_texture_failed = true; continue;
        }

        log.info("  P3R: " + std::to_string(p3r->size()) + " bytes   "
                 "XPR2: " + std::to_string(xpr2->size()) + " bytes");

        ConvertedTexture ct;
        ct.base_name  = base;
        ct.dds_bytes  = std::move(dds);
        ct.p3r_bytes  = std::move(*p3r);
        ct.xpr2_bytes = std::move(*xpr2);
        converted_textures.push_back(std::move(ct));
    }

    if (any_texture_failed) {
        const auto e = "One or more texture conversions failed. See log for details.";
        log.error(e);
        result.errors.push_back(e);
        result.log = std::move(log.lines);
        return result;
    }

    // ── Step 5: build Xbox AST ───────────────────────────────────────────────
    log.info("Stage 5/7: Building Xbox AST…");
    {
        // Tag list for Xbox: collect tags needed
        std::vector<std::uint32_t> tag_ids;
        std::vector<BgfaEntry>     entries;

        auto ensure_tag = [&](std::uint32_t id) {
            if (std::find(tag_ids.begin(), tag_ids.end(), id) == tag_ids.end())
                tag_ids.push_back(id);
        };

        // RSF entry
        ensure_tag(TAG_RSF);
        entries.push_back(make_entry(req.rsf_path.stem().string(), rsf_bytes, TAG_RSF, tag_ids));

        // Xbox texture (XPR2) entries
        for (const auto& ct : converted_textures) {
            ensure_tag(TAG_XPR);
            entries.push_back(make_entry(ct.base_name, ct.xpr2_bytes, TAG_XPR, tag_ids));
        }

        // Non-texture payload files (skip P3R — Xbox only takes XPR)
        for (const auto& fp : req.payload_files) {
            const auto cls = classify_payload_file(fp);
            if (cls == PayloadClass::SKIP || cls == PayloadClass::P3R_TEX) continue;
            std::string rerr;
            auto data = read_file(fp, &rerr);
            if (data.empty()) { log.warn("Skipping payload file (read error): " + fp.string()); continue; }
            const auto base = fp.stem().string();
            if (cls == PayloadClass::XPR_TEX) {
                ensure_tag(TAG_XPR);
                entries.push_back(make_entry(base, data, TAG_XPR, tag_ids));
            } else {
                ensure_tag(TAG_NAME);
                entries.push_back(make_entry(base, data, TAG_NAME, tag_ids));
            }
        }


        std::string bgfa_err;
        const auto xbox_bytes = build_bgfa(tag_ids, entries, &bgfa_err);
        if (xbox_bytes.empty()) {
            log.error("Xbox AST build failed: " + bgfa_err);
            result.errors.push_back("Xbox AST build failed: " + bgfa_err);
            result.log = std::move(log.lines);
            return result;
        }

        const auto out_name = "X-" + asset_name + "_BIG";
        const auto out_path = req.output_folder / out_name;
        std::string write_err;
        if (!write_file(out_path, xbox_bytes, &write_err)) {
            log.error("Failed to write Xbox AST: " + write_err);
            result.errors.push_back("Failed to write Xbox AST: " + write_err);
            result.log = std::move(log.lines);
            return result;
        }
        result.xbox_output = out_path;
        log.info("Xbox AST written: " + out_path.string()
                 + " (" + std::to_string(xbox_bytes.size()) + " bytes)");
    }

    // ── Step 6: build PS3 AST ────────────────────────────────────────────────
    log.info("Stage 6/7: Building PS3 AST…");
    {
        std::vector<std::uint32_t> tag_ids;
        std::vector<BgfaEntry>     entries;

        auto ensure_tag = [&](std::uint32_t id) {
            if (std::find(tag_ids.begin(), tag_ids.end(), id) == tag_ids.end())
                tag_ids.push_back(id);
        };

        // RSF entry
        ensure_tag(TAG_RSF);
        entries.push_back(make_entry(req.rsf_path.stem().string(), rsf_bytes, TAG_RSF, tag_ids));

        // PS3 texture (P3R) entries
        for (const auto& ct : converted_textures) {
            ensure_tag(TAG_P3R);
            entries.push_back(make_entry(ct.base_name, ct.p3r_bytes, TAG_P3R, tag_ids));
        }

        // Non-texture payload files (skip XPR — PS3 only takes P3R)
        for (const auto& fp : req.payload_files) {
            const auto cls = classify_payload_file(fp);
            if (cls == PayloadClass::SKIP || cls == PayloadClass::XPR_TEX) continue;
            std::string rerr;
            auto data = read_file(fp, &rerr);
            if (data.empty()) { log.warn("Skipping payload file (read error): " + fp.string()); continue; }
            const auto base = fp.stem().string();
            if (cls == PayloadClass::P3R_TEX) {
                ensure_tag(TAG_P3R);
                entries.push_back(make_entry(base, data, TAG_P3R, tag_ids));
            } else {
                ensure_tag(TAG_NAME);
                entries.push_back(make_entry(base, data, TAG_NAME, tag_ids));
            }
        }

        std::string bgfa_err;
        const auto ps3_bytes = build_bgfa(tag_ids, entries, &bgfa_err);
        if (ps3_bytes.empty()) {
            log.error("PS3 AST build failed: " + bgfa_err);
            result.errors.push_back("PS3 AST build failed: " + bgfa_err);
            result.log = std::move(log.lines);
            return result;
        }

        const auto out_name = "P-" + asset_name + "_BIG";
        const auto out_path = req.output_folder / out_name;
        std::string write_err;
        if (!write_file(out_path, ps3_bytes, &write_err)) {
            log.error("Failed to write PS3 AST: " + write_err);
            result.errors.push_back("Failed to write PS3 AST: " + write_err);
            result.log = std::move(log.lines);
            return result;
        }
        result.ps3_output = out_path;
        log.info("PS3 AST written: " + out_path.string()
                 + " (" + std::to_string(ps3_bytes.size()) + " bytes)");
    }

    // ── Step 7: done ─────────────────────────────────────────────────────────
    log.info("Stage 7/7: Build complete.");
    log.info("  Xbox: " + result.xbox_output.string());
    log.info("  PS3:  " + result.ps3_output.string());

    result.success = true;
    result.log = std::move(log.lines);
    return result;
}

} // namespace


// ============================================================================
//  CreateRsfAstDialog — static pipeline entry point
// ============================================================================

RsfAstBuildResult CreateRsfAstDialog::run_pipeline(RsfAstBuildRequest req) {
    return run_pipeline_impl(std::move(req));
}


// ============================================================================
//  CreateRsfAstDialog — constructor / UI
// ============================================================================

CreateRsfAstDialog::CreateRsfAstDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle("Create RSF-Based AST");
    setMinimumSize(700, 520);
    resize(860, 580);

    auto* root = new QVBoxLayout(this);
    root->setSpacing(8);

    // ── Description ────────────────────────────────────────────────────────
    auto* desc = new QLabel(
        "<b>RSF-Based AST Creation</b><br>"
        "Reproduces the EASE AST Editor 2.0 workflow. Select a folder containing "
        "a <tt>.rsf</tt> file and one or more <tt>.tga</tt> texture files. "
        "The tool will produce two platform-specific AST outputs "
        "(<tt>X-&lt;name&gt;_BIG</tt> for Xbox&nbsp;360 and "
        "<tt>P-&lt;name&gt;_BIG</tt> for PS3).",
        this);
    desc->setWordWrap(true);
    root->addWidget(desc);

    // ── Input folder row ───────────────────────────────────────────────────
    auto* form = new QGridLayout();
    form->setColumnStretch(1, 1);

    form->addWidget(new QLabel("RSF/TGA Asset Folder:", this), 0, 0);
    m_inputEdit = new QLineEdit(this);
    m_inputEdit->setPlaceholderText("Select the folder containing the .rsf and .tga files…");
    m_inputEdit->setReadOnly(true);
    form->addWidget(m_inputEdit, 0, 1);
    auto* browseInput = new QPushButton("Browse…", this);
    browseInput->setFixedWidth(90);
    form->addWidget(browseInput, 0, 2);

    // ── Output folder row ──────────────────────────────────────────────────
    form->addWidget(new QLabel("Output Folder:", this), 1, 0);
    m_outputEdit = new QLineEdit(this);
    m_outputEdit->setPlaceholderText("Leave blank to write outputs into the asset folder");
    form->addWidget(m_outputEdit, 1, 1);
    auto* browseOutput = new QPushButton("Browse…", this);
    browseOutput->setFixedWidth(90);
    form->addWidget(browseOutput, 1, 2);

    root->addLayout(form);

    // ── Requirement note ───────────────────────────────────────────────────
    auto* note = new QLabel(
        "<i>Note: source textures must be <tt>.tga</tt> files. "
        "If <tt>.dds</tt> files are present in the folder, the build will be refused — "
        "delete them and re-run from the original TGA sources.</i>",
        this);
    note->setWordWrap(true);
    root->addWidget(note);

    // ── Progress bar ───────────────────────────────────────────────────────
    m_progress = new QProgressBar(this);
    m_progress->setRange(0, 0); // indeterminate
    m_progress->setVisible(false);
    root->addWidget(m_progress);

    // ── Log view ───────────────────────────────────────────────────────────
    m_logView = new QPlainTextEdit(this);
    m_logView->setReadOnly(true);
    m_logView->setPlaceholderText("Build log will appear here…");
    {
        QFont mono("Courier New");
        mono.setStyleHint(QFont::Monospace);
        mono.setPointSize(9);
        m_logView->setFont(mono);
    }
    root->addWidget(m_logView, 1);

    // ── Status label ───────────────────────────────────────────────────────
    m_statusLabel = new QLabel(this);
    m_statusLabel->setWordWrap(true);
    root->addWidget(m_statusLabel);

    // ── Button row ─────────────────────────────────────────────────────────
    auto* btns = new QHBoxLayout();
    m_buildButton = new QPushButton("Build AST", this);
    m_buildButton->setDefault(true);
    m_buildButton->setEnabled(false);
    btns->addStretch(1);
    btns->addWidget(m_buildButton);
    m_buttonBox = new QDialogButtonBox(QDialogButtonBox::Close, this);
    btns->addWidget(m_buttonBox);
    root->addLayout(btns);

    // ── Connections ────────────────────────────────────────────────────────
    connect(browseInput,   &QPushButton::clicked, this, &CreateRsfAstDialog::onBrowseInput);
    connect(browseOutput,  &QPushButton::clicked, this, &CreateRsfAstDialog::onBrowseOutput);
    connect(m_buildButton, &QPushButton::clicked, this, &CreateRsfAstDialog::onBuild);
    connect(m_buttonBox,   &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_inputEdit,   &QLineEdit::textChanged, this, [this](const QString& t) {
        m_buildButton->setEnabled(!t.trimmed().isEmpty());
    });
}


// ── Slot implementations ─────────────────────────────────────────────────────

void CreateRsfAstDialog::onBrowseInput() {
    const auto dir = QFileDialog::getExistingDirectory(
        this,
        "Select RSF/TGA Asset Folder",
        m_inputEdit->text().isEmpty() ? QDir::homePath() : m_inputEdit->text());
    if (!dir.isEmpty()) {
        m_inputEdit->setText(dir);
        if (m_outputEdit->text().isEmpty())
            m_outputEdit->setPlaceholderText("Output to: " + dir);
    }
}

void CreateRsfAstDialog::onBrowseOutput() {
    const auto def = m_outputEdit->text().isEmpty()
                   ? (m_inputEdit->text().isEmpty() ? QDir::homePath() : m_inputEdit->text())
                   : m_outputEdit->text();
    const auto dir = QFileDialog::getExistingDirectory(this, "Select Output Folder", def);
    if (!dir.isEmpty()) m_outputEdit->setText(dir);
}

void CreateRsfAstDialog::onBuild() {
    const QString input_str = m_inputEdit->text().trimmed();
    if (input_str.isEmpty()) {
        QMessageBox::warning(this, "No Input Folder",
                             "Please select the RSF/TGA asset folder first.");
        return;
    }

    // Pre-validate before spinning up the worker thread
    const fs::path input_folder = input_str.toStdString();
    const auto vr = validate_input_folder(input_folder);
    if (!vr.ok) {
        showValidationErrors(vr.errors, vr.warnings);
        return;
    }
    if (!vr.warnings.empty()) {
        showValidationErrors({}, vr.warnings);
    }

    setUiEnabled(false);
    m_logView->clear();
    m_progress->setVisible(true);
    m_statusLabel->setText("Building…");

    RsfAstBuildRequest req;
    req.input_folder  = input_folder;
    req.output_folder = m_outputEdit->text().trimmed().isEmpty()
                      ? fs::path{}
                      : fs::path(m_outputEdit->text().trimmed().toStdString());

    // Run pipeline on a worker thread via QtConcurrent
    auto* watcher = new QFutureWatcher<RsfAstBuildResult>(this);
    connect(watcher, &QFutureWatcher<RsfAstBuildResult>::finished, this, [this, watcher]() {
        const auto result = watcher->result();
        watcher->deleteLater();

        // Forward all log lines to the GUI log
        for (const auto& line : result.log)
            appendLog(QString::fromStdString(line));

        // Also forward to spdlog
        if (auto lg = gf::core::Log::get()) {
            for (const auto& line : result.log)
                lg->info("[RsfAstBuild] {}", line);
        }

        const bool ok = result.success;
        QString summary;
        if (ok) {
            summary = QString("✓ Build succeeded.\n  Xbox: %1\n  PS3:  %2")
                      .arg(QString::fromStdString(result.xbox_output.string()))
                      .arg(QString::fromStdString(result.ps3_output.string()));
        } else {
            summary = "✗ Build failed. See log for details.";
            if (!result.errors.empty())
                summary += "\n\nErrors:\n  • " +
                            QStringList(
                                [&]() {
                                    QStringList sl;
                                    for (const auto& e : result.errors)
                                        sl << QString::fromStdString(e);
                                    return sl;
                                }()
                            ).join("\n  • ");
        }
        onBuildFinished(ok, summary);
    });

    watcher->setFuture(QtConcurrent::run(run_pipeline, std::move(req)));
}

void CreateRsfAstDialog::onBuildFinished(bool success, const QString& summary) {
    setUiEnabled(true);
    m_progress->setVisible(false);

    if (success) {
        m_statusLabel->setStyleSheet("color: green; font-weight: bold;");
        m_statusLabel->setText(summary);
        QMessageBox::information(this, "Build Complete",
                                 summary + "\n\nThe output files are ready in the selected output folder.");
    } else {
        m_statusLabel->setStyleSheet("color: red;");
        m_statusLabel->setText("Build failed. See log for details.");
        QMessageBox::critical(this, "Build Failed", summary);
    }
}


// ── Private helpers ──────────────────────────────────────────────────────────

void CreateRsfAstDialog::appendLog(const QString& line) {
    m_logView->appendPlainText(line);
    auto* sb = m_logView->verticalScrollBar();
    if (sb) sb->setValue(sb->maximum());
}

void CreateRsfAstDialog::setUiEnabled(bool enabled) {
    m_buildButton->setEnabled(enabled && !m_inputEdit->text().isEmpty());
    m_inputEdit->setEnabled(enabled);
    m_outputEdit->setEnabled(enabled);
}

void CreateRsfAstDialog::showValidationErrors(const std::vector<std::string>& errors,
                                               const std::vector<std::string>& warnings) {
    QString msg;
    if (!errors.empty()) {
        msg += "<b>The selected folder cannot be used:</b><ul>";
        for (const auto& e : errors)
            msg += "<li>" + QString::fromStdString(e).toHtmlEscaped() + "</li>";
        msg += "</ul>";
    }
    if (!warnings.empty()) {
        msg += "<b>Warnings:</b><ul>";
        for (const auto& w : warnings)
            msg += "<li>" + QString::fromStdString(w).toHtmlEscaped() + "</li>";
        msg += "</ul>";
    }

    if (errors.empty()) {
        QMessageBox::warning(this, "Validation Warnings", msg);
    } else {
        QMessageBox mb(QMessageBox::Critical, "Cannot Build — Validation Errors",
                       msg, QMessageBox::Ok, this);
        mb.setTextFormat(Qt::RichText);
        mb.exec();
    }
}

} // namespace gf::gui
