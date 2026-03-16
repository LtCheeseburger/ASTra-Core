# ASTra Detection Rules Contract

This document defines **non-negotiable, user-facing behavior** for ASTra's
library scanning + container detection.

The intent is to prevent regressions as we move toward editor features in v0.6+.
If behavior here changes, it **must** be accompanied by:

- A versioned changelog entry
- Updated tests / fixtures
- A deliberate migration note for users

## Core Principles

1. **Header sniffing beats extensions**
   - Container classification is based on file header magic, not filename suffix.
   - Extensions may be misleading in real dumps.

2. **Platform rules are authoritative**
   - Scan roots and recursion rules vary by platform and must be respected.

3. **Diagnostics are non-invasive**
   - Debug traces, summaries, and warnings must not change scan outcomes.

4. **Warnings are soft**
   - Common mistakes are surfaced as informational warnings.
   - They must never block adding a game or rescanning.

5. **Game IDs are immutable**
   - Rescans must not change existing IDs.
   - ID generation must be stable/deterministic for supported platforms.

## Container Classification

The following 4-byte header magics are recognized:

- `BIGF`, `BIG4`, `BIG ` → **BIG**
- `TERF` → **TERF**
- Anything else → **Unknown**

When reporting "primary" container type:

- If any BIG/TERF is found, primary is the type with the larger count.
- If neither is found, primary is **Unknown**.

## Platform Scan Roots

### PS Vita (`psvita`)

- **AST/BGFA archives must be treated as root-only.**
  - Scan title root (non-recursive) for `*.ast`.
  - Do not introduce recursion for Vita AST discovery.
- If no ASTs are found at root, show a **soft informational warning**.

### PSP (`psp`)

- Primary TERF location is typically:
  - `PSP_GAME/USRDIR/data`
- `*.viv` containers are treated as BIG4.
- PSP titles may contain BIG/BIG4 containers in multiple folders.
- If the selected root is missing `PSP_GAME`, show a **soft informational warning**.

### PS3 (`ps3`)

- Default scan root is typically `USRDIR` (resolved from a game root).

### Xbox 360 (`xbox360`)

- Container types vary by title. Header sniffing is always used.

## Guardrails

If a chosen folder clearly is **not** a game dump (examples):

- No `PARAM.SFO`
- No `PSP_GAME` folder
- No obvious platform markers

Then show a **one-time soft warning** explaining the selection likely isn't a game.

## Debug Trace

When debug trace is enabled, a scan summary should be emitted containing:

- Scan root
- Folders scanned
- Files examined
- AST count
- BIG count
- TERF count
- Primary container type

This output is logging-only and must not affect behavior.
