#include "gf/models/rsf_preview.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <limits>
#include <optional>
#include <set>
#include <string_view>

namespace gf::models::rsf {
namespace {

std::string to_lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

bool contains_any(std::string_view s, std::initializer_list<std::string_view> needles) {
  for (const auto n : needles) {
    if (s.find(n) != std::string_view::npos) return true;
  }
  return false;
}

void clamp_color(float& v) {
  if (std::isnan(v)) v = 1.0f;
  v = std::clamp(v, 0.0f, 1.0f);
}

void apply_named_scalar(preview_object& obj, preview_material& mat, std::string_view name, float v) {
  const std::string n = to_lower(std::string(name));
  if (contains_any(n, {"offsetx", "translatex", "positionx", "posx", "centerx", "pivotx", "locx"})) {
    obj.transform.x = v;
    obj.transform_editable = true;
    obj.mapped_params.emplace_back(std::string(name));
  } else if (contains_any(n, {"offsety", "translatey", "positiony", "posy", "centery", "pivoty", "locy"})) {
    obj.transform.y = v;
    obj.transform_editable = true;
    obj.mapped_params.emplace_back(std::string(name));
  } else if (contains_any(n, {"offsetz", "translatez", "positionz", "posz", "centerz", "pivotz", "locz"})) {
    obj.transform.z = v;
    obj.transform_editable = true;
    obj.mapped_params.emplace_back(std::string(name));
  } else if (contains_any(n, {"rot", "angle", "yaw", "heading"})) {
    obj.transform.rotation_deg = v;
    obj.transform_editable = true;
    obj.mapped_params.emplace_back(std::string(name));
  } else if (contains_any(n, {"scalex", "sizex", "width", "stretchx"})) {
    obj.transform.scale_x = (std::abs(v) < 0.0001f) ? 1.0f : v;
    obj.transform_editable = true;
    obj.mapped_params.emplace_back(std::string(name));
  } else if (contains_any(n, {"scaley", "sizey", "height", "stretchy"})) {
    obj.transform.scale_y = (std::abs(v) < 0.0001f) ? 1.0f : v;
    obj.transform_editable = true;
    obj.mapped_params.emplace_back(std::string(name));
  } else if (contains_any(n, {"scalez", "sizez", "stretchz"})) {
    obj.transform.scale_z = (std::abs(v) < 0.0001f) ? 1.0f : v;
    obj.transform_editable = true;
    obj.mapped_params.emplace_back(std::string(name));
  } else if (contains_any(n, {"alpha", "opacity"})) {
    mat.alpha = v > 1.0f ? (v / 255.0f) : v;
  } else if (contains_any(n, {"uvscaleu", "uvscale_x", "texscalex", "uscale"})) {
    mat.uv_scale_x = (std::abs(v) < 0.0001f) ? 1.0f : v;
  } else if (contains_any(n, {"uvscalev", "uvscale_y", "texscaley", "vscale"})) {
    mat.uv_scale_y = (std::abs(v) < 0.0001f) ? 1.0f : v;
  } else if (contains_any(n, {"uvoffsetu", "uvoffset_x", "texoffsetx", "uoffset"})) {
    mat.uv_offset_x = v;
  } else if (contains_any(n, {"uvoffsetv", "uvoffset_y", "texoffsety", "voffset"})) {
    mat.uv_offset_y = v;
  }
}

void apply_named_vector(preview_object& obj, preview_material& mat, std::string_view name, const param_vec& vec) {
  const std::string n = to_lower(std::string(name));
  if (vec.values.empty()) return;

  if (contains_any(n, {"color", "colour", "tint"})) {
    if (vec.values.size() >= 3) {
      mat.color_r = vec.values[0];
      mat.color_g = vec.values[1];
      mat.color_b = vec.values[2];
    }
    if (vec.values.size() >= 4) mat.alpha = vec.values[3];
    return;
  }

  if (contains_any(n, {"offset", "translate", "position", "pos", "pivot", "center"})) {
    if (vec.values.size() >= 1) obj.transform.x = vec.values[0];
    if (vec.values.size() >= 2) obj.transform.y = vec.values[1];
    if (vec.values.size() >= 3) obj.transform.z = vec.values[2];
    obj.transform_editable = true;
    obj.mapped_params.emplace_back(std::string(name));
    return;
  }

  if (contains_any(n, {"scale", "size"})) {
    if (vec.values.size() >= 1 && std::abs(vec.values[0]) > 0.0001f) obj.transform.scale_x = vec.values[0];
    if (vec.values.size() >= 2 && std::abs(vec.values[1]) > 0.0001f) obj.transform.scale_y = vec.values[1];
    if (vec.values.size() >= 3 && std::abs(vec.values[2]) > 0.0001f) obj.transform.scale_z = vec.values[2];
    obj.transform_editable = true;
    obj.mapped_params.emplace_back(std::string(name));
    return;
  }

  if (contains_any(n, {"uv", "texcoord", "tex"}) && vec.values.size() >= 2) {
    mat.uv_offset_x = vec.values[0];
    mat.uv_offset_y = vec.values[1];
  }
}

float decode_short_signed_bytepair(std::uint8_t hi, std::uint8_t lo) {
  if (hi == 0xFFu && lo == 0xFFu) return -0.0f;
  if (hi >= 0xF0u) {
    const float whole = static_cast<float>(int(hi) - 255);
    const float frac = static_cast<float>(256 - int(lo)) / 256.0f;
    return whole - frac;
  }
  return static_cast<float>(hi) + (static_cast<float>(lo) / 256.0f);
}

bool read_be_u16(std::span<const std::uint8_t> b, std::size_t off, std::uint16_t& out) {
  if (off + 2 > b.size()) return false;
  out = static_cast<std::uint16_t>((std::uint16_t(b[off]) << 8) | std::uint16_t(b[off + 1]));
  return true;
}

bool read_be_u32(std::span<const std::uint8_t> b, std::size_t off, std::uint32_t& out) {
  if (off + 4 > b.size()) return false;
  out = (std::uint32_t(b[off]) << 24) | (std::uint32_t(b[off + 1]) << 16) | (std::uint32_t(b[off + 2]) << 8) | std::uint32_t(b[off + 3]);
  return true;
}

void update_bounds(mesh_data& mesh, const vec3f& p) {
  if (mesh.positions.empty()) {
    mesh.bounds_min = p;
    mesh.bounds_max = p;
    return;
  }
  mesh.bounds_min.x = std::min(mesh.bounds_min.x, p.x);
  mesh.bounds_min.y = std::min(mesh.bounds_min.y, p.y);
  mesh.bounds_min.z = std::min(mesh.bounds_min.z, p.z);
  mesh.bounds_max.x = std::max(mesh.bounds_max.x, p.x);
  mesh.bounds_max.y = std::max(mesh.bounds_max.y, p.y);
  mesh.bounds_max.z = std::max(mesh.bounds_max.z, p.z);
}

float extent_of(const mesh_data& mesh) {
  return std::max({mesh.bounds_max.x - mesh.bounds_min.x,
                   mesh.bounds_max.y - mesh.bounds_min.y,
                   mesh.bounds_max.z - mesh.bounds_min.z});
}

std::optional<mesh_data> decode_candidate_vertices(std::span<const std::uint8_t> bytes,
                                                   std::size_t offset,
                                                   std::size_t limit,
                                                   int stride,
                                                   int uv_offset) {
  if (stride < 6 || offset + 6 > bytes.size() || limit <= offset) return std::nullopt;
  mesh_data mesh;
  const std::size_t max_count = std::min<std::size_t>((limit - offset) / std::size_t(stride), 8192);
  for (std::size_t i = 0; i < max_count; ++i) {
    const std::size_t base = offset + i * std::size_t(stride);
    if (base + 6 > bytes.size()) break;
    const vec3f p{
      decode_short_signed_bytepair(bytes[base + 0], bytes[base + 1]),
      decode_short_signed_bytepair(bytes[base + 2], bytes[base + 3]),
      decode_short_signed_bytepair(bytes[base + 4], bytes[base + 5])
    };
    if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) break;
    if (std::abs(p.x) > 4096.0f || std::abs(p.y) > 4096.0f || std::abs(p.z) > 4096.0f) break;
    mesh.positions.push_back(p);
    update_bounds(mesh, p);

    if (uv_offset >= 0 && base + std::size_t(uv_offset + 4) <= bytes.size()) {
      std::uint16_t u = 0;
      std::uint16_t v = 0;
      if (read_be_u16(bytes, base + std::size_t(uv_offset), u) && read_be_u16(bytes, base + std::size_t(uv_offset + 2), v)) {
        mesh.uvs.push_back(vec2f{static_cast<float>(u) / 65535.0f, static_cast<float>(v) / 65535.0f});
        mesh.has_uvs = true;
      }
    }
  }
  if (mesh.positions.size() < 24) return std::nullopt;
  return mesh;
}

float score_mesh(const mesh_data& mesh, std::vector<std::string>& reasons) {
  if (mesh.positions.size() < 24) return 0.0f;
  const float ext = extent_of(mesh);
  if (ext <= 0.0001f) return 0.0f;

  std::size_t unique_count = 0;
  std::set<std::tuple<int, int, int>> uniq;
  for (const auto& p : mesh.positions) {
    uniq.emplace(int(std::round(p.x * 8.0f)), int(std::round(p.y * 8.0f)), int(std::round(p.z * 8.0f)));
  }
  unique_count = uniq.size();
  const float unique_ratio = static_cast<float>(unique_count) / static_cast<float>(mesh.positions.size());

  double adj_sum = 0.0;
  for (std::size_t i = 1; i < mesh.positions.size(); ++i) {
    const auto& a = mesh.positions[i - 1];
    const auto& b = mesh.positions[i];
    const double dx = double(a.x) - double(b.x);
    const double dy = double(a.y) - double(b.y);
    const double dz = double(a.z) - double(b.z);
    adj_sum += std::sqrt(dx * dx + dy * dy + dz * dz);
  }
  const float adj_avg = mesh.positions.size() > 1 ? static_cast<float>(adj_sum / double(mesh.positions.size() - 1)) : 0.0f;

  float score = 0.0f;
  if (ext > 0.25f && ext < 2048.0f) {
    score += 0.35f;
    reasons.emplace_back("bounds extent looks plausible for short-signed model-space coordinates");
  }
  if (unique_ratio > 0.70f) {
    score += 0.25f;
    reasons.emplace_back("decoded vertex cloud has a high unique-position ratio");
  }
  if (adj_avg > 0.001f && adj_avg < 256.0f) {
    score += 0.20f;
    reasons.emplace_back("adjacent decoded vertices stay within a reasonable local distance");
  }
  if (mesh.has_uvs && mesh.uvs.size() == mesh.positions.size()) {
    score += 0.15f;
    reasons.emplace_back("candidate exposes interleaved UV-like data");
  }
  if (mesh.positions.size() >= 128) {
    score += 0.10f;
    reasons.emplace_back("candidate emits a substantial number of vertices");
  }
  return std::clamp(score, 0.0f, 1.0f);
}

std::size_t find_index_pattern16(std::span<const std::uint8_t> bytes, std::size_t begin, std::size_t end) {
  const std::size_t stop = std::min(end, bytes.size());
  for (std::size_t i = begin; i + 6 <= stop; ++i) {
    if (bytes[i + 0] == 0x00 && bytes[i + 1] == 0x00 &&
        bytes[i + 2] == 0x00 && bytes[i + 3] == 0x01 &&
        bytes[i + 4] == 0x00 && bytes[i + 5] == 0x02) {
      return i;
    }
  }
  return std::numeric_limits<std::size_t>::max();
}

void attach_indices(std::span<const std::uint8_t> bytes,
                    geometry_candidate& candidate,
                    std::size_t search_begin,
                    std::size_t search_end) {
  const std::size_t max_vertices = candidate.mesh.positions.size();
  if (max_vertices < 3) return;

  const std::size_t idx_off = find_index_pattern16(bytes, search_begin, search_end);
  if (idx_off == std::numeric_limits<std::size_t>::max()) {
    candidate.reasons.emplace_back("no 16-bit 0,1,2 index pattern was found near the candidate vertex window");
    return;
  }

  candidate.index_offset = idx_off;
  candidate.index_size = 2;
  std::size_t degenerate = 0;
  for (std::size_t cur = idx_off; cur + 6 <= bytes.size(); cur += 2) {
    std::uint16_t v = 0;
    if (!read_be_u16(bytes, cur, v)) break;
    if (v >= max_vertices) break;
    candidate.mesh.indices.push_back(v);
    if (candidate.mesh.indices.size() >= 3 && (candidate.mesh.indices.size() % 3u) == 0u) {
      const auto a = candidate.mesh.indices[candidate.mesh.indices.size() - 3];
      const auto b = candidate.mesh.indices[candidate.mesh.indices.size() - 2];
      const auto c = candidate.mesh.indices[candidate.mesh.indices.size() - 1];
      if (a == b || b == c || a == c) ++degenerate;
    }
    if (candidate.mesh.indices.size() >= max_vertices * 12u) break;
  }

  candidate.mesh.indexed = candidate.mesh.indices.size() >= 6;
  candidate.emitted_indices = candidate.mesh.indices.size();
  candidate.degenerate_triangles = degenerate;
  if (candidate.mesh.indexed) {
    candidate.reasons.emplace_back("found a nearby 16-bit face/index run beginning with 00 00 00 01 00 02");
  }
}

void add_candidate(std::vector<geometry_candidate>& out,
                   std::span<const std::uint8_t> bytes,
                   std::size_t offset,
                   std::size_t limit,
                   int stride,
                   int uv_offset,
                   std::string source_kind) {
  auto mesh = decode_candidate_vertices(bytes, offset, limit, stride, uv_offset);
  if (!mesh) return;

  geometry_candidate c;
  c.vertex_offset = offset;
  c.vertex_limit = limit;
  c.source_offset = offset;
  c.source_size = limit > offset ? (limit - offset) : 0;
  c.stride_guess = stride;
  c.uv_offset = uv_offset;
  c.source_kind = std::move(source_kind);
  c.mesh = std::move(*mesh);
  c.emitted_vertices = c.mesh.positions.size();
  c.bounds_extent = extent_of(c.mesh);
  c.label = "candidate @0x" + [&]() {
    char buf[32]{};
    std::snprintf(buf, sizeof(buf), "%zx", offset);
    return std::string(buf);
  }();
  c.confidence = score_mesh(c.mesh, c.reasons);
  attach_indices(bytes, c, limit, std::min(bytes.size(), limit + std::size_t(1u << 18)));
  if (c.mesh.indexed) c.confidence = std::min(1.0f, c.confidence + 0.15f);
  if (c.degenerate_triangles > 0 && c.mesh.indices.size() >= 3) {
    const float ratio = static_cast<float>(c.degenerate_triangles) / static_cast<float>(c.mesh.indices.size() / 3u);
    if (ratio > 0.35f) {
      c.confidence = std::max(0.0f, c.confidence - 0.10f);
      c.reasons.emplace_back("triangle stream contains a high degenerate ratio");
    }
  }
  if (c.confidence < 0.35f) return;
  out.push_back(std::move(c));
}

} // namespace

