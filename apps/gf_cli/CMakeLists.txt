add_executable(gf_cli
  src/main.cpp
)

target_link_libraries(gf_cli PRIVATE gf_core gf_models)

target_include_directories(gf_cli PRIVATE
  ${CMAKE_BINARY_DIR}/generated
)

set_target_properties(gf_cli PROPERTIES OUTPUT_NAME "astra")
