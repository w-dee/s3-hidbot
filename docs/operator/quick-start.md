# Linux-first clean-room quick start

This is the currently physically validated operator path. The code and package
are intended to be portable, but CI and accepted fixture validation are
currently Linux-only. macOS and Windows-specific operator procedures are not
yet validated.

## Before starting

You need a Freenove ESP32-S3 WROOM Board / FNK0085, a data-capable USB-C cable,
Python 3.11 or newer, and access to the intended GitHub Actions run. No native
USB/HID connection is required for provisioning or identity verification.

With the board front-facing and the ESP32-S3 module at the top, use the **left
USB-C below EN/RST**. It is the CH343 USB-UART port for programming and control.
The right USB-C below BOOT is native USB-OTG/HID and should remain disconnected
for this provisioning path. VBUS/backfeed and general dual-cable behavior are
**UNKNOWN**; do not add a second cable without a deliberately validated setup.

## 1. Select matching development artifacts

Choose an exact source revision and a successful Actions run for that revision.
Download and unpack both artifacts from their matching runs:

- `firmware-artifact`: a versioned `s3-hidbot-firmware-*.tar.gz` and matching
  `.sha256` file;
- `host-package`: a versioned `s3_hidbot_host-*.whl` and matching `.sha256`
  file.

These artifacts expire after 14 days. They are not a GitHub Release and the
host package is not on PyPI. Record the commit SHA and Actions run identifier
before continuing.

## 2. Verify downloaded bytes

On Linux, verify each downloaded file from the directory containing its
provided checksum:

```bash
sha256sum --check s3-hidbot-firmware-<version>-esp32s3-freenove-fnk0085.tar.gz.sha256
sha256sum --check s3_hidbot_host-<version>-py3-none-any.whl.sha256
```

The outer checksum compares downloaded bytes with the record produced beside
that Actions artifact. It detects accidental corruption or substitution
relative to that record. It is not a publisher signature, independent artifact
authentication, or a replacement for choosing the intended repository, run,
and source SHA.

## 3. Install the matching host wheel

Create an isolated environment, then install the wheel with the optional flash
dependency:

```bash
python3 -m venv .venv
. .venv/bin/activate
python -m pip install './s3_hidbot_host-<version>-py3-none-any.whl[flash]'
python -c 'import sys; from importlib.metadata import version; print(sys.version); print(version("s3-hidbot-host"))'
python -m esptool version
hidbotctl --help
```

The `[flash]` extra supplies supported `esptool >=4.12,<5`. A source checkout
can instead install `./host[flash]`, but it is not required for a wheel-based
operator flow.

## 4. Select the control port

Connect only the left CH343 USB-C port. Identify the new CH343 serial device
using your Linux device-management procedure, then keep the selected path
machine-local:

```bash
export S3_HIDBOT_SERIAL='<ch343-control-port>'
```

Do not put the exact path into repository files, shared automation, or bug
reports. If no port appears, inspect the cable, connector, enumeration and
permissions; native USB-OTG is not a substitute for this port.

## 5. Verify the firmware artifact without hardware

```bash
hidbotctl --json verify-artifact \
  ./s3-hidbot-firmware-<version>-esp32s3-freenove-fnk0085.tar.gz
```

Require exit 0 and `classification:"VALID"`. Record the returned version,
source revision, application ELF SHA-256, build profile, target, IDF version,
and protocol version.

## 6. Provision the fixture

`flash-firmware` is destructive programming. It is the explicit authorization
to program the verified supported plan:

```bash
hidbotctl --json flash-firmware \
  ./s3-hidbot-firmware-<version>-esp32s3-freenove-fnk0085.tar.gz
```

Success requires all of the following: exit 0,
`classification:"FLASHED_AND_VERIFIED"`,
`flash.classification:"FLASHED"`, and
`verification.classification:"MATCH"`. The command owns up to three identical
programming attempts. After programming succeeds, it must never automatically
reflash because post-flash verification failed.

## 7. Confirm and retain evidence

An optional later comparison uses the same artifact:

```bash
hidbotctl --json verify-firmware \
  ./s3-hidbot-firmware-<version>-esp32s3-freenove-fnk0085.tar.gz
```

Require exit 0 and `match:true`. Save the command's JSON, exit status, package
and tool versions, source/run identifier, archive SHA-256, and sanitized
stderr. See [automation evidence](automation.md#evidence-record) and the
[recovery guide](safety-and-recovery.md) before retrying any failure.
