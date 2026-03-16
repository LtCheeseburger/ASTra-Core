#pragma once

#include <array>

namespace gf::core {

enum class ContainerMagicType {
  Big,
  Terf,
  Unknown,
};

// Classify a 4-byte file header magic into a container type.
ContainerMagicType classify_container_magic(const std::array<char, 4>& magic4);

} // namespace gf::core
