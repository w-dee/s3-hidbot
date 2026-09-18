# rp-test qualification infrastructure v1

This is host infrastructure, not a physical qualification runner. None of its
CLIs enumerates serial interfaces, opens UART, invokes D-Bus adapter methods,
or captures radio/HCI traffic. `startup_probe.py` exposes injected adapters for
a separately authorized physical mission; its regression tests use fake peers.
Product firmware is not part of this infrastructure change.

## Install and warm use

Copy this directory privately to the Raspberry Pi test appliance. First setup:

```sh
sudo -n bash bootstrap.sh --initialize-host
sudo -n bash bootstrap.sh
python3 doctor.py --json
python3 preflight.py --profile reference --no-hardware --json
python3 run_capsule.py inventory
```

Bootstrap requires a normal sudo caller, Raspberry Pi 4 hardware, aarch64,
Debian/Raspberry Pi OS 12 or 13, and a root-owned, non-writable host marker at
`/etc/s3-hidbot-test-host`. Initial marker creation additionally requires
`--initialize-host`. A wrong existing marker is never overwritten. No username,
hardware serial number or address enters the marker or sanitized output.

`/srv/s3-hidbot-test` and its children `toolchains`, `cache/artifacts`,
`cache/tooling`, `private/references`, `runs`, `state`, `tmp` have mode 0700.
The dedicated login user owns the root and private state. The dedicated venv
is `toolchains/qualification`. Commands run with umask 077. Avoid running normal
doctor/cache/capsule commands through sudo. Bootstrap and the narrowly scoped
privileged HCI helper described below are the only repository-owned sudo
entrypoints in this directory.

The audited package set is Python/venv/pip, rsync, openssh-client, git,
coreutils, usbutils, udev, bluez and util-linux. These support the existing
Python serial client, esptool, artifact transfers/hashes, future USB discovery,
BlueZ user tools and flock. D-Bus access in existing helpers uses system tools;
no new Python D-Bus stack is needed. Bootstrap only installs missing packages;
APT index refresh is conditional on missing packages. It never upgrades the OS,
changes BlueZ configuration, or explicitly restarts Bluetooth.

`requirements.lock` pins esptool 4.12.0, pyserial 3.5 and their entire runtime
dependency set. Pip/check/import failures produce sanitized categories, never
raw stdout/stderr. The stamp records OS/package/Python/dependency versions,
lock/source digests and venv tree metadata. A warm mismatch returns
`TOOLCHAIN_CACHE_STALE` and requires explicit bootstrap repair. No installation
or network access occurs in doctor/preflight. Library bytecode caches are
excluded from the stamp; sources, binaries and distribution metadata are not.

## Content-addressed cache

Transfer an exact tree to a private `tmp` directory, then:

```sh
python3 cache.py import tooling MANIFEST_SHA256 --source PRIVATE_TRANSFER_DIR --files COUNT --bytes BYTES
python3 cache.py verify tooling MANIFEST_SHA256
```

Kinds are `artifacts`, `tooling`, `references`; the latter routes under
`private/references`. Cache keys are SHA-256 of the exact SHA256SUMS file.
Import rejects additional/missing files, duplicate manifest paths, absolute
paths, traversal, symlinks, hardlinks and special files. It verifies full
content, copies into a private sibling directory, re-verifies, fsyncs, seals,
and atomically publishes. `VERIFIED.json` is published only with complete data.
Existing digest entries are never overwritten. A valid cache hit skips copying.

Cache entry directories are 0700; sealed payload directories/files are 0500/0400.
Reference payloads remain 0700/0600. Runtime work copies belong in run capsules,
not sealed caches. Python imports can use `PYTHONDONTWRITEBYTECODE=1`.
Warm checks compare tree membership, modes, size, inode, mtime/ctime and owner
metadata. A changed/missing VERIFIED marker falls back to full SHA verification.
Bad bytes, missing files and invalid permissions fail closed. This is integrity
checking on a trusted appliance, not a signature against a malicious owner who
can replace both content and metadata. The expected digest comes from the mission.

