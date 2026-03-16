#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace gf::models::rsf {

// RSF supports both little- and big-endian payload fields depending on platform.
// For PS3, most section fields are big-endian. We auto-detect endianness by
// peeking the first section tag (expects 'INFO').
enum class endian { little, big };

struct texture_entry {
  std::string name;      // logical name
  std::string filename;  // resource path/filename
};

struct param_tex   { std::uint32_t texture_index = 0; };
struct param_vec   { std::vector<float> values; };
struct param_bool  { bool value = false; };
struct param_i32   { std::int32_t value = 0; };
struct param_f32   { float value = 0.0f; };

using param_value = std::variant<param_tex, param_vec, param_bool, param_i32, param_f32>;

struct material_param {
  // VariableType is an ASCII 1-4 char code stored as a 4-byte ID field.
  // Examples: "tex", "v2", "v3", "v4", "bool", "i32", "f32".
  std::string var_type;
  std::vector<std::string> names; // usually 1 element, sometimes more
  param_value value;
};

struct material {
  std::string name;
  std::string sub_name;
  std::uint32_t pad0 = 0;
  std::uint32_t pad1 = 0;
  std::uint32_t pad2 = 0;
  std::uint32_t sub_name_count = 0;
  std::uint32_t unknown_count = 0;
  std::vector<material_param> params;
};

struct section_slice {
  std::uint32_t tag = 0; // 4cc
  std::size_t offset = 0;
  std::size_t size = 0;
};

struct document {
  endian order = endian::big;

  // Raw RSF header bytes (first 16 bytes). We preserve these to avoid
  // accidentally changing unknown fields.
  std::vector<std::uint8_t> header16;

  // Parsed editable pieces
  std::vector<texture_entry> textures;
  std::vector<material> materials;

  // Raw slices for sections we don't edit (INFO/JOBN/GEOM/SHDR/SGRF/etc).
  // We will reuse them verbatim when rebuilding.
  std::vector<section_slice> sections;
  std::vector<std::uint8_t> tail;
};

std::optional<document> parse(std::span<const std::uint8_t> bytes);

// Rebuild an RSF file by reusing untouched section bytes and rewriting the
// TEXT and MATL sections from the parsed structures.
// Rebuild a new RSF buffer.
//
// We reuse raw bytes for sections we don't currently edit and rewrite TEXT and
// MATL sections from the parsed structures.
std::vector<std::uint8_t> rebuild(const document& doc, std::span<const std::uint8_t> original_bytes);

// Helpers for UI
std::string fourcc(std::uint32_t tag);

} // namespace gf::models::rsf
