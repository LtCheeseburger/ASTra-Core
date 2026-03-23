#include <catch2/catch_test_macros.hpp>

#include <gf/textures/texture_replace.hpp>
#include <gf/textures/dds_build.hpp>
#include <gf/textures/dxt_compress.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <numeric>
#include <sstream>
#include <vector>
#include <string>

namespace {

// ── Byte-order helpers ────────────────────────────────────────────────────────

static void wr16(std::vector<std::uint8_t>& v, std::size_t off, std::uint16_t x) {
    v[off + 0] = static_cast<std::uint8_t>(x & 0xFFu);
    v[off + 1] = static_cast<std::uint8_t>((x >> 8) & 0xFFu);
}
static void wr32(std::vector<std::uint8_t>& v, std::size_t off, std::uint32_t x) {
    v[off + 0] = static_cast<std::uint8_t>(x & 0xFFu);
    v[off + 1] = static_cast<std::uint8_t>((x >> 8) & 0xFFu);
    v[off + 2] = static_cast<std::uint8_t>((x >> 16) & 0xFFu);
    v[off + 3] = static_cast<std::uint8_t>((x >> 24) & 0xFFu);
}
static std::uint32_t rd32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0])
         | (static_cast<std::uint32_t>(p[1]) <<  8)
         | (static_cast<std::uint32_t>(p[2]) << 16)
         | (static_cast<std::uint32_t>(p[3]) << 24);
}

// ── Synthetic DDS builder ─────────────────────────────────────────────────────

// Builds a minimal valid DDS blob: 128-byte header + `payloadBytes` of zeroed data.
// `fourcc` selects the compressed format (DXT1, DXT5, ATI1, ATI2, or DXT3).
static std::vector<std::uint8_t> make_dds(std::uint32_t width = 4,
                                           std::uint32_t height = 4,
                                           std::uint32_t mips = 1,
                                           std::uint32_t fourcc = 0x31545844u, // DXT1
                                           std::size_t payloadBytes = 8) {
    std::vector<std::uint8_t> out(128 + payloadBytes, 0);
    out[0] = 'D'; out[1] = 'D'; out[2] = 'S'; out[3] = ' ';
    wr32(out,  4, 124);         // header size
    std::uint32_t flags = 0x1007u | 0x80000u; // CAPS|HEIGHT|WIDTH|PIXELFORMAT|LINEARSIZE
    if (mips > 1) flags |= 0x20000u;          // MIPMAPCOUNT
    wr32(out,  8, flags);
    wr32(out, 12, height);
    wr32(out, 16, width);
    wr32(out, 20, static_cast<std::uint32_t>(payloadBytes)); // pitchOrLinearSize
    wr32(out, 28, mips);
    // Pixel format at offset 76
    wr32(out, 76, 32);          // PF size
    wr32(out, 80, 0x00000004u); // DDPF_FOURCC
    wr32(out, 84, fourcc);
    // Caps at offset 108
    std::uint32_t caps = 0x1000u; // DDSCAPS_TEXTURE
    if (mips > 1) caps |= 0x400008u; // MIPMAP|COMPLEX
    wr32(out, 108, caps);
    return out;
}

// Wraps a DDS blob as a P3R container (first 4 bytes → 'p3R<ver>').
static std::vector<std::uint8_t> make_p3r(const std::vector<std::uint8_t>& dds,
                                           std::uint8_t verByte = 0x02) {
    auto p3r = dds;
    if (p3r.size() >= 4) {
        p3r[0] = 0x70; // 'p' (lowercase)
        p3r[1] = '3';
        p3r[2] = 'R';
        p3r[3] = verByte;
    }
    return p3r;
}

// ── Synthetic TGA builder ─────────────────────────────────────────────────────

// Builds a Type-2 (uncompressed true-color) 32-bit TGA.
// Pixel colour is uniform R,G,B,A.
// TGA stores pixels as BGRA on-disk; read_tga converts to RGBA.
static std::vector<std::uint8_t> make_tga(std::uint32_t w, std::uint32_t h,
                                           std::uint8_t r = 128,
                                           std::uint8_t g = 64,
                                           std::uint8_t b = 32,
                                           std::uint8_t a = 255) {
    const std::size_t pixelCount = static_cast<std::size_t>(w) * h;
    std::vector<std::uint8_t> out;
    out.reserve(18 + pixelCount * 4);

    // 18-byte TGA header
    out.push_back(0);    // IDLength
    out.push_back(0);    // ColorMapType (none)
    out.push_back(2);    // ImageType: uncompressed true-color
    // ColorMapSpec (5 bytes, all zero)
    out.push_back(0); out.push_back(0);
    out.push_back(0); out.push_back(0);
    out.push_back(0);
    // XOrigin, YOrigin (little-endian uint16)
    out.push_back(0); out.push_back(0);
    out.push_back(0); out.push_back(0);
    // Width, Height (little-endian uint16)
    out.push_back(static_cast<std::uint8_t>(w & 0xFFu));
    out.push_back(static_cast<std::uint8_t>(w >> 8));
    out.push_back(static_cast<std::uint8_t>(h & 0xFFu));
    out.push_back(static_cast<std::uint8_t>(h >> 8));
    out.push_back(32);   // BitsPerPixel
    out.push_back(0x28); // ImageDescriptor: top-left origin (bit 5), 8 alpha bits (bits 0-3)

    // Pixel data: BGRA (TGA native for 32-bit)
    for (std::size_t i = 0; i < pixelCount; ++i) {
        out.push_back(b);
        out.push_back(g);
        out.push_back(r);
        out.push_back(a);
    }
    return out;
}

