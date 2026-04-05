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
  src/ModProfileStore.cpp
  src/ModWorkspaceManager.cpp
  src/ModProfileManager.cpp
  src/ProfileContextService.cpp
  src/ActiveProfileWidget.cpp
  src/ModProfilesDialog.cpp
  src/ModManifestReader.cpp
  src/ModRegistryStore.cpp
  src/ModInstallPlanner.cpp
  src/ModInstallTransaction.cpp
  src/ModInstaller.cpp
  src/InstallModDialog.cpp
  src/ModPackageExporter.cpp
  src/ExportModDialog.cpp
  src/ModAssetValidator.cpp
  src/ModMetadataEditorDialog.cpp
  src/ProfileWorkspaceNaming.cpp
  src/ShellHelper.cpp
  src/ModCatalogService.cpp
  src/InstalledModsModel.cpp
  src/InstalledModsDialog.cpp
  src/RuntimeTargetManager.cpp
  src/BaselineCaptureService.cpp
  src/RuntimeSetupDialog.cpp
  src/ProfileResolverService.cpp
  src/ProfileApplyService.cpp
  src/DeploymentStateStore.cpp
  src/DriftDetector.cpp
  src/LaunchService.cpp
  # Phase 6
  src/ModUninstallService.cpp
  src/ProfileCloneService.cpp
  src/ApplyPreviewDialog.cpp
  src/NewProfileDialog.cpp
  src/MediaImportDialog.cpp
  # Phase 7: copy-based profile workspaces
  src/ProfileWorkspaceBuilder.cpp
  # Phase 3B: audio browser
  src/AudioPlayer.cpp
  src/WaveformWidget.cpp
  src/AudioBrowserPanel.cpp
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
  src/ModProfile.hpp
  src/RuntimeContentRoot.hpp
  src/ModProfileStore.hpp
  src/ModWorkspaceManager.hpp
  src/ModProfileManager.hpp
  src/ProfileContextService.hpp
  src/ActiveProfileWidget.hpp
  src/ModProfilesDialog.hpp
  src/ModManifest.hpp
  src/ModManifestReader.hpp
  src/ModInstallPlan.hpp
  src/InstalledModRecord.hpp
  src/ModRegistryStore.hpp
  src/ModInstallPlanner.hpp
  src/ModInstallTransaction.hpp
  src/ModInstaller.hpp
  src/InstallModDialog.hpp
  src/ModPackageSpec.hpp
  src/ModPackageExporter.hpp
  src/ExportModDialog.hpp
  src/ModAssetValidator.hpp
  src/ModMetadataEditorDialog.hpp
  src/ProfileWorkspaceNaming.hpp
  src/ShellHelper.hpp
  src/ModCatalogEntry.hpp
  src/ModCatalogService.hpp
  src/InstalledModsModel.hpp
  src/InstalledModsDialog.hpp
  src/RuntimeTargetConfig.hpp
  src/RuntimeTargetManager.hpp
  src/BaselineCaptureService.hpp
  src/RuntimeSetupDialog.hpp
  src/ProfileResolverService.hpp
  src/ProfileApplyService.hpp
  src/DeploymentState.hpp
  src/DeploymentStateStore.hpp
  src/DriftDetector.hpp
  src/LaunchService.hpp
  # Phase 6
  src/ContentRootDiscovery.hpp
  src/ModUninstallService.hpp
  src/ProfileCloneService.hpp
  src/ApplyPreviewDialog.hpp
  src/NewProfileDialog.hpp
  src/MediaImportDialog.hpp
  # Phase 7: copy-based profile workspaces
  src/ProfileWorkspaceBuilder.hpp
  # Phase 3B: audio browser
  src/AudioPlayer.hpp
  src/WaveformWidget.hpp
  src/AudioBrowserPanel.hpp
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
  ${CMAKE_SOURCE_DIR}/libs/gf_audio/include
  ${CMAKE_SOURCE_DIR}/libs/gf_core/include
  ${CMAKE_SOURCE_DIR}/libs/gf_models/include
  ${CMAKE_SOURCE_DIR}/libs/gf_apt/include
  ${CMAKE_SOURCE_DIR}/libs/gf_dat/include
  /libs/gf_platform_ps3/include
  /libs/gf_textures/include
)

target_link_libraries(gf_toolsuite_gui PRIVATE
  Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Concurrent Qt6::Network
  gf_audio
  gf_core
  gf_models
  gf_platform_ps3
  gf_textures
  gf_apt
  gf_dat
  winmm   # WinMM audio (waveOut API) for AudioPlayer
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


