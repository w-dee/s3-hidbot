# Firmware artifacts

This document defines the U6.3A local firmware artifact contract. It is a
development contract, not a release announcement: no public GitHub Release,
PyPI publication, or durable release artifact exists yet. The CI workflow does
upload temporary Actions artifacts as documented below; they are development
evidence rather than a stable public distribution channel.

## Builder inputs and isolation

`tools/build_firmware_artifact.py` requires explicit `--source-revision` (40
lowercase hexadecimal characters) and `--source-date-epoch` (a bounded,
non-negative integer). The builder does not invoke Git, inspect `.git`, or
infer a dirty state, so it also works from a source archive. The caller may
provide an immutable container reference; local builds record `null` rather
than inventing container provenance.

Every build uses a fresh temporary ESP-IDF build directory and combines the
normal `firmware/sdkconfig.defaults` with
`firmware/sdkconfig.artifact.defaults`. The latter enables
`CONFIG_APP_REPRODUCIBLE_BUILD=y` only for artifact builds. An existing
`firmware/build` directory is never an artifact input.

## Memory envelope and fixture profile

The firmware deliberately targets a conservative minimum envelope of **4 MiB
flash** and **no required external PSRAM**. `firmware/sdkconfig.defaults`
explicitly selects the ESP-IDF 4 MiB flash setting and disables external
SPIRAM, so a canonical artifact intentionally records `--flash_size 4MB`.
The current partition table ends well below that address-space limit; unused
flash is not consumed by placeholder partitions.

`build_profile=freenove-fnk0085` identifies the validated FNK0085 fixture and
its board-integration assumptions. It is not a claim that the physical fixture
has only the firmware minimum: the fixture may provide 8 MiB flash and 8 MiB
PSRAM. That additional capacity is intentionally unused by the current
firmware, which does not thereby claim compatibility with arbitrary
ESP32-S3 boards whose USB, UART, or GPIO topology has not been validated.

An artifact built before this explicit policy, with the inherited 2 MiB
ESP-IDF flash default, remains structurally verifiable historical evidence but
is not a positive target for the C4 flash gate. A new source revision and
canonical artifact are required for that gate.

## Bundle layout

The output is a deterministic `.tar.gz` with one top-level directory named:

```text
s3-hidbot-firmware-<version>-esp32s3-freenove-fnk0085/
```

The minimum payload is:

```text
manifest.json
SHA256SUMS
flasher_args.json
<generated application ELF>
<generated application BIN>
<generated bootloader BIN>
<generated partition-table BIN>
provenance/sdkconfig
provenance/dependencies.lock
LICENSE
```

All names are relative POSIX paths. Absolute paths, drive prefixes, empty or
`.`/`..` components, symlinks, and special files are rejected. The generated
`flasher_args.json` is authoritative for image paths and offsets; the verifier
checks that its target is `esp32s3`, its three image entries map to listed
payloads, and all paths remain inside the bundle.

## Manifest schema v1

`manifest.json` is strict and has exactly these top-level fields:

```text
artifact_manifest_version
project
firmware
runtime_identity
build
provenance
flash_plan
files
```

The firmware section records `version`, `protocol_version`, `source_revision`,
`target`, `build_profile`, and `idf_version`. The build section records
`reproducible: true`, `source_date_epoch`, nullable `container_image`, and
normalized compiler, CMake, Ninja, Python, and esptool versions. The provenance
section links the exact `dependencies.lock` and effective `sdkconfig` hashes.

`files` contains one entry per payload, with a SHA-256 digest and one bounded
role: `application_elf`, `application_bin`, `bootloader_bin`,
`partition_table_bin`, `flash_plan`, `effective_sdkconfig`,
`dependencies_lock`, or `license`. The manifest and checksum file are not
members of this self-hash set.

The runtime identity is intentionally linked to the exact linked ELF:

```text
manifest.runtime_identity.app_elf_sha256
== manifest.files[application ELF].sha256
```

`SHA256SUMS` contains `manifest.json` plus every manifest payload in lexical
path order, with each lowercase SHA-256 digest written in the fixed
`<digest>  <path>` format. It excludes only `SHA256SUMS` itself.

## Verification and reproducibility