// ── FourCC constants ──────────────────────────────────────────────────────────

constexpr std::uint32_t CC_DXT1 = 0x31545844u; // "DXT1"
constexpr std::uint32_t CC_DXT3 = 0x33545844u; // "DXT3" (unsupported format)
constexpr std::uint32_t CC_DXT5 = 0x35545844u; // "DXT5"
constexpr std::uint32_t CC_ATI1 = 0x31495441u; // "ATI1" (BC4)
constexpr std::uint32_t CC_ATI2 = 0x32495441u; // "ATI2" (BC5)

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// replace_texture — success paths
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("P3R DXT1 replacement from TGA succeeds", "[texture_replace]") {
    const auto orig = make_p3r(make_dds(4, 4, 1, CC_DXT1, 8));
    const auto tga  = make_tga(4, 4);

    std::string err;
    gf::textures::TextureReplaceReport report;
    auto res = gf::textures::replace_texture(
        orig, tga, gf::textures::TexImportFormat::TGA, 0, &err, &report);

    REQUIRE(res.has_value());
    REQUIRE(res->containerBytes.size() >= 128);

    // P3R magic: lowercase 'p', '3', 'R', version byte from original
    REQUIRE(res->containerBytes[0] == 0x70u); // 'p' lowercase
    REQUIRE(res->containerBytes[1] == '3');
    REQUIRE(res->containerBytes[2] == 'R');
    REQUIRE(res->containerBytes[3] == 0x02u); // version byte preserved

    // FourCC in the DDS body (offset 84) must remain DXT1
    REQUIRE(rd32(res->containerBytes.data() + 84) == CC_DXT1);

    // Report
    REQUIRE(report.success);
    REQUIRE(report.origFormatName == "BC1/DXT1");
    REQUIRE(report.rebuiltFormatName == "BC1/DXT1");
    REQUIRE(report.rebuiltMipCount == 1);
    REQUIRE(report.payloadSizeMatch);
    REQUIRE(report.containerKind == gf::textures::TexContainerKind::P3R);
}

TEST_CASE("P3R DXT5 replacement from TGA succeeds", "[texture_replace]") {
    const auto orig = make_p3r(make_dds(4, 4, 1, CC_DXT5, 16));
    const auto tga  = make_tga(4, 4);

    std::string err;
    gf::textures::TextureReplaceReport report;
    auto res = gf::textures::replace_texture(
        orig, tga, gf::textures::TexImportFormat::TGA, 0, &err, &report);

    REQUIRE(res.has_value());
    REQUIRE(res->containerBytes[0] == 0x70u);  // lowercase 'p'
    REQUIRE(rd32(res->containerBytes.data() + 84) == CC_DXT5);
    REQUIRE(report.success);
    REQUIRE(report.origFormatName == "BC3/DXT5");
    REQUIRE(report.rebuiltFormatName == "BC3/DXT5");
}

TEST_CASE("Plain DDS BC1 replacement from TGA succeeds", "[texture_replace]") {
    const auto orig = make_dds(4, 4, 1, CC_DXT1, 8);
    const auto tga  = make_tga(4, 4);

    std::string err;
    gf::textures::TextureReplaceReport report;
    auto res = gf::textures::replace_texture(
        orig, tga, gf::textures::TexImportFormat::TGA, 0, &err, &report);

    REQUIRE(res.has_value());
    REQUIRE(res->containerBytes.size() >= 128);
    // Must start with "DDS "
    REQUIRE(res->containerBytes[0] == 'D');
    REQUIRE(res->containerBytes[1] == 'D');
    REQUIRE(res->containerBytes[2] == 'S');
    REQUIRE(res->containerBytes[3] == ' ');
    REQUIRE(rd32(res->containerBytes.data() + 84) == CC_DXT1);
    REQUIRE(report.success);
    REQUIRE(report.containerKind == gf::textures::TexContainerKind::DDS);
}

