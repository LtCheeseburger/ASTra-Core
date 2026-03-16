# ASTra

ASTra is a C++20 + Qt6 modding toolkit focused on EA Sports archive and UI formats used by NCAA Football 14 and related titles.

This repository is the active source tree for the desktop app and supporting libraries. The project is currently in an internal alpha stage, with current work centered on archive browsing/editing, texture workflows, RSF handling, and APT UI tooling.

## Current focus

ASTra is being built as a safer, more modern replacement for older game-specific tools. The main goals are:

- inspect and browse AST/BGFA containers
- safely replace or export supported files
- work with RSF data and related configs
- inspect textures and support EA/XPR2 texture workflows
- parse and edit APT UI data with a visual preview/editor foundation
- provide both a GUI app and a CLI for batch or validation workflows

## Project layout

```text
ASTra/
├─ apps/
│  ├─ gf_cli/                # command-line entrypoint
│  └─ gf_toolsuite_gui/      # Qt6 desktop app
├─ libs/
│  ├─ gf_apt/                # APT parsing / XML / preview helpers
│  ├─ gf_core/               # core utilities, archive logic, safe-write helpers
│  ├─ gf_models/             # shared models and RSF-related types
│  ├─ gf_platform_ps3/       # PS3/platform-specific helpers
│  └─ gf_textures/           # DDS/XPR2 and texture rebuild helpers
├─ docs/
│  ├─ architecture/
│  └─ requirements/
├─ tests/
└─ cmake/
```

## Build requirements

Minimum tooling:

- CMake 3.24+
- a C++20 compiler
- Ninja or another supported CMake generator
- Qt 6 Widgets
- Git
- zlib

Third-party libraries such as `spdlog`, `nlohmann/json`, and `Catch2` are pulled through CMake `FetchContent`.

## Build

### Windows example

```powershell
cmake -S . -B out-win
cmake --build out-win --target gf_toolsuite_gui -j 8
```

### Run

```powershell
out-win\apps\gf_cli\astra.exe --version
out-win\apps\gf_toolsuite_gui\gf_toolsuite_gui.exe
```

## Notes

- Qt is expected to be installed separately and discoverable by CMake.
- Generated files are written to the selected build directory.
- The repository may move quickly while the internal toolset is being cleaned up and prepared for broader releases.

## Documentation

Additional project notes live here:

- `docs/requirements/requirements.md`
- `docs/architecture/architecture.md`
- `docs/detection_rules.md`

## Status

This is not intended to be a polished public release branch yet. It is the live working repository for ASTra as the tool moves toward a more complete 0.8.x/0.9.x feature set.

## License

See `LICENSE`.
