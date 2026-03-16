#include <gf/apt/apt_renderer.hpp>

#include <algorithm>
#include <string>
#include <utility>

namespace gf::apt {

static const AptCharacter* resolve_character(const AptFile& aptFile,
                                             const std::vector<AptCharacter>& characterTable,
                                             std::uint32_t characterId) {
  if (characterId < characterTable.size() && characterTable[characterId].type != 0) {
    return &characterTable[characterId];
  }
  if (&characterTable != &aptFile.characters
      && characterId < aptFile.characters.size()
      && aptFile.characters[characterId].type != 0) {
    return &aptFile.characters[characterId];
  }
  return nullptr;
}

std::vector<RenderNode> renderAptTimelineFrame(
    const AptFile&                   aptFile,
    const std::vector<AptFrame>&     timelineFrames,
    std::size_t                      frameIndex,
    const std::vector<AptCharacter>& characterTable,
    const Transform2D&               parentTransform,
    const RenderOptions&             opts,
    int                              currentDepth,
    int                              rootPlacementIndex,
    const std::string&               parentChainLabel)
{
  if (timelineFrames.empty() || currentDepth > opts.maxRecursionDepth) {
    return {};
  }
  const AptFrame resolved = build_display_list_frame(timelineFrames, frameIndex);
  return renderAptFrame(aptFile, resolved, characterTable, parentTransform,
                        opts, currentDepth, rootPlacementIndex, parentChainLabel);
}

// Recursion model (Flash display-list, column-vector math):
//   worldTransform = M_parent * M_local
// Using Transform2D::composeWith:
//   worldTransform = localTransform.composeWith(parentTransform)
std::vector<RenderNode> renderAptFrame(
    const AptFile&                   aptFile,
    const AptFrame&                  frame,
    const std::vector<AptCharacter>& characterTable,
    const Transform2D&               parentTransform,
    const RenderOptions&             opts,
    int                              currentDepth,
    int                              rootPlacementIndex,
    const std::string&               parentChainLabel)
{
  std::vector<RenderNode> nodes;
  if (currentDepth > opts.maxRecursionDepth)
    return nodes;

  const std::size_t count = frame.placements.size();
  std::vector<std::size_t> order(count);
  for (std::size_t i = 0; i < count; ++i) order[i] = i;
  std::stable_sort(order.begin(), order.end(),
    [&](std::size_t x, std::size_t y) {
      return frame.placements[x].depth < frame.placements[y].depth;
    });

  for (const std::size_t pi : order) {
    const AptPlacement& pl = frame.placements[pi];
    const int rootIdx = (currentDepth == 0)
        ? static_cast<int>(pi)
        : rootPlacementIndex;

    const Transform2D localTransform = toTransform2D(pl.transform);
    const Transform2D worldTransform = localTransform.composeWith(parentTransform);

    const AptCharacter* chPtr = resolve_character(aptFile, characterTable, pl.character);
    if (!chPtr) {
      RenderNode node;
      node.kind = RenderNode::Kind::Unknown;
      node.characterId = pl.character;
      node.placementDepth = pl.depth;
      node.instanceName = pl.instance_name;
      node.worldTransform = worldTransform;
      node.rootPlacementIndex = rootIdx;
      if (opts.collectParentChain) node.parentChainLabel = parentChainLabel;
      nodes.push_back(std::move(node));
      continue;
    }
    const AptCharacter& ch = *chPtr;

    if (ch.type == 5 || ch.type == 9) {
      if (!ch.frames.empty()) {
        std::string childLabel;
        if (opts.collectParentChain) {
          const std::string sprName = pl.instance_name.empty()
              ? ((ch.type == 9 ? std::string("Movie C") : std::string("Sprite C")) + std::to_string(pl.character))
              : pl.instance_name;
          childLabel = parentChainLabel + " \xE2\x86\x92 " + sprName;
        }
        const std::vector<AptCharacter>& childTable =
            (ch.type == 9 && !ch.nested_characters.empty()) ? ch.nested_characters : characterTable;

        auto sub = renderAptTimelineFrame(aptFile, ch.frames, 0, childTable,
                                          worldTransform, opts, currentDepth + 1,
                                          rootIdx, childLabel);
        if (!sub.empty()) {
          nodes.insert(nodes.end(),
                       std::make_move_iterator(sub.begin()),
                       std::make_move_iterator(sub.end()));
        } else {
          RenderNode plh;
          plh.kind = (ch.type == 9) ? RenderNode::Kind::Movie : RenderNode::Kind::Sprite;
          plh.characterId = pl.character;
          plh.placementDepth = pl.depth;
          plh.instanceName = pl.instance_name;
          plh.worldTransform = worldTransform;
          plh.localBounds = ch.bounds;
          plh.rootPlacementIndex = rootIdx;
          if (opts.collectParentChain) plh.parentChainLabel = parentChainLabel;
          nodes.push_back(std::move(plh));
        }
      } else {
        RenderNode node;
        node.kind = (ch.type == 9) ? RenderNode::Kind::Movie : RenderNode::Kind::Sprite;
        node.characterId = pl.character;
        node.placementDepth = pl.depth;
        node.instanceName = pl.instance_name;
        node.worldTransform = worldTransform;
        node.localBounds = ch.bounds;
        node.rootPlacementIndex = rootIdx;
        if (opts.collectParentChain) node.parentChainLabel = parentChainLabel;
        nodes.push_back(std::move(node));
      }
      continue;
    }

    RenderNode node;
    node.kind = kindFromCharType(ch.type);
    node.characterId = pl.character;
    node.placementDepth = pl.depth;
    node.instanceName = pl.instance_name;
    node.worldTransform = worldTransform;
    node.localBounds = ch.bounds;
    node.rootPlacementIndex = rootIdx;
    if (opts.collectParentChain) node.parentChainLabel = parentChainLabel;
    nodes.push_back(std::move(node));
  }

  return nodes;
}

} // namespace gf::apt
