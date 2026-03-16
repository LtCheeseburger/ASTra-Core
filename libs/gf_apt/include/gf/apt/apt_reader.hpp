#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace gf::apt {

struct AptImport {
  std::string movie;
  std::string name;
  std::uint32_t character = 0;
  std::uint64_t offset = 0;
};

struct AptExport {
  std::string name;
  std::uint32_t character = 0;
  std::uint64_t offset = 0;
};

struct AptTransform {
  // Flash/EA APT 2D affine matrix: | a  c  tx |  → x' = a*x + c*y + tx
  //                                | b  d  ty |     y' = b*x + d*y + ty
  double x = 0.0;             // tx  (translation X)
  double y = 0.0;             // ty  (translation Y)
  double scale_x = 1.0;      // a   (scale / cos component)
  double rotate_skew_0 = 0.0; // b   (rotate / sin component)
  double rotate_skew_1 = 0.0; // c   (rotate / -sin component)
  double scale_y = 1.0;      // d   (scale / cos component)
};

struct AptColorTransform {
  double mul_r = 1.0;
  double mul_g = 1.0;
  double mul_b = 1.0;
  double mul_a = 1.0;
  double add_r = 0.0;
  double add_g = 0.0;
  double add_b = 0.0;
  double add_a = 0.0;
};

enum class AptFrameItemKind : std::uint8_t {
  Unknown = 0,
  Action = 1,
  FrameLabel = 2,
  PlaceObject = 3,
  RemoveObject = 4,
  BackgroundColor = 5,
  InitAction = 8,
};

struct AptPlacement {
  std::uint32_t depth = 0;
  std::uint32_t character = 0;
  std::uint32_t flags = 0;
  std::string instance_name;
  AptTransform transform;
  AptColorTransform color_transform;
  std::uint64_t offset = 0;
};

struct AptFrameItem {
  AptFrameItemKind kind = AptFrameItemKind::Unknown;
  std::uint64_t offset = 0;
  AptPlacement placement;          // valid for PlaceObject
  std::uint32_t remove_depth = 0;  // valid for RemoveObject
  std::uint32_t background_rgba = 0;
  std::uint32_t init_sprite = 0;
  std::string label;               // valid for FrameLabel
  // Byte offset within original_apt where the action bytecode stream starts.
  // Valid for Action (kind==1) and InitAction (kind==8); 0 = not available.
  std::uint64_t action_bytes_offset = 0;
};

struct AptFrame {
  std::uint32_t index = 0;
  std::uint32_t frameitemcount = 0;
  std::uint64_t offset = 0;
  std::uint64_t items_offset = 0;
  std::vector<AptPlacement> placements;
  std::vector<AptFrameItem> items;
};

struct AptBounds {
  float left = 0.0f;
  float top = 0.0f;
  float right = 0.0f;
  float bottom = 0.0f;
};

struct AptCharacter {
  std::uint32_t index = 0;
  std::uint32_t id = 0;
  std::uint32_t type = 0;
  std::uint32_t signature = 0;
  std::string linkage_name;
  std::uint64_t offset = 0;
  std::optional<AptBounds> bounds;
  std::vector<AptFrame> frames;

  // For type==9 (Movie) characters: the nested movie's own character table,
  // index-aligned so nested_characters[i] corresponds to nested character ID i.
  // Empty for non-Movie types and for Movies with no parseable character table.
  std::vector<AptCharacter> nested_characters;
};

struct AptSlice {
  std::string name;
  std::uint64_t offset = 0;
  std::uint64_t size = 0;
};

// Minimal summary used by the GUI viewer groundwork.
struct AptSummary {
  std::uint32_t screensizex = 0;
  std::uint32_t screensizey = 0;
  std::uint32_t framecount = 0;
  std::uint32_t charactercount = 0;
  std::uint32_t importcount = 0;
  std::uint32_t exportcount = 0;
  std::uint64_t aptdataoffset = 0;
};

struct AptFile {
  AptSummary summary;
  std::vector<AptSlice> slices;
  std::vector<AptImport> imports;
  std::vector<AptExport> exports;
  std::vector<AptFrame> frames;
  std::vector<AptCharacter> characters;
  std::vector<std::uint8_t> original_apt;
  std::vector<std::uint8_t> original_const;

  [[nodiscard]] std::vector<std::uint8_t> rebuild_binary() const;
};

// Returns a human-readable name for a character type value.
// Matches the CharacterType enum from the EA APT binary format.
inline std::string aptCharTypeName(std::uint32_t type) {
  switch (type) {
    case 1:  return "Shape";
    case 2:  return "EditText";
    case 3:  return "Font";
    case 4:  return "Button";
    case 5:  return "Sprite";
    case 7:  return "Image";
    case 8:  return "Morph";
    case 9:  return "Movie";
    case 10: return "Text";
    default: return "Unknown(" + std::to_string(type) + ")";
  }
}

// Reads basic information from an APT file and its matching .const file.
//
// v0.6.21.1: parses the CONST header + OutputMovie tables (best-effort)
// and provides stable slices for the hex viewer.
//
// Returns std::nullopt on failure and populates err (if provided).
std::optional<AptFile> read_apt_file(const std::string& apt_path,
                                     const std::string& const_path,
                                     std::string* err);

std::optional<AptSummary> read_apt_summary(const std::string& apt_path,
                                          const std::string& const_path,
                                          std::string* err);

[[nodiscard]] AptFrame build_display_list_frame(const std::vector<AptFrame>& timeline_frames,
                                                std::size_t frame_index);

} // namespace gf::apt
