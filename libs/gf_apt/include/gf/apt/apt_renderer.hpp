#pragma once

#include <gf/apt/apt_reader.hpp>
#include <gf/apt/apt_transform.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace gf::apt {

// ── Render node ─────────────────────────────────────────────────────────────
//
// A flat, fully-resolved descriptor for one leaf element to be drawn.
// The renderer returns a depth-sorted (back-to-front) list of these.
// The GUI layer converts them to QGraphicsItems (or whatever backend it uses).

struct RenderNode {
  enum class Kind : std::uint8_t {
    Shape    = 1,
    EditText = 2,
    Button   = 4,
    Sprite   = 5,  // sprites that could not be fully recursed (depth-limited)
    Image    = 7,
    Movie    = 9,
    Unknown  = 0,
  };

  Kind        kind        = Kind::Unknown;
  std::uint32_t characterId = 0;
  std::uint32_t placementDepth = 0;   // depth value from the placement record
  std::string instanceName;            // APT placement instance_name (may be empty)

  // Human-readable resolved names (set by the GUI layer; empty when unknown).
  std::string symbolName;   // export/import symbol name, e.g. "Screen", "CLOCK_PANEL"
  std::string importRef;    // cross-file import reference, e.g. "ui_common/CLOCK_PANEL"

  Transform2D worldTransform;          // accumulated transform to scene space
  std::optional<AptBounds> localBounds; // character's own bounds, if known

  // Index in the *root* frame's placement vector (−1 for nested placements).
  int rootPlacementIndex = -1;

  // Full parent-chain label for the debug overlay: "Root → Sprite C5 → ..."
  std::string parentChainLabel;
};

// ── Render options ───────────────────────────────────────────────────────────

struct RenderOptions {
  // Highlight one root-frame placement (highlights its descendants too).
  int highlightRootPlacementIdx = -1;

  // Stop recursing sprites/movies below this nesting depth (prevent infinite loops).
  int maxRecursionDepth = 8;

  // Fill RenderNode::parentChainLabel (slightly more expensive; used by debug overlay).
  bool collectParentChain = false;
};

// ── Main renderer function ───────────────────────────────────────────────────
//
// Recursively renders `frame` (and all nested Sprite/Movie characters) into a
// flat, depth-sorted list of RenderNodes.
//
// characterTable:   character table to use for pl.character lookups.
//                   The renderer automatically switches to a Movie character's
//                   nested_characters when recursing into it.
//                   Use the convenience overload below to pass aptFile.characters.
// parentTransform:  accumulated world transform from all ancestors.
//                   Pass Transform2D::identity() for the root frame.
// currentDepth:     nesting level; 0 for the root frame.
// parentChainLabel: debug label; starts as "Root".

std::vector<RenderNode> renderAptFrame(
    const AptFile&                   aptFile,
    const AptFrame&                  frame,
    const std::vector<AptCharacter>& characterTable,
    const Transform2D&               parentTransform    = Transform2D::identity(),
    const RenderOptions&             opts               = {},
    int                              currentDepth       = 0,
    int                              rootPlacementIndex = -1,
    const std::string&               parentChainLabel   = "Root");

// Convenience overload: uses aptFile.characters as the character table.
inline std::vector<RenderNode> renderAptFrame(
    const AptFile&       aptFile,
    const AptFrame&      frame,
    const Transform2D&   parentTransform    = Transform2D::identity(),
    const RenderOptions& opts               = {},
    int                  currentDepth       = 0,
    int                  rootPlacementIndex = -1,
    const std::string&   parentChainLabel   = "Root") {
  return renderAptFrame(aptFile, frame, aptFile.characters,
                        parentTransform, opts, currentDepth,
                        rootPlacementIndex, parentChainLabel);
}



// Builds the cumulative display-list state for timelineFrames[0..frameIndex] and
// renders that resolved frame. This matches Flash/SWF's place/move/remove model.
std::vector<RenderNode> renderAptTimelineFrame(
    const AptFile&                   aptFile,
    const std::vector<AptFrame>&     timelineFrames,
    std::size_t                      frameIndex,
    const std::vector<AptCharacter>& characterTable,
    const Transform2D&               parentTransform    = Transform2D::identity(),
    const RenderOptions&             opts               = {},
    int                              currentDepth       = 0,
    int                              rootPlacementIndex = -1,
    const std::string&               parentChainLabel   = "Root");

inline std::vector<RenderNode> renderAptTimelineFrame(
    const AptFile&               aptFile,
    const std::vector<AptFrame>& timelineFrames,
    std::size_t                  frameIndex,
    const Transform2D&           parentTransform    = Transform2D::identity(),
    const RenderOptions&         opts               = {},
    int                          currentDepth       = 0,
    int                          rootPlacementIndex = -1,
    const std::string&           parentChainLabel   = "Root") {
  return renderAptTimelineFrame(aptFile, timelineFrames, frameIndex, aptFile.characters,
                                parentTransform, opts, currentDepth,
                                rootPlacementIndex, parentChainLabel);
}

// ── Helpers ──────────────────────────────────────────────────────────────────

// Build a Transform2D from an AptTransform (bridges apt_reader ↔ apt_transform).
inline Transform2D toTransform2D(const AptTransform& t) noexcept {
  Transform2D r;
  r.a  = t.scale_x;
  r.b  = t.rotate_skew_0;
  r.c  = t.rotate_skew_1;
  r.d  = t.scale_y;
  r.tx = t.x;
  r.ty = t.y;
  return r;
}

// Classify a character type value as a RenderNode::Kind.
inline RenderNode::Kind kindFromCharType(std::uint32_t type) noexcept {
  switch (type) {
    case 1:  return RenderNode::Kind::Shape;
    case 2:  return RenderNode::Kind::EditText;
    case 4:  return RenderNode::Kind::Button;
    case 5:  return RenderNode::Kind::Sprite;
    case 7:  return RenderNode::Kind::Image;
    case 9:  return RenderNode::Kind::Movie;
    default: return RenderNode::Kind::Unknown;
  }
}

} // namespace gf::apt
