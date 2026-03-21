find_package(ZLIB REQUIRED)

add_library(gf_core
  src/log.cpp
  src/config.cpp
  src/features.cpp
  src/container_magic.cpp
  src/file_detection.cpp
  src/version_model.cpp
  src/AstRootResolver.cpp
  src/AstArchive.cpp
  src/AstContainerEditor.cpp
  src/AstContainerBuilder.cpp
  src/scan.cpp
  src/zip_writer.cpp
  src/safe_write.cpp
)

target_include_directories(gf_core PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(gf_core PUBLIC
  spdlog::spdlog
  nlohmann_json::nlohmann_json
  ZLIB::ZLIB
)

target_compile_features(gf_core PUBLIC cxx_std_20)
