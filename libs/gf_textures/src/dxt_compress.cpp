#include <gf/textures/dxt_compress.hpp>

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstring>
#include <limits>

// ---------------------------------------------------------------------------
// DXT1 / DXT5 / BC4 / BC5 encoder — nvdxt-equivalent quality
//
// Quality goal: match the visual output of nvdxt.exe -quality_highest as used
// by EASE (AST Editor 2.0) for NCAA Football texture replacement.
//
// EASE's pipeline:
//   TGA → nvdxt.exe -dxt1/-dxt5 -quality_highest -prescale W H -nmips N → DDS
//   DDS → DDStoP3R (magic swap only) → ZLIB compress → AST entry
//
// ASTra replicates the same pipeline inline.  The structural path is
// identical; the only remaining delta is DXT block encoding quality.
//
// DXT1 encoding algorithm (per block):
//   1. PCA: compute covariance matrix of the 16 RGB pixels, extract
//      principal axis via 8 iterations of power iteration.
//   2. Project pixels onto principal axis; use min/max projection
//      pixels as initial float endpoints.
//   3. Iterative least-squares refinement (up to 8 rounds, early-exit
//      when endpoints converge within 0.5 units).  LS solves the 2×2
//      normal equations per channel to find the endpoints that minimise
//      total squared error given the current index assignment.
//   4. Quantise both refined float endpoints to RGB565.
//   5. Enforce DXT1 4-colour mode: c0 > c1 (unsigned 16-bit).
//      Degenerate (c0 == c1): bump c0 UP.  NEVER decrement c0 — that
//      flips c0 < c1 and silently activates 3-colour transparency mode.
//   6. Expand endpoints using hardware-exact bit replication:
//        5-bit: (v<<3)|(v>>2)    6-bit: (v<<2)|(v>>4)
//      The approximation v*255/31 is off by ±1 at mid-range, causing
//      index misassignment at colour boundaries.
//   7. Nearest-neighbour index assignment against the exact palette.
//
// References:
//   - "Real-Time DXT Compression" van Waveren & Castano (2006)
//   - "High Quality DXT Compression using CUDA" (NVIDIA)
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

// Hardware-exact 565 channel expansion (matches GPU decoder).
static inline std::uint8_t expand5(std::uint32_t v5) noexcept {
    return static_cast<std::uint8_t>((v5 << 3) | (v5 >> 2));
}
static inline std::uint8_t expand6(std::uint32_t v6) noexcept {
    return static_cast<std::uint8_t>((v6 << 2) | (v6 >> 4));
}

// ── 4×4 block extraction ────────────────────────────────────────────────────

