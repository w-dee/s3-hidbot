# Linux-first clean-room quick start

This is the currently physically validated Linux-first operator path. The code
and package are intended to be portable, but CI, provisioning, and host-side
evdev procedures remain Linux-first. Named Android devices have only scoped
BLE peer evidence in the development hardware matrix; they do not establish a
general Android operator procedure. macOS and Windows-specific operator
procedures are not validated.

## Before starting

You need a Freenove ESP32-S3 WROOM Board / FNK0085, a data-capable USB-C cable,
Python 3.11 or newer, and access to the intended GitHub Actions run. No native
USB/HID connection is required for provisioning or identity verification.

With the board front-facing and the ESP32-S3 module at the top, use the **left
USB-C below EN/RST**. It is the CH343 USB-UART port for programming and control.
The right USB-C below BOOT is native USB-OTG/HID and should remain disconnected
for this provisioning path. VBUS/backfeed and general dual-cable behavior are
**UNKNOWN**; do not add a second cable without a deliberately validated setup.
Native USB HID is hidden by default in the current development firmware. The
CH343 UART path remains available for bootstrap and lifecycle control even when
native USB is absent.

## 1. Select matching stable or development artifacts

For a published version, start at GitHub Releases, choose the intended version,
and download its firmware archive, host wheel, and adjacent checksum files.
Also retain the attached `LICENSE` and `THIRD_PARTY_NOTICES.md`.

For development or an unreleased commit, choose an exact source revision and a
successful Actions run for that revision. Download and unpack both artifacts
from their matching runs:

- `firmware-artifact`: a versioned `s3-hidbot-firmware-*.tar.gz` and matching
  `.sha256` file;
- `host-package`: a versioned `s3_hidbot_host-*.whl` and matching `.sha256`
  file.

Development artifacts expire after 14 days. They are not GitHub Releases and
the host package is not on PyPI. Record the commit SHA and Actions run
identifier before continuing. For a published version, record the release tag
and source revision supplied by `verify-artifact`.

## 2. Verify downloaded bytes

On Linux, verify each downloaded file from the directory containing its
provided checksum:

```bash
sha256sum --check s3-hidbot-firmware-<version>-esp32s3-freenove-fnk0085.tar.gz.sha256
sha256sum --check s3_hidbot_host-<version>-py3-none-any.whl.sha256
```

The outer checksum compares downloaded bytes with the adjacent published
record. It detects accidental corruption or substitution relative to that
record. It is not a publisher signature, independent artifact authentication,
attestation, secure-boot proof, device authentication, or a replacement for
choosing the intended repository, release/run, and source SHA.

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

## Native USB exposure after an authorized hardware gate

Do not infer native USB availability from the UART port. When a separately
authorized native-USB qualification procedure applies, use the UART path to
request exposure and poll its lifecycle state:

```bash
hidbotctl --json usb-attach
# establish a fresh session, then:
hidbotctl --json usb-exposure-status
# when status is mounted and both endpoints are ready:
hidbotctl --json hid-route-set usb
# establish another fresh session before unsafe HID
```

Attach creates a fresh TinyUSB stack instance; `attaching` only means the
request was accepted. Wait for `disconnected` or `mounted` and never treat an
accepted response as enumeration or endpoint-ready proof. Mount does not select
an HID output route: reports remain `HID_NOT_READY` until explicit
`hid-route-set usb`. `hid-route-set none` leaves native USB mounted while safely
retiring HID output authority. `usb-detach` first
attempts all-up safety handling and then uninstalls the public TinyUSB stack;
it does not prove that the host observed the all-up state. If
`host_release_uncertain` or `recovery_required` is true, stop unsafe use and
require human review. This software slice is not itself authorization for a
physical attach/detach operation.

This is an intentional U7.2B migration. The old sequence was `usb-attach`, wait
for mount, then send HID. The U7.2B sequence is `usb-attach`, wait for mount,
observe `HID_NOT_READY` for HID while the route is none, run
`hid-route-set usb`, establish a fresh hello session, and only then send HID.

## BLE exposure after a separate authorized hardware gate

BLE is uninitialized and non-advertising at boot. `hidbotctl --json ble-enable`
requests lazy startup; pairing remains a separate explicit transaction. After
the peer is connected, secured, composite-subscribed, and unsuspended, select
BLE only from observable stable none:

```bash
hidbotctl --json hid-route-status
hidbotctl --json hid-route-set ble
```

The host uses route v2 when advertised. V1-only firmware supports none/USB and
rejects BLE locally. Never treat route selection or `ready:true` as proof of
peer delivery. To switch USB and BLE, select none and wait for `stable` before
selecting the other transport. Disconnect/reconnect and restored eligibility
do not restore the BLE route automatically.

## Converge to a safe/quiescent state

Use a fresh control session, inspect status, and converge explicitly:

```bash
hidbotctl --json release-all
hidbotctl --json hid-route-set none
# poll until active/desired are none and transition is stable
hidbotctl --json hid-route-status
hidbotctl --json ble-disable
# poll until hidden, idle, disconnected, and non-advertising
hidbotctl --json ble-exposure-status
```

Route `none` prevents normal HID delivery; it does not require native USB to
be detached. `release-all` and route retirement are best-effort device-side
safety operations, not proof that a host consumed an all-up report. If any
command result is lost or ambiguous, do not blindly replay a normal HID
report. Establish a fresh session, read route/exposure state, and continue only
after lifecycle convergence. Stop for operator diagnosis on
`recovery_required`, persistent uncertainty, or inability to establish the
all-up/route-none state. Routine recovery must not erase NVS or remove all
Bluetooth records.
