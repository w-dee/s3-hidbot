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
- `tools/rp-test/privileged_evidence.py` implements the separate version 2
  root-owned capture boundary. The fixed installation and root, pinned directory
  and capture descriptors, immutable capture journal and strict receipts are
  documented in [`evidence-authority.md`](../../tools/rp-test/evidence-authority.md).
  `evidence_pipeline.py` publishes a final commit only after metadata and modes
  are durable. Ordinary code never reads or changes root raw evidence.
- `tools/qualification_campaign/official_campaign.py` and `q8_host_security.py`
  are the maintained future official coordinator and Q8 integration. Their
  bundle preparation freezes the remaining audited phase snapshot, plan and
  exact boundary source. Preparing a bundle does not authorize qualification.
- `tools/qualification_campaign/official_preflight.py` is the protected
  nonqualification start gate. It verifies the running identity, performs one
  controlled application reset even from an already-strict state, reacquires a
  fresh session, and requires the strict boot default plus route, HID, BLE, USB,
  bond and host safety postconditions before an attempt package can be created.
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

Possession of SSH configuration, `known_hosts`, cached routes, routing
availability, prior access, or historical tooling state does not authorize a
remote qualification endpoint or jump route. Every endpoint and route requires
explicit authorization for the current task.

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
- The reusable `qualification_harness.btmon` capture is bounded and
  unprivileged. Its raw file is retained at the caller-selected private
  evidence location; normal JSON contains only byte count, SHA-256, exit/stop
  state, duration, and focused counters. A campaign requiring root-owned HCI
  bytes must instead use the rp-test privileged boundary. Its ordering is
  writer stop, confirmed reap, stable privileged stat/hash, receipt publication,
  then unprivileged manifest/result/index packaging.

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

## Campaign contracts versus reusable infrastructure

An accepted asynchronous `ble.enable` response establishes Stage A only:

```text
desired=exposed
observed=enabling
stack_ready=false
recovery_required=false
last_error=null
```

Qualification then uses bounded read-only `ble.exposure.status` polling to
establish convergence and advertising readiness. It must not require immediate
`stack_ready=true` from the accepted mutation response.

Connection-scoped `encrypted`, `authenticated`, and `bonded` flags may retire
on disconnect. They cannot replace persistent bond inventory when establishing
retained-bond authority after disconnect.

Campaign runners may require durable operation intent, invocation reservation,
returned-plus-outcome state, ambiguity classification, product verdicts, and
continuation lineage. Those are campaign contracts, not guarantees of the
generic `tools/rp-test` `OperationBudget`. In particular, the durable product
retained-reconnect verdict and the stability/final-containment/full-smoke
verdict are separate; product PASS is not erased by a later containment
failure. Sealed campaign runners remain functional artifacts rather than
maintained generic tooling.

## BLE continuity cleanup and engineering rehearsal

`qualification_harness.run_ble_cleanup` consumes the caller's existing valid
attempt session. It keeps that session through workload completion, correlated
explicit release, exact host ALL_UP proof, a bounded quiet tail, and intentional
route-none. Same-session ping checks refresh and validate authority before retirement.
Route selection retires setup authority before the workload session starts;
accepted route-none intentionally ends that attempt session. A fresh session
is allowed after this retirement boundary for final lifecycle cleanup/status. Observer callbacks must fit within the lease. No fresh hello or
mutation replay is allowed to rescue this continuity evidence. In particular,
`SessionLostError` may follow an executed mutation; it is not evidence of
non-admission. Any ambiguous result fails continuity and enters separate safe
recovery. Recovery success never converts the original failure to PASS.

Observer ENODEV/ENOENT is expected only for the exact device after an accepted
intentional retirement boundary. It never substitutes for pre-retirement
ALL_UP and quiet-tail evidence. Arbitrary I/O errors, SYN_DROPPED, held input,
wrong device identity, and premature disappearance fail closed. Final control
state must establish stable route-none, no active sequence, ALL_UP, disconnected
BLE and the explicitly required terminal lifecycle state.

The opt-in `tools/ble_cleanup_rehearsal.py` exercises this sequence with no normal
keyboard, mouse, or Sequence workload. It requires an existing retained strict
bond, headless host, exact artifact archive SHA256 and source revision, verified
runtime identity and exact BLE keyboard/mouse evdev observers. EVIOCGKEY checks
include input held before the observer opened. Its result is always classified
`CLEANUP_REHEARSAL_ONLY / NOT_QUALIFICATION`; it consumes no official physical
qualification attempt and makes no report-delivery qualification claim.

A separately authorized physical wrapper must hold the appliance physical lock
and execute a hashed runner snapshot. With those prerequisites established, the
runner's explicit invocation is:

```sh
python3 tools/ble_cleanup_rehearsal.py \
  --hardware --cleanup-rehearsal-only --not-qualification \
  --artifact "$ARTIFACT" --expected-source "$SOURCE_REVISION" \
  --expected-artifact-sha256 "$ARTIFACT_SHA256" --evidence "$EVIDENCE"
```

No artifact or campaign identity is hardcoded into the reusable runner. It
checks the expected archive before inspecting or accessing hardware. The
canonical qualification-harness suite tests session loss after mutation,
identity replacement, observer loss on either side of retirement, exact
ordering, kernel held-state inspection, and the production rehearsal adapter's
same-session handoff using fakes.

## Intentionally deferred

U7.6D remains responsible for approved physical scenario composition: the
multi-bond/StoreFull campaign, any human-approved stale-host cleanup, route and
reconnect/retirement soaks, reset persistence, and raw evidence collection.
Broad stale operator-document consolidation remains U7.6C work.
