project(gf_dat LANGUAGES CXX)

add_library(gf_dat
  src/dat_reader.cpp
)

target_include_directories(gf_dat PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(gf_dat PUBLIC cxx_std_20)

target_link_libraries(gf_dat PUBLIC
)

if (COMMAND gf_apply_warnings)
  gf_apply_warnings(gf_dat)
endif()
