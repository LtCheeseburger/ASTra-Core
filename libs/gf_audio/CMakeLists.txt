add_library(gf_audio STATIC
  src/sbkr_reader.cpp
  src/sbbe_reader.cpp
)
target_include_directories(gf_audio PUBLIC include)
target_compile_features(gf_audio PRIVATE cxx_std_20)
if (COMMAND gf_apply_warnings)
  gf_apply_warnings(gf_audio)
endif()
