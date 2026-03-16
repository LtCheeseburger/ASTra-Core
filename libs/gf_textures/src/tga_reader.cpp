#include <gf/textures/tga_reader.hpp>

#include <algorithm>
#include <cstring>

namespace gf::textures {

namespace {

// TGA image type codes we handle
constexpr std::uint8_t TGA_TYPE_GRAY       = 3;
constexpr std::uint8_t TGA_TYPE_TRUE_COLOR = 2;
constexpr std::uint8_t TGA_TYPE_RLE_TRUE   = 10;
constexpr std::uint8_t TGA_TYPE_RLE_GRAY   = 11;

struct TgaHeader {
    std::uint8_t  idLength;
    std::uint8_t  colorMapType;
    std::uint8_t  imageType;
    // color map spec (5 bytes, ignored)
    std::uint8_t  cmapSpec[5];
    // image spec
    std::uint16_t xOrigin;
    std::uint16_t yOrigin;
    std::uint16_t width;
    std::uint16_t height;
    std::uint8_t  bitsPerPixel;
    std::uint8_t  imageDescriptor;
};

static std::uint16_t read_u16le(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(p[0] | (static_cast<std::uint16_t>(p[1]) << 8));
}

static bool parse_header(std::span<const std::uint8_t> bytes, TgaHeader& h) {
    if (bytes.size() < 18) return false;
    const auto* p = bytes.data();
    h.idLength        = p[0];
    h.colorMapType    = p[1];
    h.imageType       = p[2];
    std::memcpy(h.cmapSpec, p + 3, 5);
    h.xOrigin         = read_u16le(p + 8);
    h.yOrigin         = read_u16le(p + 10);
    h.width           = read_u16le(p + 12);
    h.height          = read_u16le(p + 14);
    h.bitsPerPixel    = p[16];
    h.imageDescriptor = p[17];
    return true;
}

// Expand a single TGA pixel (src_bpp bytes at *src) to RGBA at *dst.
// Returns true on success.
static bool expand_pixel(const std::uint8_t* src, int src_bpp, std::uint8_t* dst) {
    switch (src_bpp) {
    case 4: // BGRA
        dst[0] = src[2]; // R
        dst[1] = src[1]; // G
        dst[2] = src[0]; // B
        dst[3] = src[3]; // A
        return true;
    case 3: // BGR
        dst[0] = src[2];
        dst[1] = src[1];
        dst[2] = src[0];
        dst[3] = 0xFF;
        return true;
    case 1: // gray
        dst[0] = dst[1] = dst[2] = src[0];
        dst[3] = 0xFF;
        return true;
    default:
        return false;
    }
}

} // namespace

std::optional<TgaImage> read_tga(std::span<const std::uint8_t> bytes, std::string* err) {
    TgaHeader h{};
    if (!parse_header(bytes, h)) {
        if (err) *err = "TGA file is too small to contain a valid header (need >= 18 bytes).";
        return std::nullopt;
    }

    // Determine bytes-per-pixel from image type + bpp field
    int src_bpp = 0;
    bool is_rle = false;
    bool is_gray = false;

    switch (h.imageType) {
    case TGA_TYPE_TRUE_COLOR: src_bpp = h.bitsPerPixel / 8; break;
    case TGA_TYPE_GRAY:       src_bpp = 1; is_gray = true;  break;
    case TGA_TYPE_RLE_TRUE:   src_bpp = h.bitsPerPixel / 8; is_rle = true; break;
    case TGA_TYPE_RLE_GRAY:   src_bpp = 1; is_rle = true; is_gray = true; break;
    default:
        if (err) *err = "Unsupported TGA image type " + std::to_string(h.imageType) +
                        ". Only types 2 (true-color), 3 (gray), 10 (RLE true-color), "
                        "11 (RLE gray) are supported.";
        return std::nullopt;
    }

    if (src_bpp != 1 && src_bpp != 3 && src_bpp != 4) {
        if (err) *err = "Unsupported TGA bits-per-pixel: " + std::to_string(h.bitsPerPixel) +
                        ". Expected 8, 24, or 32.";
        return std::nullopt;
    }

    if (h.width == 0 || h.height == 0) {
        if (err) *err = "TGA reports zero dimensions (" + std::to_string(h.width) +
                        "x" + std::to_string(h.height) + ").";
        return std::nullopt;
    }

    // Skip over ID field and (ignored) color map data
    // Color map entry count * entry size
    std::size_t cmap_bytes = static_cast<std::size_t>(read_u16le(h.cmapSpec + 2)) // entry count
                           * static_cast<std::size_t>((h.cmapSpec[4] + 7) / 8);   // bytes per entry
    std::size_t data_offset = 18 + h.idLength + cmap_bytes;

    if (data_offset >= bytes.size()) {
        if (err) *err = "TGA data offset exceeds file size.";
        return std::nullopt;
    }

    const std::uint8_t* src = bytes.data() + data_offset;
    std::size_t src_remaining = bytes.size() - data_offset;

    const std::uint32_t W = h.width;
    const std::uint32_t H = h.height;
    const std::uint32_t total_pixels = W * H;

    TgaImage img;
    img.width  = W;
    img.height = H;
    img.rgba.resize(static_cast<std::size_t>(total_pixels) * 4, 0xFF);

    // Determine row order from descriptor bit 5 (0=bottom-to-top, 1=top-to-bottom)
    const bool top_to_bottom = ((h.imageDescriptor >> 5) & 1) != 0;

    if (!is_rle) {
        // Uncompressed
        const std::size_t expected = static_cast<std::size_t>(total_pixels) * src_bpp;
        if (src_remaining < expected) {
            if (err) *err = "TGA pixel data is truncated (need " + std::to_string(expected) +
                            " bytes, got " + std::to_string(src_remaining) + ").";
            return std::nullopt;
        }

        for (std::uint32_t row = 0; row < H; ++row) {
            const std::uint32_t dst_row = top_to_bottom ? row : (H - 1 - row);
            std::uint8_t* dst_ptr = img.rgba.data() + static_cast<std::size_t>(dst_row) * W * 4;
            for (std::uint32_t col = 0; col < W; ++col) {
                expand_pixel(src, src_bpp, dst_ptr + col * 4);
                src += src_bpp;
            }
        }
    } else {
        // RLE
        std::uint32_t pixels_decoded = 0;
        const std::uint8_t* src_end = bytes.data() + bytes.size();

        while (pixels_decoded < total_pixels) {
            if (src >= src_end) {
                if (err) *err = "TGA RLE stream ended prematurely.";
                return std::nullopt;
            }
            const std::uint8_t packet = *src++;
            const bool is_run = (packet & 0x80) != 0;
            const int count = (packet & 0x7F) + 1;

            if (static_cast<std::size_t>(pixels_decoded) + count > total_pixels) {
                if (err) *err = "TGA RLE packet overflows pixel count.";
                return std::nullopt;
            }

            if (is_run) {
                // Run-length packet: one pixel repeated `count` times
                if (src + src_bpp > src_end) {
                    if (err) *err = "TGA RLE run packet truncated.";
                    return std::nullopt;
                }
                std::uint8_t expanded[4];
                expand_pixel(src, src_bpp, expanded);
                src += src_bpp;

                for (int i = 0; i < count; ++i) {
                    const std::uint32_t px   = pixels_decoded + i;
                    const std::uint32_t row  = px / W;
                    const std::uint32_t col  = px % W;
                    const std::uint32_t dst_row = top_to_bottom ? row : (H - 1 - row);
                    std::uint8_t* dst_ptr = img.rgba.data()
                                          + (static_cast<std::size_t>(dst_row) * W + col) * 4;
                    std::memcpy(dst_ptr, expanded, 4);
                }
            } else {
                // Raw packet: `count` distinct pixels
                if (src + static_cast<std::size_t>(count) * src_bpp > src_end) {
                    if (err) *err = "TGA RLE raw packet truncated.";
                    return std::nullopt;
                }
                for (int i = 0; i < count; ++i) {
                    const std::uint32_t px   = pixels_decoded + i;
                    const std::uint32_t row  = px / W;
                    const std::uint32_t col  = px % W;
                    const std::uint32_t dst_row = top_to_bottom ? row : (H - 1 - row);
                    std::uint8_t* dst_ptr = img.rgba.data()
                                          + (static_cast<std::size_t>(dst_row) * W + col) * 4;
                    expand_pixel(src, src_bpp, dst_ptr);
                    src += src_bpp;
                }
            }
            pixels_decoded += count;
        }
        (void)is_gray; // suppress unused warning for gray path (handled via expand_pixel bpp=1)
    }

    return img;
}

} // namespace gf::textures
