# Validation entrypoints

The repository-owned scripts below are the canonical local and CI validation
entrypoints. This document is authoritative for prerequisites, command
composition, and CI tiers; README and the contributor runbook link here
instead of copying the test recipes. CI invokes these scripts and does not
contain a separate hidden test recipe.

## Prerequisites

- Bash, Python 3.11 or newer, and a C/C++ toolchain providing `cc` and `c++`.
- `test-static.sh`, `test-host.sh`, `test-package.sh`, and `test-native.sh` do
  not require an activated ESP-IDF environment. `test-host.sh` requires
  network access to install the declared PyPI dependencies into its temporary
  venv. `test-package.sh` additionally downloads the temporary `build` and
  `twine` tools and resolves the package dependency while validating fresh
  wheel/sdist environments.
- `test-control-protocol.sh`, `test-firmware.sh`, and `test-nonhardware.sh`
  require an activated ESP-IDF v5.5.4 environment. Keep activation paths in
  local shell configuration; do not add them to this repository.
- The ESP-IDF project root is `firmware/`.
- `test-hardware-hid.sh` runs the U5.4.1/U5.4.2 Linux HID
  observer/discovery and F24 orchestration unit tests without touching
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
host tests from the extracted sdist. It is a validation-only command; it never
uploads or publishes an artifact.

## Entrypoints

Run these commands from the repository root:

```text
./tools/test-static.sh
./tools/test-host.sh
./tools/test-package.sh
./tools/test-native.sh
./tools/test-control-protocol.sh
./tools/test-firmware.sh
./tools/test-nonhardware.sh
./tools/test-hardware-hid.sh
./tools/run-hardware-hid.sh --hardware --keyboard --json
```

`test-static.sh` runs the protocol, default-HID-safety, HID-runtime, UART
writer, documentation static guards, and the U5.4.1 read-only observer unit
tests. `test-package.sh` is the release artifact validation entrypoint; it
requires network access but no ESP-IDF.
`test-native.sh` runs the three IDF-independent C++ suites.
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

`test-hardware-hid.sh` is the no-hardware CI/unit-test entrypoint for the
U5.4.1/U5.4.2 observer and F24 orchestration tests. With no arguments it
executes only the stdlib unit tests and remains hardware-free. The canonical
physical entrypoint is `run-hardware-hid.sh`; it performs the Linux-only,
read-only event-device discovery and bounded F24 transaction described in
`hardware-validation.md` after its temporary package installation. Physical
execution requires a separate hardware review gate and is never a CI command.

`run-hardware-hid.sh` is the canonical physical runner wrapper. It accepts the
same arguments as `hid_hardware_smoke.py` without adding `--hardware`; the
hardware opt-in therefore remains explicit. It installs `host/` into an
ephemeral virtual environment before invoking the runner, forwards the local
serial environment to the child process, and cleans up the environment on
success or failure. Its pip cache, version-check state, and build temporaries
are contained inside that temporary environment. It is not a CI command and
must not be invoked with
`--hardware` during no-hardware validation. Runner exit statuses are returned
unchanged; wrapper setup uses exit `70` for virtual-environment creation
failure and `71` for package-install failure.

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
- Tier C (`nonhardware.yml`, firmware job): the IDF-dependent protocol test
  and ESP-IDF v5.5.4 firmware build in the official
  `espressif/idf:v5.5.4` container.

The workflows run on pushes to every branch and on pull requests. They cancel
an older run for the same ref. Push refs (`refs/heads/*`) and pull-request refs
(`refs/pull/*`) produce distinct concurrency groups, so a push cannot cancel a
required pull-request run. No hardware workflow or artifact publishing is
part of this slice. The official IDF container is pinned by release tag; a
large mutable toolchain cache is intentionally deferred until its cache key
and invalidation policy can be tested against `dependencies.lock`.

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
- [`uart-control-plane.md`](uart-control-plane.md) — protocol/runtime
  contract.