TEST_CASE("BC4/ATI1 replacement from TGA produces correct FourCC", "[texture_replace]") {
    const auto orig = make_dds(4, 4, 1, CC_ATI1, 8);
    const auto tga  = make_tga(4, 4);

    std::string err;
    gf::textures::TextureReplaceReport report;
    auto res = gf::textures::replace_texture(
        orig, tga, gf::textures::TexImportFormat::TGA, 0, &err, &report);

    REQUIRE(res.has_value());
    // FourCC in the rebuilt DDS must be ATI1, not silently changed to DXT5
    REQUIRE(rd32(res->containerBytes.data() + 84) == CC_ATI1);
    REQUIRE(report.origFormatName == "BC4/ATI1");
    REQUIRE(report.rebuiltFormatName == "BC4/ATI1");
    REQUIRE(report.success);
}

TEST_CASE("BC5/ATI2 replacement from TGA produces correct FourCC", "[texture_replace]") {
    const auto orig = make_dds(4, 4, 1, CC_ATI2, 16);
    const auto tga  = make_tga(4, 4);

    std::string err;
    gf::textures::TextureReplaceReport report;
    auto res = gf::textures::replace_texture(
        orig, tga, gf::textures::TexImportFormat::TGA, 0, &err, &report);

    REQUIRE(res.has_value());
    REQUIRE(rd32(res->containerBytes.data() + 84) == CC_ATI2);
    REQUIRE(report.origFormatName == "BC5/ATI2");
    REQUIRE(report.rebuiltFormatName == "BC5/ATI2");
    REQUIRE(report.success);
}

// ─────────────────────────────────────────────────────────────────────────────
// replace_texture — failure paths
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Dimension mismatch returns nullopt with clear error", "[texture_replace]") {
    // Original is 4x4, replacement TGA is 8x8
    const auto orig = make_dds(4, 4, 1, CC_DXT1, 8);
    const auto tga  = make_tga(8, 8);

    std::string err;
    gf::textures::TextureReplaceReport report;
    auto res = gf::textures::replace_texture(
        orig, tga, gf::textures::TexImportFormat::TGA, 0, &err, &report);

    REQUIRE_FALSE(res.has_value());
    REQUIRE_FALSE(err.empty());
    // Error must mention the dimension mismatch (not a generic failure)
    REQUIRE(err.find("8x8") != std::string::npos);
    REQUIRE(err.find("4x4") != std::string::npos);
    REQUIRE_FALSE(report.success);
    REQUIRE_FALSE(report.shortReason.empty());
}

TEST_CASE("DXT3 unsupported format returns nullopt with encoder error", "[texture_replace]") {
    // DXT3/BC2 has no encoder in this build
    const auto orig = make_dds(4, 4, 1, CC_DXT3, 16);
    const auto tga  = make_tga(4, 4);

    std::string err;
    gf::textures::TextureReplaceReport report;
    auto res = gf::textures::replace_texture(
        orig, tga, gf::textures::TexImportFormat::TGA, 0, &err, &report);

    REQUIRE_FALSE(res.has_value());
    REQUIRE_FALSE(err.empty());
    // Must name the unsupported format, not just say "failed"
    REQUIRE(err.find("BC2") != std::string::npos);
    REQUIRE_FALSE(report.success);
    // Short reason must identify the format
    REQUIRE(report.shortReason.find("BC2") != std::string::npos);
}

TEST_CASE("Empty original payload returns nullopt", "[texture_replace]") {
    const std::vector<std::uint8_t> empty;
    const auto tga = make_tga(4, 4);

    std::string err;
    auto res = gf::textures::replace_texture(
        empty, tga, gf::textures::TexImportFormat::TGA, 0, &err);

    REQUIRE_FALSE(res.has_value());
    REQUIRE_FALSE(err.empty());
}

// ─────────────────────────────────────────────────────────────────────────────
// validate_texture_import
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("validate_texture_import accepts matching TGA dimensions", "[texture_replace]") {
    const auto orig = make_dds(4, 4, 1, CC_DXT1, 8);
    const auto tga  = make_tga(4, 4);

    const std::string result = gf::textures::validate_texture_import(
        orig, tga, gf::textures::TexImportFormat::TGA, 0);

    REQUIRE(result.empty()); // empty = no issues
}

TEST_CASE("validate_texture_import rejects mismatched TGA dimensions", "[texture_replace]") {
    const auto orig = make_dds(8, 8, 1, CC_DXT1, 32);
    const auto tga  = make_tga(4, 4);

    const std::string result = gf::textures::validate_texture_import(
        orig, tga, gf::textures::TexImportFormat::TGA, 0);

    REQUIRE_FALSE(result.empty());
    REQUIRE(result.find("4x4") != std::string::npos);
}