struct Block16 {
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

// ── Palette-index weights for 4-colour DXT1 mode ─────────────────────────────
//   idx 0 → ep0               (w0=1,   w1=0  )
//   idx 1 → ep1               (w0=0,   w1=1  )
//   idx 2 → (2*ep0+ep1)/3    (w0=2/3, w1=1/3)
//   idx 3 → (ep0+2*ep1)/3    (w0=1/3, w1=2/3)

static constexpr float kW0[4] = { 1.0f,       0.0f,       2.0f/3.0f, 1.0f/3.0f };
static constexpr float kW1[4] = { 0.0f,       1.0f,       1.0f/3.0f, 2.0f/3.0f };

// ── PCA endpoint selection ───────────────────────────────────────────────────
//
// Compute the principal axis of the 16 RGB pixel cloud using the 3×3
// covariance matrix and 8 rounds of power iteration.  Then project each
// pixel onto this axis and use the min/max projection pixels as endpoints.
//
// This is the same approach used by NVIDIA Texture Tools (nvdxt.exe
// -quality_highest) for initial endpoint estimation.

static void pca_endpoints(const Block16& blk,
                           float ep0[3], float ep1[3]) {
    // ── Compute mean ────────────────────────────────────────────────────────
    float mean[3] = {};
    for (int i = 0; i < 16; ++i)
        for (int c = 0; c < 3; ++c)
            mean[c] += static_cast<float>(blk.px[i * 4 + c]);
    for (int c = 0; c < 3; ++c) mean[c] *= (1.0f / 16.0f);

    // ── Compute 3×3 symmetric covariance matrix (6 unique entries) ──────────
    // Order: [0]=xx, [1]=yy, [2]=zz, [3]=xy, [4]=xz, [5]=yz
    float cov[6] = {};
    for (int i = 0; i < 16; ++i) {
        const float d[3] = {
            static_cast<float>(blk.px[i * 4 + 0]) - mean[0],
            static_cast<float>(blk.px[i * 4 + 1]) - mean[1],
            static_cast<float>(blk.px[i * 4 + 2]) - mean[2]
        };
        cov[0] += d[0] * d[0];
        cov[1] += d[1] * d[1];
        cov[2] += d[2] * d[2];
        cov[3] += d[0] * d[1];
        cov[4] += d[0] * d[2];
        cov[5] += d[1] * d[2];
    }

    // ── Power iteration: find dominant eigenvector ───────────────────────────
    // Seed with first row of the covariance matrix (or diagonal if all zero).
    float v[3] = { cov[0] + cov[3] + cov[4],
                   cov[3] + cov[1] + cov[5],
                   cov[4] + cov[5] + cov[2] };

    float len2 = v[0]*v[0] + v[1]*v[1] + v[2]*v[2];
    if (len2 < 1e-8f) {
        // Constant-colour block — any axis works; fall back to max-dist pair.
        // Find max-distance pair (robust fallback).
        int ei = 0, ej = 1;
        int bd = 0;
        for (int i = 0; i < 16; ++i) {
            const auto* a = blk.px.data() + i * 4;
            for (int j = i + 1; j < 16; ++j) {
                const auto* b = blk.px.data() + j * 4;
                const int dr = (int)a[0]-(int)b[0];
                const int dg = (int)a[1]-(int)b[1];
                const int db = (int)a[2]-(int)b[2];
                const int d  = dr*dr + dg*dg + db*db;
                if (d > bd) { bd = d; ei = i; ej = j; }
            }
        }
        const auto* p0 = blk.px.data() + ei * 4;
        const auto* p1 = blk.px.data() + ej * 4;
        ep0[0]=p0[0]; ep0[1]=p0[1]; ep0[2]=p0[2];
        ep1[0]=p1[0]; ep1[1]=p1[1]; ep1[2]=p1[2];
        return;
    }

    // Normalize and run 8 power iterations.
    {
        float len = std::sqrt(len2);
        v[0] /= len; v[1] /= len; v[2] /= len;
    }
    for (int iter = 0; iter < 8; ++iter) {
        const float w[3] = {
            cov[0]*v[0] + cov[3]*v[1] + cov[4]*v[2],
            cov[3]*v[0] + cov[1]*v[1] + cov[5]*v[2],
            cov[4]*v[0] + cov[5]*v[1] + cov[2]*v[2]
        };
        const float wl2 = w[0]*w[0] + w[1]*w[1] + w[2]*w[2];
        if (wl2 < 1e-16f) break;
        const float wl = std::sqrt(wl2);
        v[0] = w[0]/wl; v[1] = w[1]/wl; v[2] = w[2]/wl;
    }

    // ── Find pixel closest to each extreme of the principal axis ────────────
    float t_min = std::numeric_limits<float>::max();
    float t_max = -std::numeric_limits<float>::max();
    int   ep0_idx = 0, ep1_idx = 0;
    for (int i = 0; i < 16; ++i) {
        float t = 0.0f;
        for (int c = 0; c < 3; ++c)
            t += (static_cast<float>(blk.px[i * 4 + c]) - mean[c]) * v[c];
        if (t < t_min) { t_min = t; ep0_idx = i; }
        if (t > t_max) { t_max = t; ep1_idx = i; }
    }
    const auto* p0 = blk.px.data() + ep0_idx * 4;
    const auto* p1 = blk.px.data() + ep1_idx * 4;
    ep0[0]=p0[0]; ep0[1]=p0[1]; ep0[2]=p0[2];
    ep1[0]=p1[0]; ep1[1]=p1[1]; ep1[2]=p1[2];
}

// ── Axis-projection index assignment ─────────────────────────────────────────
//
// Project each pixel onto the ep0→ep1 axis, bucket by nearest palette entry:
//   t ∈ [0,   1/6) → index 0   (ep0)
//   t ∈ [1/6, 1/2) → index 2   (2/3 ep0 + 1/3 ep1)
//   t ∈ [1/2, 5/6) → index 3   (1/3 ep0 + 2/3 ep1)
//   t ∈ [5/6, 1  ] → index 1   (ep1)

static void assign_indices_axis(const Block16& blk,
                                 const float ep0[3], const float ep1[3],
                                 int indices[16]) {
    float axis[3];
    float len2 = 0.0f;
    for (int c = 0; c < 3; ++c) {
        axis[c] = ep1[c] - ep0[c];
        len2   += axis[c] * axis[c];
    }
    if (len2 < 1.0f) {
        for (int i = 0; i < 16; ++i) indices[i] = 0;
        return;
    }
    for (int i = 0; i < 16; ++i) {
        float dot = 0.0f;
        for (int c = 0; c < 3; ++c)
            dot += (static_cast<float>(blk.px[i * 4 + c]) - ep0[c]) * axis[c];
        const float t = dot / len2;
        if      (t < (1.0f / 6.0f)) indices[i] = 0;
        else if (t < (1.0f / 2.0f)) indices[i] = 2;
        else if (t < (5.0f / 6.0f)) indices[i] = 3;
        else                         indices[i] = 1;
    }
}

// ── Least-squares endpoint refinement ────────────────────────────────────────
//
// Given index assignments, solve the 2×2 normal equations per channel that
// minimise total squared error between each pixel and its palette entry.

static void refine_endpoints_ls(const Block16& blk,
                                  const int     indices[16],
                                  float         ep0[3],
                                  float         ep1[3]) {
    float a00 = 0.0f, a01 = 0.0f, a11 = 0.0f;
    float rhs0[3] = {}, rhs1[3] = {};

    for (int i = 0; i < 16; ++i) {
        const float w0 = kW0[indices[i]];
        const float w1 = kW1[indices[i]];
        a00 += w0 * w0;
        a01 += w0 * w1;
        a11 += w1 * w1;
        for (int c = 0; c < 3; ++c) {
            const float pf = static_cast<float>(blk.px[i * 4 + c]);
            rhs0[c] += w0 * pf;
            rhs1[c] += w1 * pf;
        }
    }
    const float det = a00 * a11 - a01 * a01;
    if (std::abs(det) < 1e-4f) return;
    const float inv_det = 1.0f / det;
    for (int c = 0; c < 3; ++c) {
        ep0[c] = std::clamp((a11 * rhs0[c] - a01 * rhs1[c]) * inv_det, 0.0f, 255.0f);
        ep1[c] = std::clamp((a00 * rhs1[c] - a01 * rhs0[c]) * inv_det, 0.0f, 255.0f);
    }
}

// ── Compute total squared block error against a 4-entry palette ──────────────

static float block_error(const Block16& blk, const int indices[16],
                          const std::array<std::array<std::uint8_t, 3>, 4>& pal) {
    float err = 0.0f;
    for (int i = 0; i < 16; ++i) {
        for (int c = 0; c < 3; ++c) {
            const float d = static_cast<float>(blk.px[i * 4 + c]) - static_cast<float>(pal[indices[i]][c]);
            err += d * d;
        }
    }
    return err;
}

// ── DXT1 colour block encoding ───────────────────────────────────────────────
// Writes 8 bytes: [c0_lo][c0_hi][c1_lo][c1_hi][idx×4 bytes].

static void encode_color_block_dxt1(const Block16& blk, std::uint8_t* out) {

    // ── Step 1: PCA-based initial float endpoints ─────────────────────────
    float ep0f[3], ep1f[3];
    pca_endpoints(blk, ep0f, ep1f);

    // ── Step 2: Iterative LS refinement until convergence ─────────────────
    // Each iteration: assign indices by axis projection → LS solve.
    // Up to 8 rounds; early-exit when endpoints move < 0.5 units.
    int idx16[16];
    for (int iter = 0; iter < 8; ++iter) {
        assign_indices_axis(blk, ep0f, ep1f, idx16);

        float new0[3] = { ep0f[0], ep0f[1], ep0f[2] };
        float new1[3] = { ep1f[0], ep1f[1], ep1f[2] };
        refine_endpoints_ls(blk, idx16, new0, new1);

        // Convergence check.
        float diff = 0.0f;
        for (int c = 0; c < 3; ++c) {
            diff += (new0[c] - ep0f[c]) * (new0[c] - ep0f[c]);
            diff += (new1[c] - ep1f[c]) * (new1[c] - ep1f[c]);
        }
        ep0f[0]=new0[0]; ep0f[1]=new0[1]; ep0f[2]=new0[2];
        ep1f[0]=new1[0]; ep1f[1]=new1[1]; ep1f[2]=new1[2];
        if (diff < 0.25f) break;
    }
    // Final index assignment after convergence.
    assign_indices_axis(blk, ep0f, ep1f, idx16);

    // ── Step 3: Quantise refined float endpoints to RGB565 ────────────────
    auto clamp_u8 = [](float v) -> std::uint8_t {
        return static_cast<std::uint8_t>(std::clamp(std::round(v), 0.0f, 255.0f));
    };
    std::uint16_t c0 = rgb_to_565(clamp_u8(ep0f[0]), clamp_u8(ep0f[1]), clamp_u8(ep0f[2]));
    std::uint16_t c1 = rgb_to_565(clamp_u8(ep1f[0]), clamp_u8(ep1f[1]), clamp_u8(ep1f[2]));

    // ── Step 4: Guarantee 4-colour mode (c0 strictly > c1) ────────────────
    // DXT1 spec: c0 > c1 → 4-colour interpolation.
    //            c0 ≤ c1 → 3-colour + 1 transparent entry (NEVER wanted here).
    // IMPORTANT: use INCREMENT, not decrement.  c0-- would make c0 < c1
    // and silently activate 3-colour mode, corrupting every decoded texel.
    if (c0 < c1) std::swap(c0, c1);
    if (c0 == c1) {
        if (c0 < 0xFFFFu) ++c0;
        else               --c1;
    }

    write_u16le(out + 0, c0);
    write_u16le(out + 2, c1);

    // ── Step 5: Build exact palette using hardware 565 expansion ──────────
    // Use bit-replication, NOT v*255/31 (off by ±1 at mid-range values).
    std::array<std::array<std::uint8_t, 3>, 4> palette{};
    {
        const std::uint32_t r5 = (c0 >> 11) & 0x1Fu, g6 = (c0 >> 5) & 0x3Fu, b5 = c0 & 0x1Fu;
        palette[0][0] = expand5(r5); palette[0][1] = expand6(g6); palette[0][2] = expand5(b5);
    }
    {
        const std::uint32_t r5 = (c1 >> 11) & 0x1Fu, g6 = (c1 >> 5) & 0x3Fu, b5 = c1 & 0x1Fu;
        palette[1][0] = expand5(r5); palette[1][1] = expand6(g6); palette[1][2] = expand5(b5);
    }
    for (int ch = 0; ch < 3; ++ch) {
        palette[2][ch] = static_cast<std::uint8_t>((2u * palette[0][ch] + palette[1][ch] + 1u) / 3u);
        palette[3][ch] = static_cast<std::uint8_t>((palette[0][ch] + 2u * palette[1][ch] + 1u) / 3u);
    }

    // ── Step 6: Final nearest-neighbour index assignment ──────────────────
    // Use the exact quantised palette so indices match what the GPU decodes.
    std::uint32_t packed_indices = 0;
    for (int i = 15; i >= 0; --i) {
        const auto* p = blk.px.data() + i * 4;
        int best_idx = 0, best_dist = std::numeric_limits<int>::max();
        for (int k = 0; k < 4; ++k) {
            const int dr = (int)p[0] - palette[k][0];
            const int dg = (int)p[1] - palette[k][1];
            const int db = (int)p[2] - palette[k][2];
            const int d  = dr*dr + dg*dg + db*db;
            if (d < best_dist) { best_dist = d; best_idx = k; }
        }
        packed_indices = (packed_indices << 2) | static_cast<std::uint32_t>(best_idx);
    }
    write_u32le(out + 4, packed_indices);
    (void)block_error; // suppress unused-function warning; kept for test use
}

// ── Generic single-channel block encoding (BC4 / DXT5-alpha) ─────────────────
// Writes 8 bytes: [v0][v1][6 bytes of 3-bit indices (16 texels)].

static void encode_single_channel_block(const Block16& blk, int channel,
                                         std::uint8_t* out) {
    std::uint8_t min_v = 255, max_v = 0;
    for (int i = 0; i < 16; ++i) {
        const std::uint8_t v = blk.px[i * 4 + channel];
        min_v = std::min(min_v, v);
        max_v = std::max(max_v, v);
    }

    const std::uint8_t v0 = max_v, v1 = min_v;
    out[0] = v0; out[1] = v1;

    std::array<std::uint8_t, 8> vpal{};
    vpal[0] = v0; vpal[1] = v1;
    if (v0 > v1) {
        for (int k = 2; k <= 7; ++k)
            vpal[k] = static_cast<std::uint8_t>(((8 - k) * v0 + (k - 1) * v1 + 3) / 7);
    } else {
        for (int k = 2; k <= 5; ++k)
            vpal[k] = static_cast<std::uint8_t>(((6 - k) * v0 + (k - 1) * v1 + 2) / 5);
        vpal[6] = 0; vpal[7] = 255;
    }

    std::uint64_t bits = 0;
    for (int i = 15; i >= 0; --i) {
        const std::uint8_t v = blk.px[i * 4 + channel];
        int best_idx = 0, best_dist = std::numeric_limits<int>::max();
        for (int k = 0; k < 8; ++k) {
            const int d = (int)v - (int)vpal[k];
            if (d*d < best_dist) { best_dist = d*d; best_idx = k; }
        }
        bits = (bits << 3) | static_cast<std::uint64_t>(best_idx);
    }
    out[2] = (std::uint8_t)(bits & 0xFF);
    out[3] = (std::uint8_t)((bits >>  8) & 0xFF);
    out[4] = (std::uint8_t)((bits >> 16) & 0xFF);
    out[5] = (std::uint8_t)((bits >> 24) & 0xFF);
    out[6] = (std::uint8_t)((bits >> 32) & 0xFF);
    out[7] = (std::uint8_t)((bits >> 40) & 0xFF);
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
                    const std::uint32_t sx = std::min(x*2+(std::uint32_t)dx, src_w-1);
                    const std::uint32_t sy = std::min(y*2+(std::uint32_t)dy, src_h-1);
                    const auto* p = src + (static_cast<std::size_t>(sy)*src_w+sx)*4;
                    accum[0]+=p[0]; accum[1]+=p[1]; accum[2]+=p[2]; accum[3]+=p[3];
                    ++count;
                }
            }
            auto* d = out.data() + (static_cast<std::size_t>(y)*dst_w+x)*4;
            for (int ch = 0; ch < 4; ++ch) d[ch] = (std::uint8_t)(accum[ch]/count);
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
    switch (format) {
    case DxtEncodeFormat::DXT1:
    case DxtEncodeFormat::BC4:  return blocks * 8;
    case DxtEncodeFormat::DXT5:
    case DxtEncodeFormat::BC5:  return blocks * 16;
    }
    return blocks * 8;
}

