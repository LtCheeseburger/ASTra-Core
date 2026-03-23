add_executable(gf_tests
  test_version.cpp
  test_config.cpp
  test_container_magic.cpp
  test_features.cpp
  test_scan_result_json.cpp
  test_scan_heuristics.cpp
  test_ast_container_editor.cpp
  test_ast_container_builder.cpp
  test_apt_transform.cpp
  test_apt_display_list.cpp
  test_rsf_preview.cpp
  test_dds_validate.cpp
  test_texture_replace.cpp
)

target_link_libraries(gf_tests PRIVATE
  gf_core
  gf_models
  gf_apt
  gf_textures
  Catch2::Catch2WithMain
)

add_test(NAME gf_tests COMMAND gf_tests)