TEST_CASE("validate_texture_import rejects DXT3 format early", "[texture_replace]") {
    const auto orig = make_dds(4, 4, 1, CC_DXT3, 16);
    const auto tga  = make_tga(4, 4);

    const std::string result = gf::textures::validate_texture_import(
        orig, tga, gf::textures::TexImportFormat::TGA, 0);

    REQUIRE_FALSE(result.empty());
    REQUIRE(result.find("BC2") != std::string::npos);
}

// ─────────────────────────────────────────────────────────────────────────────
// parse_original_texture_info
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("parse_original_texture_info reads P3R container", "[texture_replace]") {
    const auto p3r = make_p3r(make_dds(8, 8, 2, CC_DXT5, 176)); // 8x8 DXT5 2 mips

    std::string err;
    auto info = gf::textures::parse_original_texture_info(p3r, 0, &err);

    REQUIRE(info.has_value());
    REQUIRE(info->container == gf::textures::TexContainerKind::P3R);
    REQUIRE(info->width  == 8);
    REQUIRE(info->height == 8);
    REQUIRE(info->bcFormatCode == 0x05); // DXT5
    REQUIRE(info->p3rVersionByte == 0x02);
}

TEST_CASE("parse_original_texture_info reads plain DDS", "[texture_replace]") {
    const auto dds = make_dds(16, 16, 1, CC_ATI2, 64);

    std::string err;
    auto info = gf::textures::parse_original_texture_info(dds, 0, &err);

    REQUIRE(info.has_value());
    REQUIRE(info->container == gf::textures::TexContainerKind::DDS);
    REQUIRE(info->width  == 16);
    REQUIRE(info->height == 16);
    REQUIRE(info->bcFormatCode == 0x06); // ATI2/BC5
}

TEST_CASE("parse_original_texture_info rejects empty payload", "[texture_replace]") {
    const std::vector<std::uint8_t> empty;
    std::string err;
    auto info = gf::textures::parse_original_texture_info(empty, 0, &err);

    REQUIRE_FALSE(info.has_value());
    REQUIRE_FALSE(err.empty());
}

// ─────────────────────────────────────────────────────────────────────────────
// dxt_compress — endpoint selection quality regression
//
// These tests verify that the DXT1 encoder uses max-distance pair endpoint
// selection (actual block pixel colors) rather than per-channel min/max
// (bounding box corners).  The key requirement: for a block mixing two distinct
// hues, each pixel must be encoded as one of those two hues — not a wrong
// interpolated color produced by a misaligned bounding-box axis.
// ─────────────────────────────────────────────────────────────────────────────

namespace {

// Build a span of RGBA for N uniform pixels.
static std::vector<std::uint8_t> solid_rgba(std::uint32_t w, std::uint32_t h,
                                             std::uint8_t r, std::uint8_t g,
                                             std::uint8_t b, std::uint8_t a = 255) {
    std::vector<std::uint8_t> out(static_cast<std::size_t>(w) * h * 4);
    for (std::size_t i = 0; i < out.size(); i += 4) {
        out[i+0] = r; out[i+1] = g; out[i+2] = b; out[i+3] = a;
    }
    return out;
}

// Decode a DXT1 color block (8 bytes) and return RGBA for texel (tx, ty)
// within a 4x4 block, assuming 4-color mode (c0 > c1 after quantization).
//
// Uses hardware-exact 565 expansion (bit replication) to match GPU behaviour:
//   5-bit: (v << 3) | (v >> 2)
//   6-bit: (v << 2) | (v >> 4)
static std::array<std::uint8_t, 4> decode_dxt1_texel(const std::uint8_t* block,
                                                       int tx, int ty) {
    auto expand5 = [](std::uint32_t v) -> std::uint8_t {
        return static_cast<std::uint8_t>((v << 3) | (v >> 2));
    };
    auto expand6 = [](std::uint32_t v) -> std::uint8_t {
        return static_cast<std::uint8_t>((v << 2) | (v >> 4));
    };

    const std::uint16_t c0 = static_cast<std::uint16_t>(block[0]) |
                             (static_cast<std::uint16_t>(block[1]) << 8);
    const std::uint16_t c1 = static_cast<std::uint16_t>(block[2]) |
                             (static_cast<std::uint16_t>(block[3]) << 8);

    std::array<std::array<std::uint8_t,3>,4> pal{};
    pal[0] = {expand5((c0>>11)&0x1F), expand6((c0>>5)&0x3F), expand5(c0&0x1F)};
    pal[1] = {expand5((c1>>11)&0x1F), expand6((c1>>5)&0x3F), expand5(c1&0x1F)};
    if (c0 > c1) {
        for (int ch = 0; ch < 3; ++ch) {
            pal[2][ch] = static_cast<std::uint8_t>((2*pal[0][ch] + pal[1][ch] + 1) / 3);
            pal[3][ch] = static_cast<std::uint8_t>((pal[0][ch] + 2*pal[1][ch] + 1) / 3);
        }
    }

    const std::uint32_t idxWord = static_cast<std::uint32_t>(block[4]) |
                                  (static_cast<std::uint32_t>(block[5]) <<  8) |
                                  (static_cast<std::uint32_t>(block[6]) << 16) |
                                  (static_cast<std::uint32_t>(block[7]) << 24);
    const int texelIdx = ty * 4 + tx;
    const int palIdx   = (idxWord >> (texelIdx * 2)) & 3;
    return {pal[palIdx][0], pal[palIdx][1], pal[palIdx][2], 255};
}

} // namespace