`profiles.json` contains only environment-neutral `host-only` and `reference`
defaults. It contains no campaign artifact selector, runner digest, preserved
identity, private cache key, or recovery default. A separately authorized
mission may import exact content through `cache.py` and validate a mission-local
profile, but private identifiers and campaign digests stay outside the repository.
These warm-preflight profiles are a separate namespace from capsule profiles.
Capsule mission identifiers use the fixed `CAPSULE_PROFILES` allowlist in
`run_capsule.py`; a new capsule mission requires an explicit source allowlist
change, focused tests, review, and normal runner deployment. A name in one
namespace does not authorize or configure the other.
For a profile that uses a cached Python adapter, warm preflight can also verify
the required `hidbot` source modules inside that exact cache hit.
It does not assume that the persistent qualification venv contains the project
package, and it never imports the adapter or opens serial. A physical wrapper
must place that verified cached adapter root on its private import path before
constructing the client; package import failure is a tooling/configuration
failure, not evidence that the serial device was unavailable.
All CLI runs leave hardware checks `NOT_EXECUTED` and `mission_authorized=false`.
Missing assets classify `COLD_PREPARATION_REQUIRED`; a warm check never becomes
a cold build. Lock occupancy is informational in doctor; actual missions must
acquire the OS lock rather than trust a prior observation.

## Future physical mission integration

The authorized mission must follow this ordering:

```text
warm preflight -> exact cache/reference checks -> physical flock
-> run capsule and exact runner snapshot -> private serial resolution
-> bounded startup readiness -> exact installed identity -> product gates
```

Use `physical_run(...)` as the context manager before any hardware access. Pass
the exact source/package which will actually execute; execute the snapshot under
the returned run ID's `runner/`, not a later mutable source. Preserve its parser
and artifact digests in the manifest. A mission-specific wrapper remains required;
this infrastructure does not itself define or resume a campaign operation.

Serial identity, device-node readiness and port-open readiness are three
different facts. A stable private hardware identity may remain known while its
device node disappears during reset; a present node may still be transiently
unopenable; an open port has not yet proved protocol or firmware identity.
Physical wrappers must use `serial_readiness.py` and must not translate all open
errors to a missing peer. The resolver returns a private endpoint which is never
serialized. It is called again before every open attempt, so a stable alias may
disappear, reappear, or retarget after USB re-enumeration. Every failed/open peer
is closed before the next resolution and only one handle exists at a time.

Reset-driven readiness defaults to 12 seconds with 300 ms polling, consistent
with the existing bounded Client transactions (750 ms maximum per request).
An opportunistic already-running probe has a separate two-second deadline and
does not consume a reset budget. A profile whose durable start-state policy is
`application_not_running` skips that probe. Each actual reset consumes one of at
most two pre-side-effect startup attempts; serial/open/protocol retries inside a
deadline do not. No startup reset is allowed after a product side effect.

The expanded state path is STARTUP_BEGIN, SERIAL_IDENTITY_RESOLVED,
SERIAL_DEVICE_WAIT, SERIAL_OPEN_ATTEMPT, SERIAL_PORT_OPEN, PROTOCOL_SYNC,
HELLO_READY, IDENTITY_VERIFIED, with RESET_REQUESTED prepended only after a
budgeted reset. ENOENT/ENODEV-style disappearance and a proven pre-protocol busy
open are bounded transient failures. Access denial, unsupported hardware,
identity ambiguity, unsupported termios and genuinely unknown wrapper failures
are terminal and fail immediately. A first hello timeout, stale frame, partial
frame, or synchronization disconnect closes the peer and restarts from private
identity resolution. A late response cannot bypass the deadline.

