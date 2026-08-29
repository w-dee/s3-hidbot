# Project instructions

## Start here

- This file contains stable repository and agent invariants; it is not a
  project-history or current-status document.
- Before changing code, read
  [`docs/development/codex-runbook.md`](docs/development/codex-runbook.md).
- Use [`docs/development/validation-entrypoints.md`](docs/development/validation-entrypoints.md)
  for canonical local and CI checks.
- Read [`docs/development/hardware-validation.md`](docs/development/hardware-validation.md)
  before any physical s3-hidbot work.
- Read [`docs/development/uart-control-plane.md`](docs/development/uart-control-plane.md)
  when changing protocol, host, or HID behavior.

## Privacy

- Never place developer-specific absolute home paths in tracked files.
- Use repository-relative paths, `$HOME`, `${HOME}`, `~/`, or documented
  placeholders.
- Run the repository privacy lint before committing.

## Git / sandbox

- The required hook path is `.githooks`; verify `core.hooksPath` before
  committing. Changing it writes `.git/config` and may require elevation.
- Operations that write `.git` metadata may require elevated execution.
- After a permission denial, do not repeatedly retry the unchanged operation
  in the normal sandbox; elevate only when the operation requires it.

## Physical serial

- Serial devices may be invisible from the normal sandbox. Do not infer a
  physical disconnection from sandbox invisibility.
- Use elevated execution when physical access is necessary.
- Machine-specific serial identifiers belong in local environment/configuration,
  not tracked instructions.

## ESP-IDF

- Do not hardcode developer-specific ESP-IDF installation paths or activation
  script absolute paths in tracked files.
- `firmware/` is the ESP-IDF project root. Do not run `idf.py` from the
  repository root as though it were the project root.

## Review gates

1. Perform a READ-ONLY audit, then stop for human review.
2. Perform implementation plus host/static/build validation, then stop for
   human review.
3. Perform real-hardware validation, then stop for human review.

- Do not cross a gate unless a human explicitly requests the next stage.

## Commit / push

- Do not commit failed validation or unintended/generated tracked files.
- Do not automatically commit on `main`.
- Push only on explicit human request.
