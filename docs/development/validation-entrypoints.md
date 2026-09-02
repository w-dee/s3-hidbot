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
prove queue overflow retires notification authority. These are included by
`test-native.sh`, `test-static.sh`, and therefore canonical
`test-nonhardware.sh`. An ESP-IDF target build remains mandatory because the
service database, mbuf allocation, and NimBLE notification API are
target-linked. None of these checks performs radio, serial, or hardware
access.

Run these commands from the repository root:

```text
./tools/test-static.sh
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
