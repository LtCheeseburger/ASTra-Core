# Requirements (Outline)

## Purpose
ASTra is a cross-platform SDK + toolsuite for CFBR / NCAA14 modding focused on safe AST editing, fast texture workflows, and future model viewing/import/export.

## Non-negotiables (Phase 1)
- No-corruption AST read/write
- Safe write: backups + atomic commits
- CLI-first operations available for CI/batch
- Cross-platform (Windows/macOS/Linux)

## Success Criteria
- AST mount/edit with 100% success rate (no corruption)
- Texture import/display in-game 100% success rate
- Texture workflow 90%+ faster vs legacy tools (target)

## Out of scope (initial skeleton)
- Full AST format coverage
- DDS/BCn full pipeline
- Model import/export
