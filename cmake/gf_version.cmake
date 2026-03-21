# Central versioning: SemVer + optional build metadata
set(GF_VERSION_MAJOR 1)
set(GF_VERSION_MINOR 0)
set(GF_VERSION_PATCH 1)
set(GF_VERSION_REV 0)

set(GF_VERSION_STRING "v${GF_VERSION_MAJOR}.${GF_VERSION_MINOR}.${GF_VERSION_PATCH}")

configure_file(
  ${CMAKE_CURRENT_LIST_DIR}/templates/version.hpp.in
  ${CMAKE_BINARY_DIR}/generated/gf_core/version.hpp
  @ONLY
)