std::vector<std::uint8_t> dxt_compress(std::span<const std::uint8_t> rgba,
                                        std::uint32_t width,
                                        std::uint32_t height,
                                        DxtEncodeFormat format) {
    if (rgba.empty() || width == 0 || height == 0) return {};
    if (rgba.size() < static_cast<std::size_t>(width) * height * 4) return {};

    const std::uint32_t bx = (width  + 3) / 4;
    const std::uint32_t by = (height + 3) / 4;
    const std::size_t bpb = (format == DxtEncodeFormat::DXT1) ? 8u : 16u;
    std::vector<std::uint8_t> out(static_cast<std::size_t>(bx) * by * bpb, 0);
    std::uint8_t* dst = out.data();

    Block16 blk;
    for (std::uint32_t y = 0; y < by; ++y) {
        for (std::uint32_t x = 0; x < bx; ++x) {
            blk.load(rgba.data(), x * 4, y * 4, width, height);
            switch (format) {
            case DxtEncodeFormat::DXT5:
                encode_single_channel_block(blk, 3, dst);
                encode_color_block_dxt1(blk, dst + 8);
                dst += 16; break;
            case DxtEncodeFormat::BC4:
                encode_single_channel_block(blk, 0, dst);
                dst += 8; break;
            case DxtEncodeFormat::BC5:
                encode_single_channel_block(blk, 0, dst);
                encode_single_channel_block(blk, 1, dst + 8);
                dst += 16; break;
            default:
                encode_color_block_dxt1(blk, dst);
                dst += 8; break;
            }
        }
    }
    return out;
}

} // namespace gf::textures
