#include <gf/textures/dxt_compress.hpp>

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstring>
#include <limits>

// ---------------------------------------------------------------------------
// Minimal but correct DXT1 / DXT5 encoder.
//
// Quality goal: "usable modding output" — not photographic quality but good
// enough that game textures look correct in-engine. We use a range-fit
// approach which is fast and produces acceptable block quality without
// requiring heavy external libraries.
//
// References:
//   - "Real-Time DXT Compression" by J.M.P. van Waveren & Ignacio Castano
//   - BC1/BC3 format specification (DirectX docs)
// ---------------------------------------------------------------------------

namespace gf::textures {

namespace {

// ── Helpers ─────────────────────────────────────────────────────────────────

static std::uint16_t rgb_to_565(std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    const std::uint16_t r5 = static_cast<std::uint16_t>((r >> 3) & 0x1F);
    const std::uint16_t g6 = static_cast<std::uint16_t>((g >> 2) & 0x3F);
    const std::uint16_t b5 = static_cast<std::uint16_t>((b >> 3) & 0x1F);
    return static_cast<std::uint16_t>((r5 << 11) | (g6 << 5) | b5);
}

static void write_u16le(std::uint8_t* p, std::uint16_t v) {
    p[0] = static_cast<std::uint8_t>(v & 0xFF);
    p[1] = static_cast<std::uint8_t>(v >> 8);
}
static void write_u32le(std::uint8_t* p, std::uint32_t v) {
    p[0] = static_cast<std::uint8_t>(v & 0xFF);
    p[1] = static_cast<std::uint8_t>((v >> 8) & 0xFF);
    p[2] = static_cast<std::uint8_t>((v >> 16) & 0xFF);
    p[3] = static_cast<std::uint8_t>((v >> 24) & 0xFF);
}

// ── 4×4 block extraction ────────────────────────────────────────────────────

struct Block16 {
    // 16 texels, each RGBA (4 bytes)
    std::array<std::uint8_t, 16 * 4> px{};

