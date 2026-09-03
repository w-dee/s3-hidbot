# Validation entrypoints

The repository-owned scripts below are the canonical local and CI validation
entrypoints. This document is authoritative for prerequisites, command
composition, and CI tiers; README and the contributor runbook link here
instead of copying the test recipes. CI invokes these scripts and does not
contain a separate hidden test recipe.

## Prerequisites

- Bash, Python 3.11 or newer, and a C/C++ toolchain providing `cc` and `c++`.
- `test-static.sh`, `test-host.sh`, `test-package.sh`, `test-host-artifact.sh`, and `test-native.sh` do
  not require an activated ESP-IDF environment. `test-host.sh` requires
  network access to install the declared PyPI dependencies into its temporary
  venv. `test-package.sh` additionally downloads the temporary `build` and
  `twine` tools and resolves the package dependency while validating fresh
  wheel/sdist environments.
- `test-control-protocol.sh`, `test-firmware.sh`, and `test-nonhardware.sh`
  require an activated ESP-IDF v5.5.4 environment. Keep activation paths in
  local shell configuration; do not add them to this repository.
- The ESP-IDF project root is `firmware/`.
- `test-hardware-hid.sh` runs the U5.4.1-U5.4.3 Linux HID
  observer/discovery and keyboard/mouse smoke orchestration unit tests without touching
  `/dev/input`, sysfs, serial, or USB. A physical observer/smoke run is a
  separate, explicit `./tools/run-hardware-hid.sh --hardware` operation and
  is never part of CI. The physical wrapper creates a temporary virtual
  environment, installs `host/` as a normal package (including declared
  dependencies), forwards machine-local environment variables unchanged, and
  removes the environment on exit. Dependency retrieval may require network
  access; the host Python environment, pip cache, and pip build temporaries
  are not persistently modified.

The host entrypoint creates a temporary virtual environment, stages the
`host/` package in a temporary directory, installs it as a normal distribution
(including its declared runtime dependency), checks imports and
`hidbotctl --help`, and runs the unittest suite. It does not use the caller's
`PYTHONPATH` or site-packages and does not leave packaging metadata in the
repository source tree.

The package entrypoint builds wheel and sdist artifacts from temporary copies,
checks their metadata with `twine`, verifies their allowlisted contents and
privacy, installs each artifact into a fresh virtual environment, and runs the
host tests from the extracted sdist. It also proves that the base wheel remains
esptool-free and that the optional `flash` extra installs a supported esptool
for a version/help smoke without opening a serial port. It is a validation-only
command; it never uploads or publishes an artifact.

## Entrypoints

U7.4A extends deterministic `tools/test-hid-control-executor.sh` and
`tools/test_ble_hid_service_static.py` coverage. They verify generation-owned
CCCD and Control Point state, combined security readiness, bounded callback
capture, exact internal report payloads, no retry, and that the notification
adapter remains unreachable from the public route. The focused BLE security
and executor suites also interleave immediate store-failure inhibition with
stale healthy verification, exercise generation/connection-handle reuse, and
prove queue overflow retires notification authority. They additionally prove
that a disable request changes no compound security state before executor
processing while public readiness is already false, and deterministically
order fatal-store delivery after disable or disconnect retirement. Those fatal
events still commit global recovery faults and block re-advertising, whereas a
stale StoreFull remains local and cannot poison a future same-handle peer. A
static guard rejects compound-security mutation from the disable request path
or direct production owners outside the BLE backend wrapper. The boot-lifetime
fatal-store latch is reconciled before generation-scoped overflow handling at
every executor processing boundary. Deterministic SMP-order tests dequeue an
action, advance disable generation, and then resume executor processing to prove
that a lost detailed fatal event still commits one idempotent global fault;
queue-full disconnect, no-connection fatal, repeated fatal, and StoreFull
contrast cases cover the other fallback boundaries. Authority-scoped overflow
tests cover current-then-stale, stale-then-current,
multiple-current, same-handle authority reuse, and a deterministic
producer/consumer CAS interleaving, including generation-zero wrap. Dropped
CCCD-disable and Control Point
Suspend events inhibit readiness immediately, before the executor performs the
single idempotent recovery fault. A stale producer can neither replace a
current sticky token nor poison a newer lifecycle generation. Reset/Sync
handoff tests fill the final queue slot with the retired-generation Reset, drop
the post-Reset Sync, and prove the monotonic handoff-failure latch reaches a
recovery fault rather than permanent enabling. Normal, stale, repeated-reset,
dropped lifecycle-timeout, timeout-ownership, and generation-wrap cases are
also guarded. Dropped-Connect
tests prove immediate exact-handle orphan termination, no false peer adoption,
harmless orphan Disconnect processing, and retained logical fail-close when
the termination API rejects the request. The target/static contract fixes
production cardinality at one BLE connection and verifies the ESP-IDF v5.5.4
callback-side termination call remains direct and non-deferred. The suites also
use native hooks to pause failed Sync, current overflow, dropped Connect,
and adopted-peer producers until after the executor's old final queue-drain
boundary. They then prove that the independent retained wake alone commits the
sticky failure without unrelated traffic. Wake-before-wait, blocked/idle,
active-executor, and coalesced lifecycle-plus-generic cases cover the remaining
lost-wakeup windows and reconciliation priority. Static source guards verify
arm-before-terminate ordering, immediate and delayed Disconnect completion,
synchronous initiation failure cleanup, `BLE_HS_EALREADY` retention, and
exact-purpose protection from stale Disconnect cancellation. The dedicated
`tools/test-ble-lifecycle-watchdog.sh` native suite exercises the production
lock-free ownership protocol directly: cross-purpose and same-purpose arm
conflicts, exact arm-failure release, and mutually exclusive cancel/timeout
claims. Executor regressions verify that StoreFull, pairing-timeout, SMP, and
repeat-pairing teardown failures terminalize lifecycle recovery without turning
StoreFull into persistent storage corruption. These checks
are included by `test-native.sh`, `test-static.sh`, and therefore canonical
`test-nonhardware.sh`. An ESP-IDF target build remains mandatory because the
service database, mbuf allocation, and NimBLE notification API are
target-linked. None of these checks performs radio, serial, or hardware
access.