The canonical stdlib-only artifact implementation is the installable
`hidbot.artifact` module. It verifies either an extracted bundle or its
archive, strictly validating schema fields, hashes, provenance links,
flash-plan mappings, path safety, archive traversal safety, and binary-safe
privacy markers (developer paths, serial configuration, `.envrc`, and obvious
secret markers). `tools/firmware_artifact.py` is only a source-tree adapter:
it directly loads that canonical module without importing `hidbot.__init__`,
so the existing standalone tools remain independent of pyserial and an
installed host package.

`verify_bundle_directory()` and `verify_bundle_archive()` return a deep copy
of the validated manifest mapping. `hidbot.firmware_verification` consumes
only that verified-manifest output to extract the runtime-comparable artifact
identity and compare it with validated `SystemInfo`. It does not repeat bundle
schema, hash, archive, or privacy validation and performs no serial I/O.
Comparison returns one of `MATCH`, `MISMATCH`, or `IDENTITY_UNAVAILABLE`.
The latter is used when the device does not advertise `firmware.identity-v1`
or reports a null source revision; a null revision is never a wildcard.

During a local build, the builder additionally checks the invocation's source
root, working directory, `HOME`, and `S3_HIDBOT_SERIAL` values in memory. These
machine-local values are never written into the manifest or payload.

`tools/test-firmware-artifact.sh` is the focused local entrypoint. It runs
synthetic verifier negative tests and builds two independent artifacts with
the same explicit inputs, then compares the ELF, binaries, flash plan,
manifest, checksums, and final archive byte-for-byte. Temporary build,
staging, pip, and archive paths are removed after the test.

## Dedicated CI artifact workflow

U6.3B adds `.github/workflows/firmware-artifact.yml`. It starts from a clean
checkout, derives the exact workflow revision from `github.sha`, and derives
`SOURCE_DATE_EPOCH` from that commit's timestamp. The job runs in the verified
immutable container
`espressif/idf:v5.5.4@sha256:b9f2d6ea1c19e0c9f7959bdb74a9e3c775642f9d0f3b841937c5fa3363db892b`.
The same container identity is passed to the canonical builder and recorded
in each manifest.

The workflow performs two independent clean builds with identical explicit
inputs, runs the official verifier on both bundles, compares the final
archives and every extracted payload byte-for-byte, and uploads exactly one
canonical archive plus its outer SHA-256 file as a temporary Actions artifact
with a 14-day retention period. The outer checksum covers the compressed
archive for transport integrity; the bundle's internal `SHA256SUMS` continues
to cover its manifest and payload files. Build or verification mismatch fails
the job before upload. No serial, flash, hardware runner, tag, GitHub Release,
or public package publication is involved. The archive is a development CI
artifact for version `0.1.0-dev`, not a released firmware download.

## Scope boundary

U6.3A does not publish public artifacts, add a flash helper, compare artifacts
with runtime UART identity, create attestations or SBOMs, or change firmware
versioning. U6.3B uploads only temporary CI evidence; it does not create a
public release. Attestation remains unimplemented. A release version and tag
are deferred to U6.6.

The local artifact contract remains valid when `build.container_image` is
`null`; only the dedicated CI workflow supplies immutable container
provenance.

## Artifact-only installed CLI verification

`hidbotctl verify-artifact ARTIFACT` is the installed host-wheel interface for
validating either a bundle archive or an extracted bundle directory before a
device is provisioned. It reuses the canonical `hidbot.artifact` verifier and
the verified-manifest identity conversion, so it performs no serial, USB, HID,
BLE, ESP-IDF, or source-checkout operation. A valid artifact exits 0 and
returns its runtime-comparable identity; missing, malformed, or unverifiable
input exits 2. `--json` emits one compact result object:

```text
hidbotctl --json verify-artifact firmware.tar.gz
```

`VALID` means that the artifact schema, payload/checksum integrity, declared
provenance relationships, flash-plan structure, and privacy/path contract are
internally valid. It does not mean the artifact is signed, publisher
authenticated, device authenticated, secure-boot trusted, or attested. This is
the first direct installed-wheel-to-firmware-archive bridge; it does not by
itself complete the broader U6.4 clean-room workflow.

## Runtime identity comparison

