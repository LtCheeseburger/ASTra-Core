#pragma once

// gf::dat — EA APT geometry (DAT) file parser.
//
// DAT files are companion assets to EA APT UI files.  They store the
// actual drawable geometry for APT Shape characters: triangle meshes,
// colour info, a 2-D transform matrix, and a charId that links the
// entry back to the APT character table.
//
// Binary layout (all multi-byte fields are big-endian):
//
//  File header  (12 bytes + numImages*4 bytes offset table)
//    [0x00] uint32  fileLen              — total file size
//    [0x04] uint32  numImages            — number of image/entry records
//    [0x08] uint32  zeros                — always 0
//    [0x0C] uint32  imgOff[0]            — byte offset to image 0 data
//    [0x10] uint32  imgOff[1]            — byte offset to image 1 data
//           ...
//
//  Image record  (80-byte header + numShapes*3 vertices)
//    [+00]  uint32  len                  — total block size
//    [+04]  uint32  const=1              — ignored
//    [+08]  uint32  const=0              — ignored
//    [+12]  uint32  const=16             — ignored (IMAGE_HEADER_CONST)
//    [+16]  uint32  len2                 — size of block after the first 16 bytes
//    [+20]  uint32  flag                 — 0x02 when charId==0, else 0
//    [+24]  uint32  const=0              — ignored
//    [+28]  uint32  const=0              — ignored
//    [+32]  uint8   color.b              — blue
//    [+33]  uint8   color.g              — green
//    [+34]  uint8   color.r              — red
//    [+35]  uint8   color.a              — alpha
//    [+36]  uint32  charId               — APT character table ID
//    [+40]  uint32  const=0              — ignored
//    [+44]  uint32  const=0              — ignored
//    [+48]  float   matrix.m00           — rotation/scale  (Flash: 'a')
//    [+52]  float   matrix.m01           — rotation/scale  (Flash: 'c')
//    [+56]  float   matrix.m10           — rotation/scale  (Flash: 'b')
//    [+60]  float   matrix.m11           — rotation/scale  (Flash: 'd')
//    [+64]  float   offset.x             — initial X position
//    [+68]  float   offset.y             — initial Y position
//    [+72]  uint32  numShapes            — number of triangles
//    [+76]  uint32  distFromLen2ToFP     — bytes from &len2 (+16) to first vertex
//    <gap bytes, then vertex data>
//
//  Vertex  (32 bytes each, 3 per triangle)
//    [+00]  float   x
//    [+04]  float   y
//    [+08..+31]  ignored (24 bytes of padding/unknown)

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace gf::dat {

// One triangle vertex.  Only x/y are meaningful; 24 bytes of per-vertex
// padding in the binary are discarded during parsing.
struct DatVertex {
  float x = 0.0f;
  float y = 0.0f;
};

// One triangle (3 vertices).
struct DatTriangle {
  DatVertex v[3];
};

// All data parsed from a single image/entry record.
struct DatImage {
  std::uint32_t charId      = 0;    // APT character table reference
  std::uint8_t  color_r     = 0;
  std::uint8_t  color_g     = 0;
  std::uint8_t  color_b     = 0;
  std::uint8_t  color_a     = 255;
  float         matrix_m00  = 1.0f; // Flash: a (scale X)
  float         matrix_m01  = 0.0f; // Flash: c (skew  Y)
  float         matrix_m10  = 0.0f; // Flash: b (skew  X)
  float         matrix_m11  = 1.0f; // Flash: d (scale Y)
  float         offset_x    = 0.0f;
  float         offset_y    = 0.0f;
  std::uint32_t num_shapes  = 0;    // declared triangle count in header
  std::vector<DatTriangle> triangles;

  // Byte offset of this record within the file (for hex cross-reference).
  std::uint64_t file_offset = 0;
};

// File-level summary (mirrors AptSummary pattern).
struct DatSummary {
  std::uint32_t file_len            = 0;
  std::uint32_t num_images          = 0;
  std::uint32_t first_image_offset  = 0; // value of imgOff[0]
};

// Complete parsed document.
struct DatFile {
  DatSummary              summary;
  std::vector<DatImage>   images;
  // Raw bytes kept for hex-preview use; populated by read_dat_file and parse_dat.
  std::vector<std::uint8_t> original_bytes;
};

// ---------------------------------------------------------------------------
// API
// ---------------------------------------------------------------------------

// Quick header-only heuristic: returns true if the first bytes of `data`
// look structurally consistent with a DAT file.
// Safe to call with only 16 bytes available (partial buffer).
bool looks_like_dat(const std::uint8_t* data, std::size_t size) noexcept;

// Parse a DAT file from a raw in-memory buffer.
// On success, returns a populated DatFile.
// On failure, returns std::nullopt and (if err != nullptr) sets *err.
// Never throws; malformed sub-records are skipped gracefully.
std::optional<DatFile> parse_dat(const std::uint8_t* data,
                                 std::size_t          size,
                                 std::string*         err = nullptr);

// Read a DAT file from disk, then delegate to parse_dat.
std::optional<DatFile> read_dat_file(const std::string& path,
                                     std::string*       err = nullptr);

} // namespace gf::dat