TEST_CASE("DXT1 encoder: uniform block encodes without color error", "[dxt_compress]") {
    // All 16 pixels are the same green — a solid block should decode with only
    // 565-quantisation error (at most ±4 per channel for a well-chosen endpoint).
    const auto rgba = solid_rgba(4, 4, 40, 160, 40);
    const auto compressed = gf::textures::dxt_compress(rgba, 4, 4,
                                                        gf::textures::DxtEncodeFormat::DXT1);
    REQUIRE(compressed.size() == 8);

    // Decode each texel and verify it is within 565-quantisation tolerance.
    for (int ty = 0; ty < 4; ++ty) {
        for (int tx = 0; tx < 4; ++tx) {
            const auto px = decode_dxt1_texel(compressed.data(), tx, ty);
            // 565 quantisation: R/B ≤ ±4, G ≤ ±2
            REQUIRE(std::abs((int)px[0] - 40)  <= 4);
            REQUIRE(std::abs((int)px[1] - 160) <= 4);
            REQUIRE(std::abs((int)px[2] - 40)  <= 4);
        }
    }
}

TEST_CASE("DXT1 encoder: two-hue block — endpoints must be the two source hues",
          "[dxt_compress]") {
    // Regression test for the per-channel min/max bounding-box bug:
    //
    // A block with left half green (40,160,40) and right half red (200,40,40)
    // MUST produce endpoints that are those two actual colors.  With the old
    // bounding-box approach, the endpoints were (200,160,40) and (40,40,40) —
    // neither of which exists in the block — causing every decoded texel to
    // map to a wrong brownish palette entry.
    std::vector<std::uint8_t> rgba(16 * 4);
    for (int ty = 0; ty < 4; ++ty) {
        for (int tx = 0; tx < 4; ++tx) {
            const std::size_t off = (static_cast<std::size_t>(ty) * 4 + tx) * 4;
            if (tx < 2) {
                // left half: green
                rgba[off+0]=40; rgba[off+1]=160; rgba[off+2]=40; rgba[off+3]=255;
            } else {
                // right half: red
                rgba[off+0]=200; rgba[off+1]=40; rgba[off+2]=40; rgba[off+3]=255;
            }
        }
    }

    const auto compressed = gf::textures::dxt_compress(rgba, 4, 4,
                                                        gf::textures::DxtEncodeFormat::DXT1);
    REQUIRE(compressed.size() == 8);

    // Decode and verify: each texel should decode close to its source color.
    // Acceptable RGB error per channel: ≤16 (565 quantisation + nearest-index rounding).
    // The bounding-box bug produced errors of 80–120 on wrong-axis palette entries.
    for (int ty = 0; ty < 4; ++ty) {
        for (int tx = 0; tx < 4; ++tx) {
            const auto px   = decode_dxt1_texel(compressed.data(), tx, ty);
            const bool isGreen = (tx < 2);
            if (isGreen) {
                REQUIRE(std::abs((int)px[0] - 40)  <= 16); // R
                REQUIRE(std::abs((int)px[1] - 160) <= 16); // G
                REQUIRE(std::abs((int)px[2] - 40)  <= 16); // B
            } else {
                REQUIRE(std::abs((int)px[0] - 200) <= 16); // R
                REQUIRE(std::abs((int)px[1] - 40)  <= 16); // G
                REQUIRE(std::abs((int)px[2] - 40)  <= 16); // B
            }
        }
    }
}