`hidbotctl verify-firmware ARTIFACT` is the canonical host-side comparison
entrypoint. It accepts either an ordinary bundle archive or an extracted bundle
directory. Before constructing a serial transport, it uses the canonical
artifact verifier and extracts `ArtifactFirmwareIdentity` only from the
verified manifest. It then uses one control connection and sends exactly
`protocol.hello` followed by `system.info`, validating the latter against the
advertised capabilities before calling the pure comparator.

The comparison reports `MATCH`, `MISMATCH`, or `IDENTITY_UNAVAILABLE` with a
fixed mismatch ordering. Match exits 0; a completed mismatch or unavailable
identity exits 7. Invalid artifact input exits 2 before any serial operation.
This command sends no HID report, does not query `usb.status`, and does not
call `hid.release_all`; the normal hello/info control-session lease is still
created and refreshed. Identity equality is provenance evidence, not device
authentication, cryptographic attestation, UART peer authentication, secure
boot, or signed firmware authenticity.

## U6.4B2b/B2c safe flash and post-flash verification

`hidbotctl flash-firmware ARTIFACT` is the explicit destructive programming
entrypoint for a verified archive or extracted bundle. It accepts only the
supported FNK0085 / ESP32-S3 / DIO / 4 MiB / 80 MHz provisioning plan produced
by `stage_and_verify_firmware_bundle()` and the unchanged
`plan_esptool_v4_args()` tuple. Staging and payload-integrity verification
happen before any process or serial access, and the staged payloads are
reverified immediately before each attempt. There is no confirmation prompt or
public dry-run/plan option; the command itself is the explicit programming
intent.

The host package keeps esptool optional (`s3-hidbot-host[flash]`, constrained to
`>=4.12,<5`); the base package imports and runs without it. The executor invokes
only `sys.executable -m esptool` with `shell=False`, a private working
directory, and absolute private staged image paths. Every inherited
`ESPTOOL_*` variable is removed and `ESPTOOL_CFGFILE` points to a temporary
configuration containing only an empty `[esptool]` section. The explicit port
still takes precedence over `S3_HIDBOT_SERIAL`; `S3_HIDBOT_BAUD` is ignored.

Nonzero and timeout results retry the identical programming operation at most
three total times, with a fixed 300-second timeout per attempt. There is no
erase, baud change, parameter fallback, or rebuild. Once programming succeeds,
it is permanently complete for that invocation: later runtime/readiness
failures can never re-enter esptool.

U6.4B2c then runs a fixed, bounded post-reset readiness procedure over the
control UART at 115200. It permits at most four fresh connections within a
controlled 20-second application-level deadline. Each open connection discards
at most 8192 raw bytes for up to 0.5 seconds and requires 0.1 seconds of RX
quiet before it creates a fresh Client. This is lexical post-reset alignment,
not a newline, framing, or response-validation mechanism: drained boot text,
NUL bytes, partial prefixes, and stale queued frames are neither logged as
protocol output nor fed to the Framer. The deadline covers waits controlled by
the application; it does not claim an independent OS tty-open timeout.

After the quiet boundary, normal Client responsibilities remain unchanged:
Framer reset, `TRANSPORT_SYNC` for the device parser, a fresh hello request ID,
fresh nonce, and fresh session correlation. The post-flash path sends only
`protocol.hello` and `system.info`, validates the latter, and reuses the
existing `compare_firmware_identity()` authority. Exact `MATCH` is required for
exit 0. It does not require native USB/HID, query `usb.status`, or send any HID
command.

Normal mode reports programming and verification phases separately; JSON mode
emits one phase-aware object. A successful flash followed by `MISMATCH` or
`IDENTITY_UNAVAILABLE` exits 7, `TRANSPORT_UNAVAILABLE` exits 3, and a bounded
readiness/request `TIMEOUT` exits 6. Definite protocol, compatibility, or
session-semantic failures exit 4; a correlated remote error response exits 5.
Missing/incompatible esptool remains a usage failure (exit 2), while exhausted
programming attempts remain exit 8 with a bounded diagnostic tail. The
standalone `verify-firmware` command is unchanged and remains useful for an
explicit later comparison; it is not an automatic fallback or reflash authority
for `flash-firmware`.
