#include <gf/textures/dds_build.hpp>
#include <gf/textures/dxt_compress.hpp>
#include <gf/textures/tga_reader.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <vector>

namespace gf::textures {

namespace {

// ── DDS header layout constants ──────────────────────────────────────────────
// We write a minimal but spec-conformant DDS (no DX10 extension needed for DXT1/5).

constexpr std::uint32_t DDSD_CAPS        = 0x1;
constexpr std::uint32_t DDSD_HEIGHT      = 0x2;
constexpr std::uint32_t DDSD_WIDTH       = 0x4;
constexpr std::uint32_t DDSD_PIXELFORMAT = 0x1000;
constexpr std::uint32_t DDSD_MIPMAPCOUNT = 0x20000;
constexpr std::uint32_t DDSD_LINEARSIZE  = 0x80000;

constexpr std::uint32_t DDSCAPS_COMPLEX  = 0x8;
constexpr std::uint32_t DDSCAPS_TEXTURE  = 0x1000;
constexpr std::uint32_t DDSCAPS_MIPMAP   = 0x400000;

constexpr std::uint32_t DDPF_FOURCC      = 0x4;

constexpr std::uint32_t FOURCC_DXT1      = 0x31545844u; // "DXT1"
constexpr std::uint32_t FOURCC_DXT5      = 0x35545844u; // "DXT5"

static void write_u32le(std::uint8_t* p, std::uint32_t v) {
    p[0] = static_cast<std::uint8_t>(v & 0xFF);
    p[1] = static_cast<std::uint8_t>((v >> 8)  & 0xFF);
    p[2] = static_cast<std::uint8_t>((v >> 16) & 0xFF);
    p[3] = static_cast<std::uint8_t>((v >> 24) & 0xFF);
}

// Write a standard 128-byte DDS header.
static std::vector<std::uint8_t> make_dds_header(std::uint32_t width,
                                                   std::uint32_t height,
                                                   std::uint32_t mip_count,
                                                   DdsBuildFormat fmt) {
    std::vector<std::uint8_t> hdr(128, 0);
    std::uint8_t* p = hdr.data();

    // Magic
    p[0]='D'; p[1]='D'; p[2]='S'; p[3]=' ';

    // DDSURFACEDESC2 size = 124
    write_u32le(p +  4, 124);

    // Flags
    const std::uint32_t flags = DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH |
                                DDSD_PIXELFORMAT | DDSD_MIPMAPCOUNT | DDSD_LINEARSIZE;
    write_u32le(p +  8, flags);
    write_u32le(p + 12, height);
    write_u32le(p + 16, width);

    // pitchOrLinearSize — size of the top mip level in bytes
    const std::size_t top_size = dxt_compressed_size(width, height,
        fmt == DdsBuildFormat::DXT1 ? DxtEncodeFormat::DXT1 : DxtEncodeFormat::DXT5);
    write_u32le(p + 20, static_cast<std::uint32_t>(top_size));

    // depth = 0, mipMapCount
    write_u32le(p + 24, 0);
    write_u32le(p + 28, mip_count);
    // reserved[11] at p+32..p+75 — already zero

    // DDPIXELFORMAT at offset 76
    std::uint8_t* pf = p + 76;
    write_u32le(pf +  0, 32);          // dwSize
    write_u32le(pf +  4, DDPF_FOURCC); // dwFlags
    write_u32le(pf +  8, fmt == DdsBuildFormat::DXT1 ? FOURCC_DXT1 : FOURCC_DXT5);
    // RGBBitCount, masks — 0 for compressed

    // DDSCAPS at offset 108
    std::uint8_t* caps = p + 108;
    std::uint32_t caps1 = DDSCAPS_TEXTURE;
    if (mip_count > 1) caps1 |= DDSCAPS_COMPLEX | DDSCAPS_MIPMAP;
    write_u32le(caps + 0, caps1);
    // caps2/3/4 = 0

    return hdr;
}

// Box-filter downsample of RGBA data.
static std::vector<std::uint8_t> downsample_rgba_2x(const std::uint8_t* src,
                                                      std::uint32_t src_w,
                                                      std::uint32_t src_h) {
    const std::uint32_t dst_w = std::max(1u, src_w / 2);
    const std::uint32_t dst_h = std::max(1u, src_h / 2);
    std::vector<std::uint8_t> out(static_cast<std::size_t>(dst_w) * dst_h * 4);

    for (std::uint32_t y = 0; y < dst_h; ++y) {
        for (std::uint32_t x = 0; x < dst_w; ++x) {
            std::uint32_t accum[4] = {};
            int n = 0;
            for (int dy = 0; dy < 2; ++dy) {
                for (int dx = 0; dx < 2; ++dx) {
                    const std::uint32_t sx = std::min(x * 2 + (std::uint32_t)dx, src_w - 1);
                    const std::uint32_t sy = std::min(y * 2 + (std::uint32_t)dy, src_h - 1);
                    const std::uint8_t* s = src + (static_cast<std::size_t>(sy) * src_w + sx) * 4;
                    accum[0] += s[0]; accum[1] += s[1];
                    accum[2] += s[2]; accum[3] += s[3];
                    ++n;
                }
            }
            std::uint8_t* d = out.data() + (static_cast<std::size_t>(y) * dst_w + x) * 4;
            for (int ch = 0; ch < 4; ++ch)
                d[ch] = static_cast<std::uint8_t>(accum[ch] / n);
        }
    }
    return out;
}

// Bilinear scale of RGBA image to exact target dimensions.
static std::vector<std::uint8_t> scale_rgba(const std::uint8_t* src,
                                             std::uint32_t src_w, std::uint32_t src_h,
                                             std::uint32_t dst_w, std::uint32_t dst_h) {
    std::vector<std::uint8_t> out(static_cast<std::size_t>(dst_w) * dst_h * 4);
    for (std::uint32_t dy = 0; dy < dst_h; ++dy) {
        for (std::uint32_t dx = 0; dx < dst_w; ++dx) {
            const float fx = (dst_w  > 1) ? (static_cast<float>(dx) / (dst_w  - 1)) * (src_w  - 1) : 0.f;
            const float fy = (dst_h > 1) ? (static_cast<float>(dy) / (dst_h - 1)) * (src_h - 1) : 0.f;
            const std::uint32_t x0 = static_cast<std::uint32_t>(fx);
            const std::uint32_t y0 = static_cast<std::uint32_t>(fy);
            const std::uint32_t x1 = std::min(x0 + 1, src_w - 1);
            const std::uint32_t y1 = std::min(y0 + 1, src_h - 1);
            const float tx = fx - static_cast<float>(x0);
            const float ty = fy - static_cast<float>(y0);
            const auto* p00 = src + (static_cast<std::size_t>(y0) * src_w + x0) * 4;
            const auto* p10 = src + (static_cast<std::size_t>(y0) * src_w + x1) * 4;
            const auto* p01 = src + (static_cast<std::size_t>(y1) * src_w + x0) * 4;
            const auto* p11 = src + (static_cast<std::size_t>(y1) * src_w + x1) * 4;
            std::uint8_t* d = out.data() + (static_cast<std::size_t>(dy) * dst_w + dx) * 4;
            for (int ch = 0; ch < 4; ++ch) {
                const float v = (1 - tx) * (1 - ty) * p00[ch]
                              +      tx  * (1 - ty) * p10[ch]
                              + (1 - tx) *      ty  * p01[ch]
                              +      tx  *      ty  * p11[ch];
                d[ch] = static_cast<std::uint8_t>(std::clamp(static_cast<int>(v + 0.5f), 0, 255));
            }
        }
    }
    return out;
}

} // namespace

// ── Public API ────────────────────────────────────────────────────────────────

std::vector<std::uint8_t> build_dds_from_rgba(std::span<const std::uint8_t> rgba,
                                               std::uint32_t src_w,
                                               std::uint32_t src_h,
                                               const DdsBuildParams& params,
                                               std::string* err) {
    if (err) err->clear();

    if (rgba.empty() || src_w == 0 || src_h == 0) {
        if (err) *err = "build_dds_from_rgba: empty or zero-dimension source.";
        return {};
    }
    if (rgba.size() < static_cast<std::size_t>(src_w) * src_h * 4) {
        if (err) *err = "build_dds_from_rgba: rgba span is smaller than expected for given dimensions.";
        return {};
    }

    // Determine actual target dimensions
    std::uint32_t tgt_w = (params.target_w > 0) ? params.target_w : src_w;
    std::uint32_t tgt_h = (params.target_h > 0) ? params.target_h : src_h;

    // Compute mip count: if 0, generate full chain down to 1×1
    std::uint32_t mips = params.mip_count;
    if (mips == 0) {
        std::uint32_t w = tgt_w, h = tgt_h;
        mips = 1;
        while (w > 1 || h > 1) { w = std::max(1u, w / 2); h = std::max(1u, h / 2); ++mips; }
    }
    mips = std::max(1u, mips);

    const DxtEncodeFormat enc_fmt =
        (params.format == DdsBuildFormat::DXT5) ? DxtEncodeFormat::DXT5 : DxtEncodeFormat::DXT1;

    // Scale source to target dimensions if needed
    std::vector<std::uint8_t> mip0;
    if (tgt_w != src_w || tgt_h != src_h) {
        mip0 = scale_rgba(rgba.data(), src_w, src_h, tgt_w, tgt_h);
    } else {
        mip0.assign(rgba.begin(), rgba.end());
    }

    // Build header
    auto hdr = make_dds_header(tgt_w, tgt_h, mips, params.format);

    // Encode mip chain and accumulate bytes
    std::vector<std::uint8_t> pixel_data;
    const std::uint8_t* cur_pixels = mip0.data();
    std::uint32_t cur_w = tgt_w;
    std::uint32_t cur_h = tgt_h;
    std::vector<std::uint8_t> prev_mip;

    for (std::uint32_t m = 0; m < mips; ++m) {
        const auto compressed = dxt_compress(
            std::span<const std::uint8_t>(cur_pixels, static_cast<std::size_t>(cur_w) * cur_h * 4),
            cur_w, cur_h, enc_fmt);
        if (compressed.empty()) {
            if (err) *err = "DXT compression failed at mip level " + std::to_string(m) + ".";
            return {};
        }
        pixel_data.insert(pixel_data.end(), compressed.begin(), compressed.end());

        if (m + 1 < mips) {
            prev_mip = downsample_rgba_2x(cur_pixels, cur_w, cur_h);
            cur_w = std::max(1u, cur_w / 2);
            cur_h = std::max(1u, cur_h / 2);
            cur_pixels = prev_mip.data();
        }
    }

    // Concatenate header + pixel data
    std::vector<std::uint8_t> out;
    out.reserve(hdr.size() + pixel_data.size());
    out.insert(out.end(), hdr.begin(), hdr.end());
    out.insert(out.end(), pixel_data.begin(), pixel_data.end());
    return out;
}

std::vector<std::uint8_t> build_dds_from_tga(std::span<const std::uint8_t> tga_bytes,
                                               const DdsBuildParams& params,
                                               std::string* err) {
    if (err) err->clear();
    const auto img = read_tga(tga_bytes, err);
    if (!img.has_value()) return {};
    return build_dds_from_rgba(
        std::span<const std::uint8_t>(img->rgba.data(), img->rgba.size()),
        img->width, img->height, params, err);
}

} // namespace gf::textures
