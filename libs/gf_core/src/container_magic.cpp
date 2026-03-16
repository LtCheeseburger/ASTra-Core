#include "gf/core/container_magic.hpp"

namespace gf::core {

ContainerMagicType classify_container_magic(const std::array<char, 4>& m) {
  if (m[0] == 'B' && m[1] == 'I' && m[2] == 'G' && (m[3] == 'F' || m[3] == '4' || m[3] == ' ')) {
    return ContainerMagicType::Big;
  }
  if (m[0] == 'T' && m[1] == 'E' && m[2] == 'R' && m[3] == 'F') {
    return ContainerMagicType::Terf;
  }
  return ContainerMagicType::Unknown;
}

} // namespace gf::core