U7.4B extends the same executor and static entrypoints with internal BLE route
activation, exact HID/route/BLE/connection/characteristic/ticket fencing,
single-transport dispatch, callback-side readiness-loss preemption, failure
retirement, reconnect/no-auto-restore behavior, and relative-mouse no-replay
coverage. Public protocol, host, and CLI static guards continue to require the
route-v1 value set to remain exactly `none|usb` and reject route v2.

U7.4C extends those entrypoints with the exact BLE-to-none safety transaction:
normal-authority revocation, one Keyboard and Mouse all-up attempt, dedicated
100 ms grace ownership, queue-full retained wake, the existing disconnect
watchdog path, exact Disconnect completion, stable-none publication, and
cross-transport switching only through stable none. Native cases cover stale
timer/action fencing, early and racing Disconnect, same-handle reconnect,
CCCD/Suspend/security/storage/overflow loss, disconnect-initiation failure,
lease retirement, release-epoch ticket exclusion, and no automatic route
restore. Static guards keep the public route-v1 vocabulary at `none|usb`,
verify the dedicated timer, exact no-Report-ID payload sizes, and absence of a
blocking grace delay. None of these checks accesses radio or hardware.

U7.4D adds route-v2 protocol, executor activation, host negotiation, v1
fallback, CLI BLE selection, strict v2 parsing, retry/session, stable-none,
truthful releasing-status, and v1-under-BLE compatibility coverage. Route-v1
remains exactly `none|usb`; v2 adds `ble` and is preferred only when its
capability is advertised. These checks remain software-only.

The bonded GATT-cache regression extends the executor and BLE HID static
entrypoints with legacy missing-revision migration, stale-cache readiness
inhibition, exact per-connection Service Changed eligibility and bounded call
count, authoritative Report Map read ordering, write-and-reread persistence,
fresh/current reconnect behavior, generation/handle fencing, disconnect,
missing subscription, send failure, StoreFull/persistent-storage failure, and
queue-overflow cases. The static test binds schema revision 1 to a SHA-256
fingerprint covering the Report Map, Report References, epoch UUIDs/value, and
handle topology, and checks the real 0x2A4B access path publishes the exact
generation/connection/attribute evidence only after a successful read. The
separate topology guard models the ESP-IDF v5.5.4 sequential registration:
GATT remains `0x0006..0x000d`, the three-attribute revision-1 epoch occupies
`0x000e..0x0010`, and HID moves to `0x0011..0xffff`. It also checks that legacy
keyboard/mouse CCCD value handles cannot alias current notifiable handles and
that the configured persistent capacity covers all three migrated bonds.
Executor coverage injects both old-handle and current-handle RESTORE evidence;
old handles remain ineligible and an exact current Report Map read is still
required before persistence/readiness. These tests do not access Bluetooth
radio, BlueZ, serial, or hardware.