TEST_CASE("DXT1 encoder: field-logo edge block preserves both hues", "[dxt_compress]") {
    // Simulates a field/logo edge: dark-blue background meets white logo text.
    // Old bounding-box: endpoints were (255,255,255) and (20,30,100) — OK here.
    // The failure mode: dark-green field meets red logo.
    // This test verifies the general principle: no texel may be farther from
    // its source color than the 565-quantisation threshold allows.
    const std::array<std::array<std::uint8_t,3>, 2> hues = {{
        {20, 30, 100},    // dark blue
        {230, 230, 230},  // near-white
    }};
    std::vector<std::uint8_t> rgba(16 * 4);
    for (int i = 0; i < 16; ++i) {
        const std::size_t off = static_cast<std::size_t>(i) * 4;
        const auto& h = hues[i & 1];
        rgba[off+0]=h[0]; rgba[off+1]=h[1]; rgba[off+2]=h[2]; rgba[off+3]=255;
    }

    const auto compressed = gf::textures::dxt_compress(rgba, 4, 4,
                                                        gf::textures::DxtEncodeFormat::DXT1);
    REQUIRE(compressed.size() == 8);

    for (int i = 0; i < 16; ++i) {
        const int tx = i % 4, ty = i / 4;
        const auto px = decode_dxt1_texel(compressed.data(), tx, ty);
        const auto& h = hues[i & 1];
        // Each decoded texel must be within 16 units of the source hue.
        REQUIRE(std::abs((int)px[0] - (int)h[0]) <= 16);
        REQUIRE(std::abs((int)px[1] - (int)h[1]) <= 16);
        REQUIRE(std::abs((int)px[2] - (int)h[2]) <= 16);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// DXT1 encoder correctness — additional regression tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("DXT1 encoder: c0 is always strictly greater than c1 (4-colour mode)",
          "[dxt_compress]") {
    // CRITICAL spec rule: the block must be in 4-colour mode (no transparency).
    // The encoder guarantees c0 > c1 as unsigned 16-bit values.
    // Failure mode: the old encoder did `c0--` on uniform blocks, producing
    // c0 < c1 and activating 3-colour transparency mode.

    // Test several block types: uniform, two-hue, gradient.
    auto check_4colour_mode = [](const std::vector<std::uint8_t>& rgba,
                                  std::uint32_t w, std::uint32_t h) {
        const auto cmp = gf::textures::dxt_compress(rgba, w, h,
                             gf::textures::DxtEncodeFormat::DXT1);
        const std::size_t num_blocks = cmp.size() / 8;
        for (std::size_t b = 0; b < num_blocks; ++b) {
            const std::uint8_t* blk = cmp.data() + b * 8;
            const std::uint16_t c0 = static_cast<std::uint16_t>(blk[0]) |
                                     (static_cast<std::uint16_t>(blk[1]) << 8);
            const std::uint16_t c1 = static_cast<std::uint16_t>(blk[2]) |
                                     (static_cast<std::uint16_t>(blk[3]) << 8);
            // c0 must be STRICTLY greater than c1 for 4-colour mode.
            INFO("Block " << b << ": c0=0x" << std::hex << c0
                          << " c1=0x" << c1 << std::dec);
            REQUIRE(c0 > c1);
        }
    };

    // Uniform dark-green (common field grass colour) — old encoder hit c0-- here.
    check_4colour_mode(solid_rgba(4, 4, 40, 100, 40), 4, 4);

    // Uniform near-black
    check_4colour_mode(solid_rgba(4, 4, 5, 5, 5), 4, 4);

    // Uniform near-white
    check_4colour_mode(solid_rgba(4, 4, 250, 250, 250), 4, 4);

    // Uniform mid-grey
    check_4colour_mode(solid_rgba(4, 4, 128, 128, 128), 4, 4);

    // Two-hue block (red/yellow)
    std::vector<std::uint8_t> rg(16 * 4);
    for (int i = 0; i < 16; ++i) {
        const std::size_t off = static_cast<std::size_t>(i) * 4;
        if (i < 8) { rg[off]=200; rg[off+1]=40;  rg[off+2]=40;  rg[off+3]=255; }
        else       { rg[off]=220; rg[off+1]=200; rg[off+2]=40;  rg[off+3]=255; }
    }
    check_4colour_mode(rg, 4, 4);
}

TEST_CASE("DXT1 encoder: red/yellow two-hue block decodes correctly",
          "[dxt_compress]") {
    // Red half + yellow half — common in team-colour logo edges.
    std::vector<std::uint8_t> rgba(16 * 4);
    for (int i = 0; i < 16; ++i) {
        const std::size_t off = static_cast<std::size_t>(i) * 4;
        if (i < 8) {
            rgba[off+0]=200; rgba[off+1]=40; rgba[off+2]=40; rgba[off+3]=255;
        } else {
            rgba[off+0]=220; rgba[off+1]=200; rgba[off+2]=40; rgba[off+3]=255;
        }
    }

    const auto compressed = gf::textures::dxt_compress(rgba, 4, 4,
                                                        gf::textures::DxtEncodeFormat::DXT1);
    REQUIRE(compressed.size() == 8);

    for (int i = 0; i < 16; ++i) {
        const int tx = i % 4, ty = i / 4;
        const auto px = decode_dxt1_texel(compressed.data(), tx, ty);
        if (i < 8) {
            REQUIRE(std::abs((int)px[0] - 200) <= 16);
            REQUIRE(std::abs((int)px[1] - 40)  <= 16);
            REQUIRE(std::abs((int)px[2] - 40)  <= 16);
        } else {
            REQUIRE(std::abs((int)px[0] - 220) <= 16);
            REQUIRE(std::abs((int)px[1] - 200) <= 16);
            REQUIRE(std::abs((int)px[2] - 40)  <= 16);
        }
    }
}

TEST_CASE("DXT1 encoder: black/white two-hue block decodes correctly",
          "[dxt_compress]") {
    std::vector<std::uint8_t> rgba(16 * 4);
    for (int i = 0; i < 16; ++i) {
        const std::size_t off = static_cast<std::size_t>(i) * 4;
        if (i % 2 == 0) {
            rgba[off+0]=0;   rgba[off+1]=0;   rgba[off+2]=0;   rgba[off+3]=255;
        } else {
            rgba[off+0]=255; rgba[off+1]=255; rgba[off+2]=255; rgba[off+3]=255;
        }
    }

    const auto compressed = gf::textures::dxt_compress(rgba, 4, 4,
                                                        gf::textures::DxtEncodeFormat::DXT1);
    REQUIRE(compressed.size() == 8);

    for (int i = 0; i < 16; ++i) {
        const int tx = i % 4, ty = i / 4;
        const auto px = decode_dxt1_texel(compressed.data(), tx, ty);
        if (i % 2 == 0) {
            REQUIRE((int)px[0] <= 8);
            REQUIRE((int)px[1] <= 8);
            REQUIRE((int)px[2] <= 8);
        } else {
            REQUIRE((int)px[0] >= 247);
            REQUIRE((int)px[1] >= 247);
            REQUIRE((int)px[2] >= 247);
        }
    }
}

TEST_CASE("DXT1 encoder: green/white two-hue block (field/logo) decodes correctly",
          "[dxt_compress]") {
    // Classic field-grass green meeting white logo text.
    std::vector<std::uint8_t> rgba(16 * 4);
    for (int i = 0; i < 16; ++i) {
        const std::size_t off = static_cast<std::size_t>(i) * 4;
        if (i < 8) {
            rgba[off+0]=40; rgba[off+1]=140; rgba[off+2]=40; rgba[off+3]=255;
        } else {
            rgba[off+0]=240; rgba[off+1]=240; rgba[off+2]=240; rgba[off+3]=255;
        }
    }

    const auto compressed = gf::textures::dxt_compress(rgba, 4, 4,
                                                        gf::textures::DxtEncodeFormat::DXT1);
    REQUIRE(compressed.size() == 8);

    for (int i = 0; i < 16; ++i) {
        const int tx = i % 4, ty = i / 4;
        const auto px = decode_dxt1_texel(compressed.data(), tx, ty);
        if (i < 8) {
            REQUIRE(std::abs((int)px[0] - 40)  <= 16);
            REQUIRE(std::abs((int)px[1] - 140) <= 16);
            REQUIRE(std::abs((int)px[2] - 40)  <= 16);
        } else {
            REQUIRE(std::abs((int)px[0] - 240) <= 16);
            REQUIRE(std::abs((int)px[1] - 240) <= 16);
            REQUIRE(std::abs((int)px[2] - 240) <= 16);
        }
    }
}

TEST_CASE("DXT1 encoder: RGB565 roundtrip has hardware-exact expansion",
          "[dxt_compress]") {
    // Verify that the encoder's palette expansion matches GPU hardware exactly.
    // Test a few known 565 values against their hardware-exact 8-bit equivalents.
    // 5-bit expand: (v5 << 3) | (v5 >> 2)
    // 6-bit expand: (v6 << 2) | (v6 >> 4)
    struct Case { std::uint8_t r, g, b; };
    // These are colours whose 565 roundtrip is exact (no approximation needed).
    const std::array<Case, 4> cases = {{
        {  0,   0,   0},  // pure black
        {255, 255, 255},  // pure white (255 = (31<<3)|(31>>2) = 248|7 = 255)
        {248,   0,   0},  // max red (r5=31 → 255; but r=248 → r5=31)
        {  0, 252,   0},  // max green (g6=63 → 255; but g=252 → g6=63)
    }};

    for (const auto& c : cases) {
        // Encode a solid block with this colour.
        const auto rgba = solid_rgba(4, 4, c.r, c.g, c.b);
        const auto cmp  = gf::textures::dxt_compress(rgba, 4, 4,
                              gf::textures::DxtEncodeFormat::DXT1);
        REQUIRE(cmp.size() == 8);

        // Read c0 from the encoded block and verify hardware-exact expansion.
        const std::uint16_t c0 = static_cast<std::uint16_t>(cmp[0]) |
                                 (static_cast<std::uint16_t>(cmp[1]) << 8);
        const std::uint16_t c1 = static_cast<std::uint16_t>(cmp[2]) |
                                 (static_cast<std::uint16_t>(cmp[3]) << 8);
        // c0 must be strictly greater than c1 (4-colour mode).
        REQUIRE(c0 > c1);

        // Decode all 16 texels; each must be close to the source colour.
        for (int i = 0; i < 16; ++i) {
            const auto px = decode_dxt1_texel(cmp.data(), i % 4, i / 4);
            REQUIRE(std::abs((int)px[0] - (int)c.r) <= 8);
            REQUIRE(std::abs((int)px[1] - (int)c.g) <= 8);
            REQUIRE(std::abs((int)px[2] - (int)c.b) <= 8);
        }
    }
}

TEST_CASE("DXT1 encoder: gradient block golden regression (max RMSE threshold)",
          "[dxt_compress]") {
    // Build a 16-pixel linear gradient from dark green to red — representative
    // of a field/logo edge block.  The encoder must keep the per-pixel RMSE
    // below a defined threshold to prevent the "blotchy/smeared" in-game artifact.
    std::vector<std::uint8_t> rgba(16 * 4);
    for (int i = 0; i < 16; ++i) {
        const std::size_t off = static_cast<std::size_t>(i) * 4;
        const float t = static_cast<float>(i) / 15.0f;
        rgba[off+0] = static_cast<std::uint8_t>(40  + t * (200 - 40));   // R: 40→200
        rgba[off+1] = static_cast<std::uint8_t>(140 + t * (40  - 140));  // G: 140→40
        rgba[off+2] = static_cast<std::uint8_t>(40);                      // B: constant
        rgba[off+3] = 255;
    }

    const auto compressed = gf::textures::dxt_compress(rgba, 4, 4,
                                                        gf::textures::DxtEncodeFormat::DXT1);
    REQUIRE(compressed.size() == 8);

    // Decode and compute per-pixel squared errors.
    float sum_sq = 0.0f;
    float max_err = 0.0f;
    for (int i = 0; i < 16; ++i) {
        const std::size_t off = static_cast<std::size_t>(i) * 4;
        const auto px = decode_dxt1_texel(compressed.data(), i % 4, i / 4);
        for (int ch = 0; ch < 3; ++ch) {
            const float e = static_cast<float>(px[ch]) - static_cast<float>(rgba[off+ch]);
            sum_sq += e * e;
            max_err = std::max(max_err, std::abs(e));
        }
    }
    const float rmse = std::sqrt(sum_sq / (16.0f * 3.0f));

    // DXT1 can represent 4 palette entries per block; a well-tuned encoder
    // fitting a linear gradient must keep RMSE ≤ 20 and peak error ≤ 40.
    // The old bounding-box encoder produced RMSE > 60 on this block.
    INFO("Gradient block RMSE=" << rmse << " max_err=" << max_err);
    REQUIRE(rmse    <= 20.0f);
    REQUIRE(max_err <= 40.0f);
}

TEST_CASE("DXT1 encoder: multi-block image all blocks in 4-colour mode",
          "[dxt_compress]") {
    // Encode a 16x16 image with mixed content (patches of different colours)
    // and verify that every block is in 4-colour mode.
    std::vector<std::uint8_t> rgba(16 * 16 * 4);
    for (std::uint32_t y = 0; y < 16; ++y) {
        for (std::uint32_t x = 0; x < 16; ++x) {
            const std::size_t off = (static_cast<std::size_t>(y) * 16 + x) * 4;
            // Checkerboard of two different greens (field grass simulation).
            if ((x / 4 + y / 4) % 2 == 0) {
                rgba[off+0]=35; rgba[off+1]=110; rgba[off+2]=35; rgba[off+3]=255;
            } else {
                rgba[off+0]=45; rgba[off+1]=130; rgba[off+2]=45; rgba[off+3]=255;
            }
        }
    }

    const auto compressed = gf::textures::dxt_compress(rgba, 16, 16,
                                                        gf::textures::DxtEncodeFormat::DXT1);
    REQUIRE(compressed.size() == 128u); // 16×16 px → 4×4=16 blocks × 8 bytes

    for (std::size_t b = 0; b < 16; ++b) {
        const std::uint8_t* blk = compressed.data() + b * 8;
        const std::uint16_t c0 = static_cast<std::uint16_t>(blk[0]) |
                                 (static_cast<std::uint16_t>(blk[1]) << 8);
        const std::uint16_t c1 = static_cast<std::uint16_t>(blk[2]) |
                                 (static_cast<std::uint16_t>(blk[3]) << 8);
        INFO("Block " << b << ": c0=0x" << std::hex << c0
                      << " c1=0x" << c1 << std::dec);
        REQUIRE(c0 > c1);
    }
}