The outer exception is not treated as the root cause. The central normalizer
walks at most eight exception objects through explicit `__cause__`, otherwise
implicit `__context__`, detects cycles and excessive depth, and reads only
allowlisted class names plus structured `errno`/portable `winerror`. It never
reads or serializes exception messages, `args`, traceback text, paths or reprs.
Thus a generic wrapper around ENOENT remains transient `PATH_NOT_PRESENT`, while
the same wrapper around EACCES is terminal `ACCESS_DENIED`. ENODEV/ENXIO and
ESTALE are bounded disconnect/re-enumeration cases, and EBUSY/EAGAIN is a bounded
pre-protocol `DEVICE_BUSY_TRANSIENT`. Invalid device/configuration, missing
Python adapter code, chain cycles/depth overflow and a wrapper with no recoverable
root fail closed under distinct sanitized categories. A bare `SerialException`
is not automatically assumed transient.

An opportunistic transient serial failure continues re-resolution/open attempts
for its two-second window and, if still unavailable, permits reset attempt one;
it neither consumes reset budget nor terminates the mission early. A terminal
normalized root stops immediately. Reset-driven windows retain the full twelve
seconds, and open retries inside a window never consume another reset budget.
Mission wrappers use `startup_result_classification()` so failure before any
successful port open is `STARTUP_SERIAL_READINESS_TIMEOUT` (or a specific
terminal serial result), whereas an opened port that cannot synchronize is
`STARTUP_PROTOCOL_READINESS_TIMEOUT`.

`startup.json` durably stores only sanitized attempts: whether identity was
resolved, node/open/protocol attempt counts, outer/root allowlisted class,
wrapper depth, symbolic errno category, normalized serial category, chain
status, root-same/root-unknown state, transient/terminal disposition and startup
substage. It stores no endpoint, serial number, username, exception message,
arguments or traceback.

Physical startup wrappers use `serial_reset.application_hard_reset_once()` for
an application-start reset. The helper constructs pyserial with no endpoint,
sets DTR and RTS to the established application-boot release state before
assigning/opening the private endpoint, invokes esptool `HardReset` exactly
once, restores that release state, and closes. This ordering is required
because pyserial applies cached line values during `open()`, while esptool's
`HardReset` changes RTS only and inherits the current DTR state. The helper has
no discovery, retry, protocol, BLE, or product operation; callers inject the
pinned pyserial module and `HardReset` implementation and retain their existing
durable startup budget.
The older injected `startup_probe.readiness` remains a compatibility primitive;
new physical wrappers use the explicit serial acquisition boundary.

`OperationBudget` keeps read-only polling separate from side effects. A
capsule-backed budget durably records `SIDE_EFFECT_INVOKED` before calling the
callback and consumes the normalized operation even if the callback throws or
its result is lost. The marker means the side effect may already have happened,
so it must not be issued automatically again in that run/resume lineage.
Bond-delete aliases share one consumed entry. Restarted controllers must use
`resume_budget(...)` from the same durable capsule while holding the physical
lock, never create a fresh budget to retry an uncertain invocation. Startup
attempts are likewise consumed before their action, are capped at two, and are
not available after a product side effect.

This is conservative replay suppression, not exactly-once physical execution.
The generic ledger does not persist callback entry, callback return, or
per-operation success/failure. A persistence failure prevents the callback from
being invoked; after any uncertain persistence or execution boundary, authority
must be reconstructed from the same capsule rather than inferred from the old
in-memory object. Richer intent/reservation/return/outcome state and separate
product/containment verdicts are campaign contracts, not generic appliance APIs.

## Capsules, retention, purge and export

Run IDs are UTC timestamp plus random suffix. Creation snapshots the exact runner,
computes per-file SHA-256 and an aggregate manifest digest, and fsyncs private
manifest/ledger/retention/checksum files before yielding to a mission. Interrupted
creation may leave an ACTIVE incomplete capsule; it cannot pass validation or be
purged. Investigate and explicitly resolve its state. Physical lock release is
automatic on process exit; stale PID metadata is never the lock authority.

