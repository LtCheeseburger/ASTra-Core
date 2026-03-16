project(gf_apt LANGUAGES CXX)

add_library(gf_apt
  src/apt_reader.cpp
  src/apt_renderer.cpp
  src/apt_xml.cpp

  # Vendored AptConverter bits (used for APT -> XML in-tool conversion)
  src/aptconverter/Aptfile.cpp
  src/aptconverter/ActionHelper.cpp
  src/aptconverter/Flags.cpp
  src/aptconverter/Util.cpp
  src/aptconverter/tinyxml2.cpp
)

target_include_directories(gf_apt PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}/src/aptconverter
)

target_compile_features(gf_apt PUBLIC cxx_std_20)

# Avoid dllimport/export decoration from the upstream headers.
target_compile_definitions(gf_apt PRIVATE NO_DLL)

target_link_libraries(gf_apt PUBLIC
)

if (COMMAND gf_apply_warnings)
  gf_apply_warnings(gf_apt)
endif()
