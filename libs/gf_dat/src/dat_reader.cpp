#include <gf/dat/dat_reader.hpp>

#include <cstring>
#include <fstream>

namespace gf::dat {

// ---------------------------------------------------------------------------
// Big-endian read helpers
// ---------------------------------------------------------------------------

static bool can_read(std::size_t offset, std::size_t need, std::size_t total) noexcept
{
    return offset + need <= total;
}

static std::uint32_t read_u32be(const std::uint8_t* p) noexcept
{
    return (static_cast<std::uint32_t>(p[0]) << 24)
         | (static_cast<std::uint32_t>(p[1]) << 16)
         | (static_cast<std::uint32_t>(p[2]) <<  8)
         |  static_cast<std::uint32_t>(p[3]);
}

static float read_f32be(const std::uint8_t* p) noexcept
{
    std::uint32_t u = read_u32be(p);
    float f;
    std::memcpy(&f, &u, 4);
    return f;
}

// ---------------------------------------------------------------------------
// looks_like_dat
// ---------------------------------------------------------------------------

bool looks_like_dat(const std::uint8_t* data, std::size_t size) noexcept
{
    if (size < 16) return false;

    const std::uint32_t file_len  = read_u32be(data + 0);
    const std::uint32_t num_imgs  = read_u32be(data + 4);
    const std::uint32_t zeros     = read_u32be(data + 8);
    const std::uint32_t off0      = read_u32be(data + 12);

    if (zeros != 0)                            return false;
    if (num_imgs == 0 || num_imgs >= 5000)     return false;
    if (file_len == 0)                         return false;

    const std::uint32_t min_off = 12u + num_imgs * 4u;
    if (off0 < min_off || off0 >= file_len)    return false;

    return true;
}

// ---------------------------------------------------------------------------
// parse_dat
// ---------------------------------------------------------------------------

std::optional<DatFile> parse_dat(const std::uint8_t* data,
                                  std::size_t          size,
                                  std::string*         err)
{
    auto fail = [&](const char* msg) -> std::optional<DatFile> {
        if (err) *err = msg;
        return std::nullopt;
    };

    if (size < 16)
        return fail("Buffer too small for DAT file header");

    if (!looks_like_dat(data, size))
        return fail("Data does not look like a DAT file");

    DatFile result;
    DatSummary& summary = result.summary;

    summary.file_len           = read_u32be(data + 0);
    summary.num_images         = read_u32be(data + 4);
    // [+8] zeros — ignored
    summary.first_image_offset = read_u32be(data + 12); // imgOff[0]

    // Read offset table: starts at byte 12, numImages entries of 4 bytes each.
    const std::size_t offset_table_size = 12u + summary.num_images * 4u;
    if (!can_read(0, offset_table_size, size))
        return fail("Buffer truncated in DAT offset table");

    std::vector<std::uint32_t> offsets(summary.num_images);
    for (std::uint32_t i = 0; i < summary.num_images; ++i)
        offsets[i] = read_u32be(data + 12 + i * 4);

    result.images.reserve(summary.num_images);

    for (std::uint32_t i = 0; i < summary.num_images; ++i) {
        const std::uint64_t img_base = offsets[i];

        // Need at least 80 bytes for the image record header.
        if (!can_read(static_cast<std::size_t>(img_base), 80, size)) {
            // Malformed sub-record — skip gracefully.
            continue;
        }

        const std::uint8_t* hdr = data + img_base;

        DatImage img;
        img.file_offset = img_base;

        // [+00] uint32 len          — total block size (ignored)
        // [+04] uint32 const=1      — ignored
        // [+08] uint32 const=0      — ignored
        // [+12] uint32 const=16     — ignored (IMAGE_HEADER_CONST)
        // [+16] uint32 len2         — size of block after first 16 bytes
        // [+20] uint32 flag         — 0x02 when charId==0, else 0 (ignored)
        // [+24] uint32 const=0      — ignored
        // [+28] uint32 const=0      — ignored
        // [+32] uint8  color.b
        // [+33] uint8  color.g
        // [+34] uint8  color.r
        // [+35] uint8  color.a
        // [+36] uint32 charId
        // [+40] uint32 const=0      — ignored
        // [+44] uint32 const=0      — ignored
        // [+48] float  matrix.m00   (Flash: a)
        // [+52] float  matrix.m01   (Flash: c)
        // [+56] float  matrix.m10   (Flash: b)
        // [+60] float  matrix.m11   (Flash: d)
        // [+64] float  offset.x
        // [+68] float  offset.y
        // [+72] uint32 numShapes
        // [+76] uint32 distFromLen2ToFP  — bytes from &len2 (+16) to first vertex

        img.color_b    = hdr[32];
        img.color_g    = hdr[33];
        img.color_r    = hdr[34];
        img.color_a    = hdr[35];
        img.charId     = read_u32be(hdr + 36);
        img.matrix_m00 = read_f32be(hdr + 48);
        img.matrix_m01 = read_f32be(hdr + 52);
        img.matrix_m10 = read_f32be(hdr + 56);
        img.matrix_m11 = read_f32be(hdr + 60);
        img.offset_x   = read_f32be(hdr + 64);
        img.offset_y   = read_f32be(hdr + 68);
        img.num_shapes = read_u32be(hdr + 72);

        const std::uint32_t dist_from_len2_to_fp = read_u32be(hdr + 76);

        // First vertex is at:  img_base + 16 + distFromLen2ToFP
        const std::uint64_t first_vertex_off = img_base + 16u + dist_from_len2_to_fp;

        // Each triangle = 3 vertices × 32 bytes each.
        const std::uint64_t vertex_data_size =
            static_cast<std::uint64_t>(img.num_shapes) * 3u * 32u;

        if (img.num_shapes > 0 &&
            can_read(static_cast<std::size_t>(first_vertex_off),
                     static_cast<std::size_t>(vertex_data_size),
                     size))
        {
            img.triangles.resize(img.num_shapes);
            const std::uint8_t* vp = data + first_vertex_off;
            for (std::uint32_t t = 0; t < img.num_shapes; ++t) {
                for (int v = 0; v < 3; ++v) {
                    img.triangles[t].v[v].x = read_f32be(vp + 0);
                    img.triangles[t].v[v].y = read_f32be(vp + 4);
                    // 24 bytes of per-vertex padding discarded
                    vp += 32;
                }
            }
        }

        result.images.push_back(std::move(img));
    }

    result.original_bytes.assign(data, data + size);
    return result;
}

// ---------------------------------------------------------------------------
// read_dat_file
// ---------------------------------------------------------------------------

std::optional<DatFile> read_dat_file(const std::string& path,
                                      std::string*       err)
{
    std::ifstream ifs(path, std::ios::binary | std::ios::ate);
    if (!ifs) {
        if (err) *err = "Cannot open file: " + path;
        return std::nullopt;
    }

    const auto file_size = static_cast<std::size_t>(ifs.tellg());
    ifs.seekg(0);

    std::vector<std::uint8_t> buf(file_size);
    if (!ifs.read(reinterpret_cast<char*>(buf.data()),
                  static_cast<std::streamsize>(file_size))) {
        if (err) *err = "Read error: " + path;
        return std::nullopt;
    }

    return parse_dat(buf.data(), buf.size(), err);
}

} // namespace gf::dat