Capsules contain `manifest.json`, `ledger.json`, `startup.json`, `result.json` when committed,
`handoff.json`, `retention.json`, `runner/`, `checksums/`, `evidence/`, `scratch/`.
Files are 0600, directories 0700. `retain_raw` explicitly copies authorized raw
input into canonical `raw-hci`, `raw-uart`, `raw-nvs` evidence files and verifies
hash equality; it never prints the contents. A capture adapter writing directly
must enforce the same permissions and fsync before acknowledging completion.
Structured exceptions omit messages/locals and retain only reviewed source
basenames, narrow class/function/category/stage fields and line numbers. Raw
stdout/stderr must not be inserted into ledger/result.

Results are committed explicitly once using atomic replace, file fsync and
directory fsync. Normal exception unwinding through `physical_run(...)` marks an
ACTIVE run UNRESOLVED when possible. Abrupt process death can leave an ACTIVE
capsule without `result.json`; the generic tooling does not guarantee terminal
result synthesis after process death. Such a capsule requires inspection and an
explicit retention transition, and its consumed operations remain unavailable
for automatic replay.
Handoff RESULT_COMMITTED -> RETRIEVED -> ACKNOWLEDGED is separate from retention.
Retention is ACTIVE -> UNRESOLVED/RESOLVED -> PURGE_ELIGIBLE, with an explicit
UNRESOLVED -> RESOLVED transition. There is no expiry or automatic purge.
Ordinary cleanup removes only `scratch/` contents. All exact runners, raw evidence,
ledger, result and forensic metadata remain even after ACK.

The `runner/` snapshot is immutable evidence and is never a writable Python
cache. Creation rejects `__pycache__`, `.pyc` and `.pyo` inputs and records the
exact per-file and aggregate digest. Missions execute a digest-equal scratch copy
through `runner_execution(...)`, which sets Python bytecode writes off for the
execution lifetime. Runtime caches and generated files belong only under
`scratch/`. `checksums/execution.json` attributes the execution copy to the exact
snapshot, while `checksums/post-run-runner.json` requires unchanged content and
membership at completion. The physical context marks an ACTIVE capsule
UNRESOLVED and fails if any snapshot file is added, removed or changed; ignoring
bytecode during verification is not an accepted substitute.

Raw evidence presence and payload are separate facts. In particular,
`RAW_UART_RETAINED=true`, `RAW_UART_BYTES=0` and `RAW_UART_HAS_DATA=false` means
capture was armed before open but no UART byte was observed. The zero-byte file
is valid forensic state and is retained under the same no-automatic-purge policy.

## Privileged HCI evidence boundary

Version 4 retains historical v1/v2/v3 capsules and installations. The full contract
is in [`evidence-authority.md`](evidence-authority.md). The future root service
accepts a run ID and strict request, never a caller path. Its fixed state root
is `/var/lib/s3-hidbot-authority-v4`; protected installation and runtime bundles
live under `/usr/local/lib/s3-hidbot-authority-v4`. The v3 roots remain immutable
historical authority for their consumed attempts.

`evidence_pipeline.py` invokes `/usr/bin/sudo -n /usr/bin/python3 -I -S -B` with
the fixed service path and controlled environment. Raw descriptors stay in the
unchanged Producer. A single immutable Attempt Authority Snapshot binds the
runtime, coordinator, plan, helper, contract, product and capture. Root persists
test outcome and evidence before verifying the ordinary metadata stage. It
copies verified metadata into a private root-owned package, hashes exact final
file objects, and publishes sibling `commit.json` last. SUCCESS requires PASS
test plus FINALIZED evidence. An attempt begins with an empty evidence set;
Q8 activates its HCI object at `package_request()`. A terminal failure before
that boundary has a valid FINALIZED empty set and therefore reports
`TEST_FAILED`. The ordinary user cannot mutate final files.

The maintained coordinator and Q8 live in `tools/qualification_campaign/`.
`prepare_bundle.py` creates a new measured source-only bundle from the retained
audited phase snapshot. The explicit `install_authority_runtime.py --bundle
BUNDLE` step verifies and installs it before any attempt. No runtime installs
or upgrades privileged code. Code or FROZEN replacement after start is rejected
against the original journal; bytecode is prohibited, and imports execute
measured protected source in isolated Python.

