# Codex development runbook

## Scope

`AGENTS.md` defines stable repository invariants. This runbook explains the
development procedure. Physical s3-hidbot work also requires
[`hardware-validation.md`](hardware-validation.md), which is a separate review
gate from implementation and static validation.

## Repository preflight

Before changing tracked files, establish the intended branch, local HEAD,
remote baseline when relevant, and tracked-worktree cleanliness. Confirm that
`core.hooksPath` is `.githooks`, that `.githooks/pre-commit` exists, and that
the ESP-IDF project exists at `firmware/CMakeLists.txt`.

Run the repository privacy lint before a commit:

```bash
python3 tools/privacy_lint.py --staged
```

The pre-commit hook is a backstop, not a replacement for reviewing changed
paths and their staged content.

## Sandbox and elevation

Read-only Git commands normally need no special handling. Commands that write
Git metadata can be denied in a normal sandbox; `git config` is one example.
Do not generalize a permission denial to every Git command or blindly retry the
same denied command. Use elevated execution only when the requested operation
requires it.

The required hook setting is a local `.git/config` write:

```bash
git config core.hooksPath .githooks
```

Verify that setting before commits; changing it is a deliberate local action.

## ESP-IDF environment

The current project baseline is ESP-IDF v5.5.4. Activate the pinned/supported
ESP-IDF v5.5.4 environment before invoking its tools. Installation and
activation details are local-environment concerns and must not be hardcoded
into tracked files.

`firmware/` is the ESP-IDF project root. Run `idf.py` from that directory (or
pass it with `-C firmware`); do not treat the repository root as the project
root. A repository wrapper script has not been established.

## Machine-local configuration

Keep workstation-specific configuration outside the repository, for example in
`~/.codex/AGENTS.md` or shell environment variables. This includes the exact
serial by-id value, developer-specific ESP-IDF activation details,
workstation-specific workspace paths, and elevation behavior.

## Review gates, commit, and push

Keep these stages separate:

1. READ-ONLY audit — stop for human review.
2. Implementation plus host/static/build validation — stop for human review.
3. Real-hardware validation — stop for human review.

Do not silently carry a task across a gate. Do not commit failed validation or
unintended/generated tracked files, and do not automatically commit on `main`.
Push only when a human explicitly requests it.
