#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <gf/models/rsf.hpp>

namespace gf::models::rsf {

struct preview_transform {
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
  float rotation_deg = 0.0f;
  float scale_x = 1.0f;
  float scale_y = 1.0f;
  float scale_z = 1.0f;
};

struct preview_material {
  int material_index = -1;
  std::string name;
  std::string sub_name;
  std::optional<std::uint32_t> texture_index;
  std::string texture_name;
  std::string texture_filename;
  float color_r = 1.0f;
  float color_g = 1.0f;
  float color_b = 1.0f;
  float alpha = 1.0f;
  float uv_scale_x = 1.0f;
  float uv_scale_y = 1.0f;
  float uv_offset_x = 0.0f;
  float uv_offset_y = 0.0f;
};

struct vec2f {
  float x = 0.0f;
  float y = 0.0f;
};

struct vec3f {
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
};

struct mesh_data {
  std::vector<vec3f> positions;
  std::vector<vec3f> normals;
  std::vector<vec2f> uvs;
  std::vector<std::uint32_t> indices;
  vec3f bounds_min{};
  vec3f bounds_max{};
  bool has_uvs = false;
  bool indexed = false;
};

struct preview_object {
  int object_index = -1;
  int material_index = -1;
  int mesh_candidate_index = -1;
  std::string label;
  preview_transform transform;
  preview_transform original_transform;
  preview_material material;
  float proxy_width = 256.0f;
  float proxy_height = 256.0f;
  bool transform_editable = false;
  bool has_proxy_geometry = true;
  bool has_decoded_geometry = false;
  std::vector<std::string> mapped_params;
  std::vector<std::string> warnings;
};

struct geometry_decode_warning {
  std::size_t offset = 0;
  std::string message;
};

struct geometry_candidate {
  std::string label;
  std::string source_kind;
  std::size_t vertex_offset = 0;
  std::size_t vertex_limit = 0;
  std::size_t index_offset = 0;
  std::size_t source_offset = 0;
  std::size_t source_size = 0;
  std::size_t emitted_vertices = 0;
  std::size_t emitted_indices = 0;
  int stride_guess = 0;
  int index_size = 0;
  int uv_offset = -1;
  float confidence = 0.0f;
  float bounds_extent = 0.0f;
  std::size_t degenerate_triangles = 0;
  std::vector<std::string> reasons;
  mesh_data mesh;
};

struct geometry_decode_result {
  std::size_t geom_section_offset = 0;
  std::size_t geom_section_size = 0;
  std::vector<std::size_t> strm_offsets;
  std::vector<std::size_t> idi6_offsets;
  std::vector<geometry_candidate> candidates;
  int selected_candidate = -1;
  std::vector<geometry_decode_warning> warnings;
};

struct preview_document {
  std::string display_name;
  std::vector<preview_object> objects;
  std::vector<std::string> warnings;
  geometry_decode_result geometry;
  bool partial_decode = true;
};

preview_document build_preview_document(const document& doc);
geometry_decode_result decode_geom_candidates(const document& doc, std::span<const std::uint8_t> original_bytes);

} // namespace gf::models::rsf