U7.5B bond-administration focused coverage extends the control-protocol,
executor, host, CLI, and static entrypoints. It checks empty/one/three-bond
schemas, deterministic opaque-ID ordering, strict exact removal input, normal
retry replay without a second mutation, active/advertising/pairing/BLE-route
rejection, USB-route independence, StoreFull/manual-capacity and fatal Storage
policy, exact NimBLE plus schema cleanup, post-read verification, unrelated
bond preservation, normal delete-all `BLE_HS_ENOENT` exhaustion versus genuine
delete failure, collision/partial-failure fail-close, and absence of public
key/address material. The ESP-IDF target build remains required for the real
NimBLE store and NVS callback linkage. These checks do not access Bluetooth
radio, BlueZ, serial, `.envrc`, or hardware.

The focused U7.5B teardown regression additionally distinguishes NimBLE
`BLE_HS_ENOTCONN` from an initiation failure only for an exact current
security-teardown generation and connection. It exercises ordinary success,
other-error fail-close, stale generation, wrong handle, late Disconnect,
numeric-handle reuse, route retirement, non-storage stale-key failure, and a
genuine persistent-store failure. Other disconnect callers retain their
existing nonzero-result policy.

Run these commands from the repository root:

```text
./tools/test-static.sh
./tools/test-ble-lifecycle-watchdog.sh
./tools/test-host.sh
./tools/test-package.sh
./tools/test-native.sh
./tools/test-control-protocol.sh
./tools/test-firmware.sh
./tools/test-firmware-artifact.sh
python3 tools/test_release_contract.py
python3 tools/test_release_firmware_equality.py
python3 tools/test_release_workflows.py
./tools/test-nonhardware.sh
./tools/test-hardware-hid.sh
./tools/run-hardware-hid.sh --hardware --keyboard --json
./tools/run-hardware-hid.sh --hardware --mouse --json
```

`test-static.sh` runs the protocol, default-HID-safety, HID-runtime, UART
writer, documentation static guards, and the U5.4.1 read-only observer unit
tests. `test-package.sh` is the release artifact validation entrypoint; it
requires network access but no ESP-IDF.
`test-native.sh` runs the IDF-independent C++ suites, including the deterministic
sensitive-request HMAC layout, key-failure, digest-comparison, and memory-wipe
checks.
`test-control-protocol.sh` additionally compiles against the active ESP-IDF
cJSON source and therefore requires the v5.5.4 environment.

`test-firmware.sh` verifies the active IDF version, the locked IDF/component
metadata, and target `esp32s3`, then runs the canonical firmware command:

```text
cd firmware
idf.py build
```

The existing `sdkconfig.defaults` and `dependencies.lock` provide the target
and dependency source of truth; no `set-target` step is required for a fresh
checkout. Build output remains ignored and must never be tracked.

`test-firmware-artifact.sh` is the focused U6.3A artifact contract entrypoint.
It requires an activated ESP-IDF v5.5.4 environment, runs stdlib-only
manifest/verifier/privacy tests, and performs two independent temporary
artifact builds to verify byte-for-byte reproducibility. It does not use or
modify the ordinary `firmware/build` directory and removes all temporary
outputs when it exits.

The three release helper tests are stdlib-only and run from
`test-static.sh`. They validate strict firmware/host release versions and
derived names, annotated-tag peeling, candidate/tag firmware exact-equality
handling, Actions permissions/triggers/action pins, and immutable draft-run
correlation. They never create a tag, call GitHub APIs, upload assets, or
access hardware.

`test-host-artifact.sh` is the focused U6.4A canonical host-wheel producer
test. It builds a single pure-Python wheel from a temporary staging copy of
`host/`, writes a matching project-owned SHA-256 file, and validates the exact
two-file output. It does not publish an artifact or modify repository package
build directories.

`test-hardware-hid.sh` is the no-hardware CI/unit-test entrypoint for the
U5.4.1-U5.4.3 observer and keyboard/mouse smoke orchestration tests. With no arguments it
executes only the stdlib unit tests and remains hardware-free. The canonical
physical entrypoint is `run-hardware-hid.sh`; it performs the Linux-only,
read-only event-device discovery and bounded F24 transaction described in
`hardware-validation.md` after its temporary package installation. Physical
execution requires a separate hardware review gate and is never a CI command.

`run-hardware-hid.sh` is the canonical physical runner wrapper. It accepts the
same arguments as `hid_hardware_smoke.py` without adding `--hardware`; the
hardware opt-in therefore remains explicit. With `--hardware --keyboard` it
runs the F24 keyboard smoke, and with `--hardware --mouse` it runs the one-shot
relative `REL_X=+1` mouse smoke. It installs `host/` into an ephemeral virtual
environment before invoking the runner, forwards the local serial environment
to the child process, and cleans up the environment on success or failure. Its
pip cache, version-check state, and build temporaries are contained inside
that temporary environment. It is not a CI command and must not be invoked
with `--hardware` during no-hardware validation. Runner exit statuses are
returned unchanged; wrapper setup uses exit `70` for virtual-environment
creation failure and `71` for package-install failure.

