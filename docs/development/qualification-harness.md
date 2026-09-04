# Physical qualification harness

The repository-owned U7.6 qualification harness consolidates reusable control
and evidence machinery for later physical gates. It does not itself establish
hardware qualification, automate historical bond campaigns, or replace the
review gates in [`codex-runbook.md`](codex-runbook.md).

## Architecture

The implementation is intentionally split into small components:

- `tools/qualification_harness/core.py` owns bounded monotonic polling, fresh
  UART-session authority, route/exposure/bond and single-delivery invariants,
  no-auto-restore checks, result composition, privacy validation, and safe
  quiescent cleanup.
- `artifact.py` derives Git identity at runtime and delegates artifact
  acceptance to the official host artifact verifier. It records verified flash
  payloads, offsets and semantics, runtime ELF identity, and parsed partition
  geometry without exposing staging paths.
- `input.py` models stable evdev identity rediscovery and the focused F24 and
  `REL_X` checkpoints.
- `btmon.py` owns a bounded, injectable, unprivileged capture lifecycle and a
  compact path-free summary.
- `tools/qualification_runner.py` is a thin preflight entrypoint. Later U7.6D
  scenario orchestration should compose the modules rather than grow this
  entrypoint into a single campaign script.

Run the hardware-free self-test through the canonical static suite or directly:

```bash
./tools/test-qualification-harness.sh
```

With the host package dependencies available, later qualification can create
source-only or source-plus-artifact preflight evidence:

```bash
python3 tools/qualification_runner.py preflight \
  --artifact <verified-firmware-archive> \
  --evidence <new-qualification-evidence.json>
```

The runner derives the current Git SHA; a qualification SHA is never embedded
in permanent tooling. It does not source shell configuration, open a serial
port, start Bluetooth, mutate a bond, or run `sudo`. The evidence destination
must not already exist, preventing an earlier qualification record from being
silently overwritten.

## Safety boundaries

- BlueZ support is fixed to read-only target-name inspection. There is no
  remove, unpair, forget, wildcard, or remove-all operation. Every target host
  record deletion remains a separately approved human action.
- Machine-local `.envrc` contents and the actual UART path must never enter
  evidence, logs, tests, commits, or documentation. Only booleans such as
  `serial_resolved` may be reported.
- Every poll has an explicit timeout and interval and uses monotonic time.
  UART transport acquisition may repeat an identical procedure up to three
  total attempts. A multi-attempt session manager must receive clients
  configured for one bounded connection attempt, so retries are not multiplied;
  the caller supplies the exact transport-only exception taxonomy. Functional
  or ambiguous HID delivery is never replayed.
- A lifecycle mutation invalidates session authority. The next operation that
  requires authority acquires a fresh client/session; a human wait must not
  keep a HID lease alive indefinitely.
- Cleanup uses fresh sessions for independent safety-only steps: release held
  keyboard/mouse state once, select route none, hide USB, hide BLE, and inspect
  the final quiescent state. Main and cleanup results remain separate. Cleanup
  success cannot replace a main failure, and cleanup failure prevents an
  unconditional PASS.
- evdev paths are disposable. Rediscovery matches stable device identity and
  rejects ambiguity. `ENODEV`/`ENOENT` is accepted only at an explicitly
  expected retirement boundary; callers do not retain a stale descriptor.
- F24 value `2` is an autorepeat only between the same key's value `1` DOWN and
  value `0` UP. A repeat or fresh DOWN after release is stale replay and fails.
  The mouse checkpoint requires exact `REL_X=+1` followed by `SYN_REPORT`.
- `btmon` capture is bounded and never adds privilege escalation. Its raw file
  is retained at the caller-selected private evidence location; normal JSON
  contains only byte count, SHA-256, exit/stop state, duration, and focused
  counters.

## Evidence format

`s3-hidbot-qualification-evidence` version 1 uses sorted JSON keys. Its stable
top-level fields are `source`, `target`, `artifact`, `timing`, `stages`,
`invariants`, `route_checkpoints`, `bond_snapshots`, `input_devices`,
`hid_checkpoints`, `btmon`, `result`, and `failure_classification`. Optional
areas are `null` or empty until a scenario supplies them. Bond identities are
opaque IDs; unrelated host Bluetooth identities are excluded.

A minimal redacted preflight has this shape:

```json
{
  "artifact": null,
  "bond_snapshots": [],
  "btmon": null,
  "failure_classification": null,
  "hid_checkpoints": [],
  "input_devices": [],
  "invariants": [],
  "result": {
    "cleanup": {"classification": null, "status": "not_required"},
    "failed_parts": [],
    "main": {"classification": null, "status": "pass"},
    "overall": "pass"
  },
  "route_checkpoints": [],
  "schema": "s3-hidbot-qualification-evidence",
  "source": {"branch": "<branch>", "dirty": false, "revision": "<40-hex>"},
  "stages": [{"name": "source_identity", "status": "pass"}],
  "target": {"architecture": "esp32s3", "profile": null},
  "timing": {"duration_ms": 1, "started_at": "<UTC timestamp>"},
  "version": 1
}
```

Artifact comparison reports archive byte identity separately. Physical
qualification carry-forward requires exact source SHA, runtime ELF, actual
flashed payloads, flash offsets/settings/reset semantics, and partition
geometry. A different outer archive or non-runtime provenance payload alone
does not invalidate otherwise exact physical evidence.

## Intentionally deferred

U7.6D remains responsible for approved physical scenario composition: the
multi-bond/StoreFull campaign, any human-approved stale-host cleanup, route and
reconnect/retirement soaks, reset persistence, and raw evidence collection.
Broad stale operator-document consolidation remains U7.6C work.