preview_document build_preview_document(const document& doc) {
  preview_document out;
  out.partial_decode = true;

  if (doc.materials.empty()) {
    out.warnings.emplace_back("RSF preview fell back to summary mode because no materials were parsed.");
    return out;
  }

  constexpr int kProxyColumns = 5;
  constexpr float kProxySpacing = 120.0f;
  for (std::size_t i = 0; i < doc.materials.size(); ++i) {
    const auto& m = doc.materials[i];
    preview_object obj;
    obj.object_index = static_cast<int>(i);
    obj.material_index = static_cast<int>(i);
    obj.label = m.name.empty() ? (m.sub_name.empty() ? ("Material " + std::to_string(i)) : m.sub_name) : m.name;
    obj.material.material_index = static_cast<int>(i);
    obj.material.name = m.name;
    obj.material.sub_name = m.sub_name;

    for (const auto& p : m.params) {
      const std::string pname = p.names.empty() ? p.var_type : p.names.front();
      if (std::holds_alternative<param_tex>(p.value)) {
        const auto tex = std::get<param_tex>(p.value);
        obj.material.texture_index = tex.texture_index;
        if (tex.texture_index < doc.textures.size()) {
          const auto& t = doc.textures[tex.texture_index];
          obj.material.texture_name = t.name;
          obj.material.texture_filename = t.filename;
        } else {
          obj.warnings.emplace_back("Texture slot index is outside the parsed RSF texture table.");
        }
      } else if (std::holds_alternative<param_vec>(p.value)) {
        apply_named_vector(obj, obj.material, pname, std::get<param_vec>(p.value));
      } else if (std::holds_alternative<param_f32>(p.value)) {
        apply_named_scalar(obj, obj.material, pname, std::get<param_f32>(p.value).value);
      } else if (std::holds_alternative<param_i32>(p.value)) {
        apply_named_scalar(obj, obj.material, pname, static_cast<float>(std::get<param_i32>(p.value).value));
      } else if (std::holds_alternative<param_bool>(p.value)) {
        if (contains_any(to_lower(pname), {"visible", "enable", "enabled"}) && !std::get<param_bool>(p.value).value) {
          obj.material.alpha = 0.35f;
        }
      }
    }

    clamp_color(obj.material.color_r);
    clamp_color(obj.material.color_g);
    clamp_color(obj.material.color_b);
    clamp_color(obj.material.alpha);

    if (!obj.transform_editable) {
      const int col = static_cast<int>(i % std::size_t(kProxyColumns));
      const int row = static_cast<int>(i / std::size_t(kProxyColumns));
      obj.transform.x = 80.0f + static_cast<float>(col) * kProxySpacing;
      obj.transform.y = 80.0f + static_cast<float>(row) * kProxySpacing;
      obj.warnings.emplace_back("No explicit transform parameters were recognized; using proxy layout placement.");
    }

    obj.original_transform = obj.transform;
    out.objects.push_back(std::move(obj));
  }

  if (doc.textures.empty()) {
    out.warnings.emplace_back("No RSF texture table was parsed; preview uses material colors and checkerboard proxies only.");
  }

  return out;
}