    void load(const std::uint8_t* rgba, std::uint32_t x, std::uint32_t y,
              std::uint32_t img_w, std::uint32_t img_h) {
        for (int ty = 0; ty < 4; ++ty) {
            for (int tx = 0; tx < 4; ++tx) {
                const std::uint32_t sx = std::min(x + tx, img_w - 1);
                const std::uint32_t sy = std::min(y + ty, img_h - 1);
                const std::size_t src_idx = (static_cast<std::size_t>(sy) * img_w + sx) * 4;
                const std::size_t dst_idx = (static_cast<std::size_t>(ty) * 4 + tx) * 4;
                std::memcpy(px.data() + dst_idx, rgba + src_idx, 4);
            }
        }
    }
};

// ── Range-fit colour endpoint selection ─────────────────────────────────────

struct Endpoints {
    std::uint8_t minR, minG, minB;
    std::uint8_t maxR, maxG, maxB;
};

static Endpoints find_endpoints(const Block16& blk) {
    Endpoints ep{255, 255, 255, 0, 0, 0};
    for (int i = 0; i < 16; ++i) {
        const auto* p = blk.px.data() + i * 4;
        ep.minR = std::min(ep.minR, p[0]);
        ep.minG = std::min(ep.minG, p[1]);
        ep.minB = std::min(ep.minB, p[2]);
        ep.maxR = std::max(ep.maxR, p[0]);
        ep.maxG = std::max(ep.maxG, p[1]);
        ep.maxB = std::max(ep.maxB, p[2]);
    }
    return ep;
}

// ── DXT1 colour block encoding ───────────────────────────────────────────────
// Writes 8 bytes: [c0_lo][c0_hi][c1_lo][c1_hi][idx×4 bytes].

static void encode_color_block_dxt1(const Block16& blk, std::uint8_t* out) {
    const Endpoints ep = find_endpoints(blk);

    std::uint16_t c0 = rgb_to_565(ep.maxR, ep.maxG, ep.maxB);
    std::uint16_t c1 = rgb_to_565(ep.minR, ep.minG, ep.minB);

    // Ensure c0 >= c1 (4-colour mode, no punch-through)
    if (c0 < c1) std::swap(c0, c1);
    // If identical, bump c0 slightly to avoid degenerate block
    if (c0 == c1 && c0 > 0) c0--;

    write_u16le(out + 0, c0);
    write_u16le(out + 2, c1);

    // Reconstruct interpolated palette
    // c0 = max endpoint (in 565), c1 = min endpoint
    // interp: t = 0 → c0, t = 1 → c1, t = 2 → 2/3 c0 + 1/3 c1, t = 3 → 1/3 c0 + 2/3 c1
    std::array<std::array<std::uint8_t, 3>, 4> palette{};
    // Expand c0
    {
        palette[0][0] = static_cast<std::uint8_t>(((c0 >> 11) & 0x1F) * 255 / 31);
        palette[0][1] = static_cast<std::uint8_t>(((c0 >>  5) & 0x3F) * 255 / 63);
        palette[0][2] = static_cast<std::uint8_t>(( c0        & 0x1F) * 255 / 31);
    }
    {
        palette[1][0] = static_cast<std::uint8_t>(((c1 >> 11) & 0x1F) * 255 / 31);
        palette[1][1] = static_cast<std::uint8_t>(((c1 >>  5) & 0x3F) * 255 / 63);
        palette[1][2] = static_cast<std::uint8_t>(( c1        & 0x1F) * 255 / 31);
    }
    for (int ch = 0; ch < 3; ++ch) {
        palette[2][ch] = static_cast<std::uint8_t>((2 * palette[0][ch] + palette[1][ch] + 1) / 3);
        palette[3][ch] = static_cast<std::uint8_t>((palette[0][ch] + 2 * palette[1][ch] + 1) / 3);
    }

    std::uint32_t indices = 0;
    for (int i = 15; i >= 0; --i) {
        const auto* p = blk.px.data() + i * 4;
        int best_idx = 0;
        int best_dist = std::numeric_limits<int>::max();
        for (int k = 0; k < 4; ++k) {
            const int dr = static_cast<int>(p[0]) - palette[k][0];
            const int dg = static_cast<int>(p[1]) - palette[k][1];
            const int db = static_cast<int>(p[2]) - palette[k][2];
            const int dist = dr*dr + dg*dg + db*db;
            if (dist < best_dist) { best_dist = dist; best_idx = k; }
        }
        indices = (indices << 2) | static_cast<std::uint32_t>(best_idx);
    }
    write_u32le(out + 4, indices);
}

// ── DXT5 alpha block encoding ────────────────────────────────────────────────
// Writes 8 bytes: [a0][a1][6 bytes of 3-bit indices (16 texels)].

static void encode_alpha_block_dxt5(const Block16& blk, std::uint8_t* out) {
    std::uint8_t min_a = 255, max_a = 0;
    for (int i = 0; i < 16; ++i) {
        const std::uint8_t a = blk.px[i * 4 + 3];
        min_a = std::min(min_a, a);
        max_a = std::max(max_a, a);
    }

    // DXT5 interpolated alpha: a0=max, a1=min → 8-alpha mode when a0 > a1
    const std::uint8_t a0 = max_a;
    const std::uint8_t a1 = min_a;
    out[0] = a0;
    out[1] = a1;

    // Build 8-alpha palette
    std::array<std::uint8_t, 8> apal{};
    apal[0] = a0;
    apal[1] = a1;
    if (a0 > a1) {
        for (int k = 2; k <= 7; ++k)
            apal[k] = static_cast<std::uint8_t>(((8 - k) * a0 + (k - 1) * a1 + 3) / 7);
    } else {
        for (int k = 2; k <= 5; ++k)
            apal[k] = static_cast<std::uint8_t>(((6 - k) * a0 + (k - 1) * a1 + 2) / 5);
        apal[6] = 0;
        apal[7] = 255;
    }

    // 16 indices × 3 bits = 48 bits = 6 bytes
    std::uint64_t bits = 0;
    for (int i = 15; i >= 0; --i) {
        const std::uint8_t a = blk.px[i * 4 + 3];
        int best_idx = 0;
        int best_dist = std::numeric_limits<int>::max();
        for (int k = 0; k < 8; ++k) {
            const int d = static_cast<int>(a) - static_cast<int>(apal[k]);
            const int dist = d * d;
            if (dist < best_dist) { best_dist = dist; best_idx = k; }
        }
        bits = (bits << 3) | static_cast<std::uint64_t>(best_idx);
    }
    // Pack 48 bits into 6 bytes little-endian
    out[2] = static_cast<std::uint8_t>(bits & 0xFF);
    out[3] = static_cast<std::uint8_t>((bits >>  8) & 0xFF);
    out[4] = static_cast<std::uint8_t>((bits >> 16) & 0xFF);
    out[5] = static_cast<std::uint8_t>((bits >> 24) & 0xFF);
    out[6] = static_cast<std::uint8_t>((bits >> 32) & 0xFF);
    out[7] = static_cast<std::uint8_t>((bits >> 40) & 0xFF);
}

// ── Mip downsample (box filter) ──────────────────────────────────────────────

static std::vector<std::uint8_t> downsample_rgba(const std::uint8_t* src,
                                                   std::uint32_t src_w,
                                                   std::uint32_t src_h) {
    const std::uint32_t dst_w = std::max(1u, src_w / 2);
    const std::uint32_t dst_h = std::max(1u, src_h / 2);
    std::vector<std::uint8_t> out(static_cast<std::size_t>(dst_w) * dst_h * 4);

    for (std::uint32_t y = 0; y < dst_h; ++y) {
        for (std::uint32_t x = 0; x < dst_w; ++x) {
            std::uint32_t accum[4] = {};
            int count = 0;
            for (int dy = 0; dy < 2; ++dy) {
                for (int dx = 0; dx < 2; ++dx) {
                    const std::uint32_t sx = std::min(x * 2 + dx, src_w - 1);
                    const std::uint32_t sy = std::min(y * 2 + dy, src_h - 1);
                    const auto* p = src + (static_cast<std::size_t>(sy) * src_w + sx) * 4;
                    accum[0] += p[0]; accum[1] += p[1];
                    accum[2] += p[2]; accum[3] += p[3];
                    ++count;
                }
            }
            auto* d = out.data() + (static_cast<std::size_t>(y) * dst_w + x) * 4;
            for (int ch = 0; ch < 4; ++ch)
                d[ch] = static_cast<std::uint8_t>(accum[ch] / count);
        }
    }
    return out;
}

} // namespace

// ── Public API ───────────────────────────────────────────────────────────────

std::size_t dxt_compressed_size(std::uint32_t width,
                                 std::uint32_t height,
                                 DxtEncodeFormat format) noexcept {
    const std::uint32_t bw = (width  + 3) / 4;
    const std::uint32_t bh = (height + 3) / 4;
    const std::size_t blocks = static_cast<std::size_t>(bw) * bh;
    // DXT1 = 8 bytes/block, DXT5 = 16 bytes/block
    return blocks * (format == DxtEncodeFormat::DXT1 ? 8 : 16);
}

std::vector<std::uint8_t> dxt_compress(std::span<const std::uint8_t> rgba,
                                        std::uint32_t width,
                                        std::uint32_t height,
                                        DxtEncodeFormat format) {
    if (rgba.empty() || width == 0 || height == 0)
        return {};

    const std::size_t expected = static_cast<std::size_t>(width) * height * 4;
    if (rgba.size() < expected)
        return {};

    const std::uint32_t blocks_x = (width  + 3) / 4;
    const std::uint32_t blocks_y = (height + 3) / 4;
    const std::size_t bytes_per_block = (format == DxtEncodeFormat::DXT1) ? 8 : 16;
    const std::size_t total_bytes = static_cast<std::size_t>(blocks_x) * blocks_y * bytes_per_block;

    std::vector<std::uint8_t> out(total_bytes, 0);
    std::uint8_t* dst = out.data();

    Block16 blk;
    for (std::uint32_t by = 0; by < blocks_y; ++by) {
        for (std::uint32_t bx = 0; bx < blocks_x; ++bx) {
            blk.load(rgba.data(), bx * 4, by * 4, width, height);

            if (format == DxtEncodeFormat::DXT5) {
                encode_alpha_block_dxt5(blk, dst);
                encode_color_block_dxt1(blk, dst + 8);
                dst += 16;
            } else {
                encode_color_block_dxt1(blk, dst);
                dst += 8;
            }
        }
    }
    return out;
}

} // namespace gf::textures
