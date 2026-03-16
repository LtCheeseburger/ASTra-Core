# AST validation fixtures

This folder is reserved for **AST/BGFA samples** used by the `astra ast-validate` CLI command and future automated tests.

## Suggested layout

- `good/`  — known-good AST/BGFA files that should parse with status `ok`
- `weird/` — valid-but-unusual files that should parse with status `ok` or `warning`
- `bad/`   — corrupted/truncated/malformed files that should **not crash** and should report status `error`

## How to run

From a build folder containing the `astra` CLI binary:

```bash
astra ast-validate --path ../tests/fixtures/ast --recursive --json
```

> Note: large commercial game archives are not committed to the repo.
> Keep fixtures small and anonymized whenever possible.
