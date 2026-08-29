# Validation entrypoints

The repository-owned scripts below are the canonical local and CI entrypoints.
CI invokes these scripts; it does not contain a separate hidden test recipe.

## Prerequisites

- Bash, Python 3.11 or newer, and a C/C++ toolchain providing `cc` and `c++`.
- `test-static.sh`, `test-host.sh`, and `test-native.sh` do not require an
  activated ESP-IDF environment. `test-host.sh` does require network access
  to install the declared PyPI dependencies into its temporary venv.
- `test-control-protocol.sh`, `test-firmware.sh`, and `test-nonhardware.sh`
  require an activated ESP-IDF v5.5.4 environment. Keep activation paths in
  local shell configuration; do not add them to this repository.
- The ESP-IDF project root is `firmware/`.

The host entrypoint creates a temporary virtual environment, stages the
`host/` package in a temporary directory, installs it as a normal distribution
(including its declared runtime dependency), checks imports and
`hidbotctl --help`, and runs the unittest suite. It does not use the caller's
`PYTHONPATH` or site-packages and does not leave packaging metadata in the
repository source tree.

## Entrypoints

Run these commands from the repository root:

```text
./tools/test-static.sh
./tools/test-host.sh
./tools/test-native.sh
./tools/test-control-protocol.sh
./tools/test-firmware.sh
./tools/test-nonhardware.sh
```

`test-static.sh` runs the protocol, default-HID-safety, HID-runtime, and UART
writer static guards. `test-native.sh` runs the three IDF-independent C++
suites. `test-control-protocol.sh` additionally compiles against the active
ESP-IDF cJSON source and therefore requires the v5.5.4 environment.

`test-firmware.sh` verifies the active IDF version, the locked IDF/component
metadata, and target `esp32s3`, then runs the canonical firmware command:

```text
cd firmware
idf.py build
```

The existing `sdkconfig.defaults` and `dependencies.lock` provide the target
and dependency source of truth; no `set-target` step is required for a fresh
checkout. Build output remains ignored and must never be tracked.

`test-nonhardware.sh` is the complete A-D suite (static, privacy, host,
IDF-independent native, and IDF-dependent protocol validation). It requires
an active ESP-IDF environment because the protocol test is IDF-dependent, and
also runs `git diff --check`. Firmware build is kept as the separate E
entrypoint.

All tracked shell scripts under `tools/` have executable mode and are invoked
as `./tools/name.sh`. The wrappers call the existing focused scripts instead
of duplicating their test logic.

## CI tiers

- Tier A (`privacy-lint.yml`): static guards and privacy unit/tracked scans.
- Tier B (`nonhardware.yml`, host-native job): clean host install/tests and
  IDF-independent native C++ tests.
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
