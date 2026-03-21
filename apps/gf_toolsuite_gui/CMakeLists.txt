cmake_minimum_required(VERSION 3.20)

# ASTra Toolsuite GUI (Qt Widgets)
project(gf_toolsuite_gui LANGUAGES CXX)

# Qt
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Concurrent Network)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC OFF)
set(CMAKE_AUTORCC ON)

set(GF_TOOLS_GUI_SOURCES
  src/main.cpp
  src/MainWindow.cpp
  src/AstIndexer.cpp
  src/update/UpdateChecker.cpp
  src/update/UpdateDialog.cpp
  src/update/UpdaterLauncher.cpp
  src/update/VersionBadgeWidget.cpp
  src/OperationStatusDialog.cpp
  src/GameSelectorWindow.cpp
  src/NameCache.cpp
  src/PlatformUtils.cpp
  src/GameLibrary.cpp
  src/TextEditorWindow.cpp
  src/TextSyntaxHighlighter.cpp
  src/AddGameDialog.cpp
  src/CreateDlcAstDialog.cpp
  src/CreateRsfAstDialog.cpp
  src/GamePlatform.cpp
  src/GameIconProvider.cpp
  src/apt_editor/AptPreviewScene.cpp
  src/apt_editor/AptPreviewItem.cpp
  src/apt_editor/AptSelectionManager.cpp
  src/rsf_editor/RsfPreviewWidget.cpp
)

set(GF_TOOLS_GUI_HEADERS
  src/MainWindow.hpp
  src/AstIndexer.hpp
  src/update/UpdateChecker.hpp
  src/update/UpdateDialog.hpp
  src/update/UpdaterLauncher.hpp
  src/update/UpdaterConfig.hpp
  src/update/VersionBadgeWidget.hpp
  src/OperationStatusDialog.hpp
  src/GameSelectorWindow.hpp
  src/GameLibrary.hpp
  src/AddGameDialog.hpp
  src/CreateDlcAstDialog.hpp
  src/CreateRsfAstDialog.hpp
  src/GamePlatform.hpp
  src/PlatformUtils.hpp
  src/GameIconProvider.hpp
  src/TextEditorWindow.hpp
  src/TextSyntaxHighlighter.hpp
  src/DocumentLifecycle.hpp
  src/apt_editor/AptPreviewScene.hpp
  src/apt_editor/AptPreviewItem.hpp
  src/apt_editor/AptSelectionManager.hpp
  src/rsf_editor/RsfPreviewWidget.hpp
)

set(GF_TOOLS_GUI_RESOURCES
  resources/astra_resources.qrc
)

add_executable(gf_toolsuite_gui
  ${GF_TOOLS_GUI_SOURCES}
  ${GF_TOOLS_GUI_HEADERS}
  ${GF_TOOLS_GUI_RESOURCES}
)

target_compile_features(gf_toolsuite_gui PRIVATE cxx_std_20)

# Make Windows GUI app (no console) in Release if desired:
# You can toggle this later; keeping console can help debugging.
if (WIN32)
  # If you want to remove console window later, uncomment:
  # set_target_properties(gf_toolsuite_gui PROPERTIES WIN32_EXECUTABLE TRUE)
endif()

target_include_directories(gf_toolsuite_gui PRIVATE
  ${CMAKE_SOURCE_DIR}/libs/gf_core/include
  ${CMAKE_SOURCE_DIR}/libs/gf_models/include
  ${CMAKE_SOURCE_DIR}/libs/gf_apt/include
  ${CMAKE_SOURCE_DIR}/libs/gf_dat/include
  /libs/gf_platform_ps3/include
  /libs/gf_textures/include
)

target_link_libraries(gf_toolsuite_gui PRIVATE
  Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Concurrent Qt6::Network
  gf_core
  gf_models
  gf_platform_ps3
  gf_textures
  gf_apt
  gf_dat
)

# Warnings (from root cmake/gf_warnings.cmake)
if (COMMAND gf_apply_warnings)
  gf_apply_warnings(gf_toolsuite_gui)
endif()

# Copy external game_icons to runtime output (dev-friendly)
add_custom_command(TARGET gf_toolsuite_gui POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:gf_toolsuite_gui>/game_icons"
  COMMAND ${CMAKE_COMMAND} -E copy_directory
    "${CMAKE_CURRENT_SOURCE_DIR}/resources/game_icons"
    "$<TARGET_FILE_DIR:gf_toolsuite_gui>/game_icons"
  COMMENT "Copying game_icons to runtime folder"
)
if (WIN32)
  target_sources(gf_toolsuite_gui PRIVATE resources/astra.rc)
  set_target_properties(gf_toolsuite_gui PROPERTIES WIN32_EXECUTABLE TRUE)
endif()