The passive appliance-only check uses the installed runtime ID:

```sh
/usr/bin/python3 -I -S -B /usr/local/lib/s3-hidbot-authority-v4/runtime_launcher.py \
  RUNTIME_ID evidence_rehearsal.py - \
  --evidence-pipeline-rehearsal --not-qualification
```

It is always `EVIDENCE_PIPELINE_REHEARSAL / NOT_QUALIFICATION`, using the same
Q8 capture boundary and coordinator terminal function as a future attempt.
It does not pair, run HID workloads, change bonds, or touch firmware/NVS.
`evidence_resume.py RUN_ID`, through the same launcher, finalizes only an
already recorded test result; it never reruns qualification phases. The ordinary
`retain_raw` path must not ingest root-owned bytes. There is no automatic purge.
The commands below concern ordinary run capsules, not this separate
root evidence store.

```sh
python3 run_capsule.py mark RUN_ID RESOLVED
python3 run_capsule.py mark RUN_ID PURGE_ELIGIBLE
python3 run_capsule.py purge-run RUN_ID
```

ACTIVE always refuses purge. UNRESOLVED refuses by default and requires an
explicit `--override-unresolved`. RESOLVED must first be marked PURGE_ELIGIBLE.
Purge acquires the physical lock and capsule lock, accepts exactly one validated
run ID, checks confinement and rejects symlinks throughout. No globs/broad roots.

`export RUN_ID PRIVATE_DESTINATION` is an explicit local-filesystem copy only;
it does not archive, upload or choose a remote destination. Destination must not
exist, its parent must be private, and symlink traversal is rejected. Active runs
cannot be exported. SHA-256 is checked before/after and 0700/0600 is preserved.
Copy failure never deletes the original capsule.

The disk threshold is 2 GiB: substantially more than the approximately 40 MiB
current artifact set, with room for multiple bounded UART/HCI captures and build
transfers. A mission planning larger captures must require more. Low space returns
FORENSIC_STORAGE_LOW; unresolved runs are never reclaimed automatically.
Doctor/preflight timings are monotonic. Warm doctor target is 10 seconds, warm
preflight 60 seconds; slowest phase is always reported. Physical pre-operation
latency above 120 seconds requires a phase breakdown, not product-failure labeling.

## Standing policies

1. rp-test is a persistent but disposable qualification appliance.
2. Required host tooling may stay installed between runs.
3. Exact functional artifacts/tooling may remain cached between runs.
4. Private forensic run capsules may remain on rp-test.
5. UNRESOLVED evidence is never automatically deleted.
6. RESOLVED evidence remains until explicit purge.
7. Authorized missions may retain raw HCI/UART/NVS privately on rp-test.
8. Every physical run retains its exact runner; a terminal result is durable
   when explicitly committed, while abrupt death may leave an ACTIVE run.
9. Read-only/idempotent protocol operations may use bounded retry.
10. Pair/Connect/delete/flash/NVS mutation use explicit budgets, never automatic retry.
11. A mission may permit at most two startup/reset attempts before any side effect.
12. Exact qualification firmware may remain across related retries when state is understood and contained, with no unsafe pending recovery. Explicit final restore/milestone-end/recovery requirements still apply.
13. Warm preflight installs/downloads/builds/reconstructs nothing.
14. Warm preflight target is at most 60 seconds.
15. Physical pre-operation latency above 120 seconds requires timing diagnosis.
16. Private evidence must never be pasted into chat, GitHub or repository content.
17. Exception reporting stays sanitized even with private forensic retention.

Run `./tools/test-rp-test-infra.sh` from the repository root. It is host-only;
fresh-image package installation is tested with fakes/static guards, not claimed
as a real fresh-image run. Existing qualification harness and Client tests cover
framing/privacy foundations. No firmware build or physical test is required here.
