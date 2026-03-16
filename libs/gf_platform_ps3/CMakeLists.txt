add_library(gf_platform_ps3
  src/param_sfo.cpp
)

target_include_directories(gf_platform_ps3 PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(gf_platform_ps3 PUBLIC
  gf_core
)

target_compile_features(gf_platform_ps3 PUBLIC cxx_std_20)
