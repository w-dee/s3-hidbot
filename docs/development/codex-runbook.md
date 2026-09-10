# Codex development runbook

## Production DLE-LAST SDK

Firmware builds require a fresh isolated ESP-IDF v5.5.4 tree carrying the exact
`tools/sdk-patches/nimble-dle-last.patch`. Do not patch a shared SDK. With the
ordinary clean v5.5.4 environment active:

```sh
production_sdk_parent=$(mktemp -d)
python3 tools/prepare_production_sdk.py --source "$IDF_PATH" --output "$production_sdk_parent/sdk"
export IDF_PATH="$production_sdk_parent/sdk"
export PATH="$IDF_PATH/tools:$PATH"
./tools/test-firmware.sh
```

The same verifier runs during CMake configuration and every incremental build,
and before artifact creation. Preparation rejects dirty/already-patched input;
verification accepts only the locked revisions, unchanged controller archive,
and exact single-file patch. All submodules must already be available in the
stock checkout. `firmware/production-sdk.lock.json` describes the effective
dependency; manifest version 2 embeds that identity. Version 1 release artifacts
remain readable and immutable. CI uses this same preparation and verifier,
with independent prepared SDK locations for reproducibility builds.

The SDK's actual runtime version string is `v5.5.4-dirty`. Version 2 artifacts
record this build-observed string so exact runtime identity comparison remains
valid. The dirty suffix is not dependency provenance: the separately verified
revision, patch, source and archive hashes supply that identity. No UART field
or protocol version is added for this purpose.

Only the automatic successful-CONNECT DLE tail is suppressed. The existing
control owner calls update, connected heap checkpoint, security initiation,
then one admitted direct Set Data Length (251 octets, 17040 us). DLE errors do
not add retries or teardown. Admission is retired by disconnect, reset or hide;
a command already admitted before retirement may still be in flight. This is
host API sequencing, not an RF/controller ordering or physical qualification
claim. Both fresh pairing and retained-bond reconnect require a separate
full-product physical gate. No diagnostic counters are part of production.

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
root. The canonical repository wrappers and their IDF prerequisites are
listed in [`validation-entrypoints.md`](validation-entrypoints.md); do not
duplicate their test logic in ad-hoc commands.

The UART Control Plane v1 transport invariants and deferred protocol decisions
are documented in [`uart-control-plane.md`](uart-control-plane.md). Physical
procedures and the current evidence/deferred matrix belong to
[`hardware-validation.md`](hardware-validation.md).

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

## Related documents

- [`validation-entrypoints.md`](validation-entrypoints.md) — canonical local
  and CI validation commands.
- [`hardware-validation.md`](hardware-validation.md) — physical safety and
  hardware evidence.
- [`uart-control-plane.md`](uart-control-plane.md) — normative protocol and
  runtime contract.
