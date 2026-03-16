add_library(gf_textures
  src/dds_decode.cpp
  src/dds_validate.cpp
  src/ea_dds_rebuild.cpp
  src/xpr2_rebuild.cpp
  src/xbox360_xpr2_untile.cpp
  src/tga_reader.cpp
  src/dxt_compress.cpp
  src/dds_build.cpp
)

option(USE_XGADDRESS_UNTILE "Enable Xbox 360 XGAddress untile path for XPR2" OFF)

if (USE_XGADDRESS_UNTILE)
  target_compile_definitions(gf_textures PRIVATE USE_XGADDRESS_UNTILE=1)
endif()

target_include_directories(gf_textures
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(gf_textures PUBLIC cxx_std_20)

if (COMMAND gf_apply_warnings)
  gf_apply_warnings(gf_textures)
endif()
