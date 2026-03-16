add_library(gf_models
  src/rsf.cpp
  src/scan_result.cpp
  src/rsf_preview.cpp
)

target_include_directories(gf_models PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(gf_models PUBLIC cxx_std_20)

target_link_libraries(gf_models PUBLIC
  nlohmann_json::nlohmann_json
)
