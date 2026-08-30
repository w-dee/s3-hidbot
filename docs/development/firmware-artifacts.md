# Firmware artifacts

This document defines the U6.3A local firmware artifact contract. It is a
development contract, not a release announcement: no public GitHub Release or
published artifact exists yet.

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

`tools/verify_firmware_artifact.py` is stdlib-only and verifies either an
extracted bundle or its archive. It strictly validates schema fields, hashes,
provenance links, flash-plan mappings, path safety, archive traversal safety,
and binary-safe privacy markers (developer paths, serial configuration,
`.envrc`, and obvious secret markers).

During a local build, the builder additionally checks the invocation's source
root, working directory, `HOME`, and `S3_HIDBOT_SERIAL` values in memory. These
machine-local values are never written into the manifest or payload.

`tools/test-firmware-artifact.sh` is the focused local entrypoint. It runs
synthetic verifier negative tests and builds two independent artifacts with
the same explicit inputs, then compares the ELF, binaries, flash plan,
manifest, checksums, and final archive byte-for-byte. Temporary build,
staging, pip, and archive paths are removed after the test.

## Scope boundary

U6.3A does not publish artifacts, add a GitHub Actions artifact workflow, add a
flash helper, compare artifacts with runtime UART identity, create attestations
or SBOMs, or change firmware versioning. Container-backed CI and public release
policy are subsequent slices; a release version and tag are deferred to U6.6.
