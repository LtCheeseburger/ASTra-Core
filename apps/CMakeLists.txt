add_subdirectory(gf_cli)

if (GF_BUILD_GUI)
  add_subdirectory(gf_toolsuite_gui)
  if (WIN32)
    # Helper must be built after the GUI target so the POST_BUILD copy works.
    add_subdirectory(astra_updater)
  endif()
endif()
