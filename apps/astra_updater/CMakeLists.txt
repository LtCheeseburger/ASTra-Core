cmake_minimum_required(VERSION 3.20)
project(astra_updater LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(astra_updater
  src/main.cpp
)

target_compile_features(astra_updater PRIVATE cxx_std_20)

if (WIN32)
  # GUI subsystem: no console window for the helper on Windows.
  set_target_properties(astra_updater PROPERTIES WIN32_EXECUTABLE TRUE)

  # Link shlwapi for ShellExecuteEx
  target_link_libraries(astra_updater PRIVATE shlwapi)
endif()

# After build, copy the helper next to the main GUI executable so it can
# be found at runtime by UpdaterLauncher.
add_custom_command(TARGET astra_updater POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy
    "$<TARGET_FILE:astra_updater>"
    "$<TARGET_FILE_DIR:gf_toolsuite_gui>/astra_updater.exe"
  COMMENT "Copying astra_updater.exe to GUI output directory"
  VERBATIM
)