geometry_decode_result decode_geom_candidates(const document& doc, std::span<const std::uint8_t> original_bytes) {
  geometry_decode_result out;
  for (const auto& s : doc.sections) {
    if (fourcc(s.tag) == "GEOM") {
      out.geom_section_offset = s.offset;
      out.geom_section_size = s.size;
      break;
    }
  }

  auto scan_ascii = [&](const char* needle, std::vector<std::size_t>& dst) {
    const std::size_t n = std::char_traits<char>::length(needle);
    if (n == 0 || original_bytes.size() < n) return;
    for (std::size_t i = 0; i + n <= original_bytes.size(); ++i) {
      if (std::memcmp(original_bytes.data() + i, needle, n) == 0) dst.push_back(i);
    }
  };

  scan_ascii("STRM", out.strm_offsets);
  scan_ascii("IDI6", out.idi6_offsets);

  if (out.geom_section_size == 0) {
    out.warnings.push_back({0, "No GEOM section tag was found. The decoder will rely on marker and heuristic scans."});
  }
  if (out.strm_offsets.empty()) {
    out.warnings.push_back({0, "No STRM candidate blocks were found during the current scan."});
  }
  if (out.idi6_offsets.empty()) {
    out.warnings.push_back({0, "No IDI6 candidate face/index markers were found during the current scan."});
  }

  std::vector<std::pair<std::size_t, std::size_t>> windows;
  if (!out.strm_offsets.empty()) {
    for (std::size_t i = 0; i < out.strm_offsets.size(); ++i) {
      const std::size_t start = out.strm_offsets[i] + 4;
      const std::size_t end = (i + 1 < out.strm_offsets.size()) ? out.strm_offsets[i + 1] : std::min(original_bytes.size(), start + std::size_t(1u << 19));
      windows.emplace_back(start, end);
    }
  }
  if (out.geom_section_size != 0) {
    const std::size_t start = out.geom_section_offset + 16;
    const std::size_t end = std::min(original_bytes.size(), out.geom_section_offset + out.geom_section_size);
    windows.emplace_back(start, end);
  }
  if (windows.empty()) {
    const std::size_t sample_end = std::min<std::size_t>(original_bytes.size(), 1u << 20);
    for (std::size_t off = 0; off + 512 < sample_end; off += 512) windows.emplace_back(off, std::min(sample_end, off + std::size_t(1u << 15)));
  }

  for (const auto& [start, end] : windows) {
    const std::size_t limit = std::min(end, original_bytes.size());
    for (std::size_t off = start; off + 64 < limit; off += 4) {
      for (int stride : {6, 8, 10, 12, 16, 20, 24, 28, 32}) {
        add_candidate(out.candidates, original_bytes, off, limit, stride, stride >= 10 ? 6 : -1, "heuristic");
      }
    }
  }

  std::sort(out.candidates.begin(), out.candidates.end(), [](const auto& a, const auto& b) {
    if (std::abs(a.confidence - b.confidence) > 0.0001f) return a.confidence > b.confidence;
    if (a.mesh.indexed != b.mesh.indexed) return a.mesh.indexed > b.mesh.indexed;
    return a.emitted_vertices > b.emitted_vertices;
  });

  if (out.candidates.size() > 8) out.candidates.resize(8);
  if (!out.candidates.empty()) {
    out.selected_candidate = 0;
    out.warnings.push_back({out.candidates.front().vertex_offset,
                            "Geometry investigation found one or more plausible mesh candidates. Use Wireframe or UV mode to inspect them."});
  } else {
    out.warnings.push_back({0, "No mesh candidate cleared the current confidence threshold. Preview stays in proxy mode."});
  }
  return out;
}

} // namespace gf::models::rsf