`test-nonhardware.sh` is the complete A-D suite (static, privacy, host,
package artifacts, IDF-independent native, and IDF-dependent protocol
validation). It requires an active ESP-IDF environment because the protocol
test is IDF-dependent, and also runs `git diff --check`. Firmware build is
kept as the separate E entrypoint.

All tracked shell scripts under `tools/` have executable mode and are invoked
as `./tools/name.sh`. The wrappers call the existing focused scripts instead
of duplicating their test logic.

## CI tiers

- Tier A (`privacy-lint.yml`): static guards and privacy unit/tracked scans.
- Tier B (`nonhardware.yml`): clean host install/package validation on Python
  3.11 and 3.12, plus one IDF-independent native C++ job.
- U6.4A adds a non-matrix Python 3.12 producer to that workflow. It uploads a
  temporary `host-package` Actions artifact containing only the canonical
  wheel and its project-owned SHA-256 checksum, retained for 14 days. A
  checkout-free Python 3.11/3.12 consumer matrix verifies that checksum before
  installing the downloaded wheel in a fresh virtual environment. The wheel is
  source-checkout-independent; the sdist remains validated by `test-package.sh`
  but is not uploaded. This is neither PyPI publication, a GitHub Release, a
  permanent release asset, nor signed provenance. The checksum establishes
  matching wheel bytes alongside the download; it is not a signature or an
  independent authentication mechanism. Public release and distribution are
  reserved for U6.6.
- U6.4B2b/B2c adds a checkout-free Python 3.11/3.12 flash-extra consumer to
  the same workflow. Each job verifies the producer wheel checksum before
  installing `s3-hidbot-host[flash]`, proves the installed package origin, runs
  `python -m esptool version` and `hidbotctl flash-firmware --help`, then uses
  injected fake programming, UART, and Client objects to prove the installed
  post-flash orchestration reaches an identity `MATCH`. The consumer never
  invokes a repository helper or enables `--hardware`.
- Tier C (`nonhardware.yml`, firmware job): the IDF-dependent protocol test
  and ESP-IDF v5.5.4 firmware build in the official
  `espressif/idf:v5.5.4` container.
- Dedicated firmware artifact workflow (`firmware-artifact.yml`, U6.3B): a
  clean ESP-IDF v5.5.4 `esp32s3` container build with an immutable image
  digest, explicit source revision and `SOURCE_DATE_EPOCH`, two independent
  artifact builds, official verification, byte-identical comparison, privacy
  checks, and one temporary Actions artifact upload. It does not flash
  hardware or publish a GitHub Release.
- Release preparation (`release-build.yml`, U6.6A): manual exact-commit
  candidates and `v*` tag pushes build temporary `release-assets` only, with
  `contents: read`. It reuses the canonical firmware and host builders,
  compares two firmware builds byte-for-byte, verifies the exact asset set,
  and uses checkout-free Python 3.11/3.12 consumers. It never creates a
  Release. The separate manual `release-draft.yml` has the narrowly necessary
  `contents: write` and `actions: read` permissions; it accepts explicit
  successful candidate and tag-build run IDs for the same tagged commit,
  requires their firmware archives to be exact bytes, and creates an
  unpublished draft only.

The workflows run on pushes to every branch and on pull requests. The artifact
workflow also supports manual `workflow_dispatch` runs. They cancel
an older run for the same ref. Push refs (`refs/heads/*`) and pull-request refs
(`refs/pull/*`) produce distinct concurrency groups, so a push cannot cancel a
required pull-request run. No hardware workflow is part of this slice. The
dedicated artifact workflow pins its official IDF container by immutable image
digest and uploads temporary CI evidence only; public artifact publication is
not enabled. A large mutable toolchain cache is intentionally deferred until
its cache key and invalidation policy can be tested against `dependencies.lock`.

## CI guarantees and limits

CI guarantees that the tracked privacy/static checks, clean host install and
CLI tests, IDF-independent native tests, the IDF-dependent protocol tests, and
the ESP-IDF v5.5.4 `esp32s3` firmware build pass for the tested revision.

CI does not prove real USB enumeration, host HID delivery, physical lifecycle
behavior, mouse button/wheel/pan behavior, or long-duration hardware soak.
Those are separate gates documented in
[`hardware-validation.md`](hardware-validation.md).

## Related documents

- [`codex-runbook.md`](codex-runbook.md) — contributor procedure and review
  gates.
- [`hardware-validation.md`](hardware-validation.md) — physical safety and
  evidence scope.
- [`firmware-artifacts.md`](firmware-artifacts.md) — U6.3A artifact format and
  provenance contract.
- [`uart-control-plane.md`](uart-control-plane.md) — protocol/runtime
  contract.
