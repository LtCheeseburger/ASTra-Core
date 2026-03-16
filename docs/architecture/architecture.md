# Architecture (Outline)

## Modules
- gf_core: logging, config, version, common utilities
- gf_io: paths, safe-write, backups, transactional file ops (next)
- gf_ast: AST reader/writer + mount/VFS + integrity checks (next)
- gf_textures: extraction/conversion/import validation (later)
- gf_models: placeholder for future (Assimp etc.)
- apps/gf_cli: batch + CI entrypoint
- apps/gf_toolsuite_gui: Qt6 GUI

## Dependency direction
apps -> libs
gf_ast depends on gf_io + gf_core
gf_textures depends on gf_ast + gf_io + gf_core
No cycles.

## Safety rules (must)
- Never in-place modify original AST
- Backup on first edit
- Atomic commit (temp + rename swap)
- Validate before commit
