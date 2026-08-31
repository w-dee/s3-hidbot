# s3-hidbot-host

`s3-hidbot-host` is the pure-Python host client for the s3-hidbot UART
control plane. It provides bounded transport, framing, protocol, session, and
HID primitive APIs for a connected s3-hidbot device.

The project is maintained at
<https://github.com/w-dee/s3-hidbot>. Firmware setup, hardware safety, and the
normative control-plane contract are documented in the project repository.

## Requirements and installation

Python 3.11 or newer is required. This package is not currently published on
PyPI; install it from a checkout with pip:

```bash
python3 -m pip install ./host
```

The optional flash extra installs the constrained esptool distribution only
when firmware programming is explicitly required:

```bash
python3 -m pip install './host[flash]'
```

The serial device is selected through the machine-local
`S3_HIDBOT_SERIAL` environment variable or an explicit `--port` argument; do
not put a machine-specific device path in project files.

## Safe diagnostic and recovery commands

The `hidbotctl` command exposes the five safe diagnostic commands `hello`,
`ping`, `info`, `usb-status`, and `self-test`, plus the safety recovery command
`release-all` and artifact-to-device identity comparison command
`verify-firmware`. The first safe interactions with a device are `hello` and
`usb-status`:

```bash
export S3_HIDBOT_SERIAL="<serial-device>"
hidbotctl hello
hidbotctl usb-status
hidbotctl release-all
hidbotctl self-test
```

`self-test` uses one connection and session to run `hello`, `ping`, `info`,
`usb-status`, and `release-all` in order. It is a control-plane diagnostic,
not proof of keyboard delivery, mouse delivery, evdev observation, or physical
HID behavior. `release-all` is a safe recovery operation that may submit
all-up HID reports when device state requires it; neither command intentionally
injects a key, button, or movement.

The package also exposes Python primitive APIs such as `Client`,
`PySerialTransport`, `release_all()`, `keyboard_report()`, and
`mouse_report()`. The equivalent CLI primitives require an explicit
command-local `--unsafe-hid` opt-in:

```bash
hidbotctl keyboard-report --unsafe-hid --modifiers 0 --key 0x73
hidbotctl mouse-report --unsafe-hid --buttons 0 --x 10 --y 0 --wheel 0 --pan 0
```

`--key` accepts raw decimal or `0xNN` usages; mouse buttons are absolute and
can remain held, while x/y/wheel/pan are relative one-report values. Use
`hidbotctl release-all` for explicit recovery. These are bounded protocol
primitives, not a high-level typing, clicking, dragging, or pointer-automation
layer. The CLI addition does not create new hardware evidence: mouse buttons,
wheel, and pan are implemented and natively validated, but their hardware
evidence remains deferred. Review the
[project safety and protocol documentation](https://github.com/w-dee/s3-hidbot/blob/main/docs/development/uart-control-plane.md)
before using an unsafe primitive. The package is distributed under the
[MIT License](LICENSE).

## Artifact-only firmware verification

`hidbotctl verify-artifact ARTIFACT` validates an ordinary artifact archive or
an extracted artifact directory locally, without a serial port, connected
fixture, repository checkout, ESP-IDF, or any hardware operation. This makes
it suitable before provisioning a device when this package has been installed
from its wheel:

```bash
hidbotctl verify-artifact ./s3-hidbot-firmware.tar.gz
hidbotctl --json verify-artifact ./extracted-firmware-bundle
```

A valid artifact exits 0 and prints its compact runtime-comparable identity;
missing, malformed, or unverifiable input exits 2. `VALID` means the artifact
schema, checksums, payloads, flash plan, provenance relationships, and privacy
rules are internally valid. It is not a signature, publisher authentication,
device authentication, secure-boot result, or attestation.

## Firmware artifact identity comparison

`hidbotctl verify-firmware ARTIFACT` first verifies an ordinary artifact archive
or extracted artifact directory locally, then uses one UART connection to send
only `protocol.hello` and `system.info`. It compares the verified artifact's
project, target, protocol, firmware version, source revision, linked ELF
SHA-256, build profile, and ESP-IDF version with validated runtime identity:

```bash
hidbotctl verify-firmware ./s3-hidbot-firmware.tar.gz
hidbotctl --json verify-firmware ./extracted-firmware-bundle
```

An exact match exits 0. A completed comparison with a mismatch or unavailable
identity exits 7 and reports its deterministic classification; malformed or
unverifiable artifact input exits 2 before any serial transport is constructed.
The command sends no HID report, does not query USB status, and does not run
`release-all`; `protocol.hello` and `system.info` do create and refresh the
normal control-session lease. This is provenance evidence, not physical device
authentication, UART peer authentication, secure boot, or proof of signed
firmware authenticity.

## Safe firmware flashing

`hidbotctl flash-firmware ARTIFACT` is the explicit programming command for a
verified `.tar.gz` bundle or extracted bundle directory. It first programs the
supported plan and then performs bounded post-reset runtime identity
verification over the control UART:

```bash
hidbotctl --port "$S3_HIDBOT_SERIAL" flash-firmware ./s3-hidbot-firmware.tar.gz
hidbotctl --json flash-firmware ./extracted-firmware-bundle
```

Install the optional `flash` extra before using it. The command accepts the
explicit `--port` (otherwise `S3_HIDBOT_SERIAL`) plus `--json` and `--verbose`;
`--baud`, `--timeout`, and `--attempts` are rejected as usage errors, and
`S3_HIDBOT_BAUD` is ignored. Artifact verification and the supported
FNK0085/ESP32-S3 4 MiB DIO flash plan complete before esptool is started.
There is no confirmation prompt or public dry-run/plan mode; invoking this
command is the explicit programming intent.

The runner invokes only the installed esptool module with absolute paths from
a private staged snapshot. It removes inherited `ESPTOOL_*` settings, uses a
private empty esptool configuration and working directory, and retries an
unchanged nonzero or timed-out operation at most three times with a fixed
five-minute timeout. It never erases, changes baud, falls back to another
plan, or rebuilds.

Base package installation remains esptool-free; a missing or unsupported
optional dependency exits 2 with installation guidance before any subprocess
or serial access. Programming failure after the bounded attempts exits 8.
JSON mode emits one compact success object and keeps esptool diagnostics out of
stdout; normal mode passes diagnostics through.

After esptool succeeds, programming is permanently complete for that command
invocation: readiness retries never reflash. The host uses the fixed 115200
control UART policy, with a controlled 20-second readiness deadline, at most
four fresh connections, and a bounded raw receive drain (0.5 seconds or 8192
bytes) followed by a 0.1-second quiet boundary. The discarded bytes are not
framed or interpreted. Only then does a fresh Client perform its normal framing
reset, device parser sync, nonce/session-correlated `protocol.hello`, and
`system.info`; the existing artifact-to-runtime comparator must return `MATCH`.
No HID request, USB status query, or native USB connection is required.

Therefore `flash-firmware` exits 0 only for **FLASHED_AND_VERIFIED**: esptool
succeeded, the firmware became reachable, and runtime identity matched the
verified artifact. A successful programming phase can still produce a
phase-aware post-flash verification failure: mismatch or unavailable identity
exits 7, transport unavailability exits 3, and bounded startup/request timeout
exits 6. Protocol, compatibility, and session-semantic failures exit 4; a
correlated remote error response exits 5. The standalone `verify-firmware`
command remains available for a separate later comparison, but it is not an
automatic fallback or reflash authority for this command.
