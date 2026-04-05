# Audio file parsers (SBKR descriptors, SBbe containers)
add_subdirectory(gf_audio)

add_subdirectory(gf_core)

add_subdirectory(gf_models)

add_subdirectory(gf_platform_ps3)

# Preview-only texture decoding (DDS) used by the GUI viewer.
add_subdirectory(gf_textures)

# APT container support (viewer groundwork in v0.6.21.x)
add_subdirectory(gf_apt)

# DAT geometry file parser (companion to APT shape assets)
add_subdirectory(gf_dat)
