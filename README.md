# s3-hidbot

## What is s3-hidbot?

`s3-hidbot` is an ESP32-S3 diagnostic HID bridge. It exposes a native USB
Composite HID device (Boot Keyboard + Boot Mouse) and a separate bounded JSON
control plane over the board's USB-UART console.

This is a low-level bring-up and control foundation. It does not provide a
typing, clicking, dragging, or arbitrary HID automation layer.

## Supported development hardware

The validated development fixture is the Freenove ESP32-S3 WROOM Board /
FNK0085. The evidence below is scope-specific to that fixture and is not a
claim that every ESP32-S3 board has the same wiring or power behavior.

## USB port roles

Keep the two USB paths separate:

```text
Host PC
  |
  +-- USB-UART -----------------> S3 console / control plane / flash / monitor
  |
  +-- native USB-OTG ------------> HID device path toward the DUT USB host
```

The USB-UART path is used for diagnostics and control. The native USB-OTG
path is the TinyUSB HID device path. Do not infer that one path is attached
from the state of the other. Board-specific VBUS, backfeed, and detach-sense
behavior is documented only where confirmed in
[`hardware-validation.md`](docs/development/hardware-validation.md).

## Quick start

### Firmware

The firmware uses ESP-IDF v5.5.4 and keeps `firmware/` as the ESP-IDF project
root. Activate the supported ESP-IDF environment in local shell
configuration, then run:

```bash
cd firmware
idf.py build
```

For physical flashing and monitoring, set the machine-local USB-UART device
in your shell and follow the safety gate in
[`hardware-validation.md`](docs/development/hardware-validation.md):

```bash
cd firmware
export S3_HIDBOT_SERIAL=/dev/serial/by-id/<s3-hidbot-uart>
idf.py -p "$S3_HIDBOT_SERIAL" flash monitor
```

Do not copy an actual serial identifier or ESP-IDF installation path into the
repository.

### Host package

From the repository root (not `firmware/`), install the host package, which
uses `pyserial`:

```bash
python3 -m pip install ./host
export S3_HIDBOT_SERIAL=/dev/serial/by-id/<s3-hidbot-uart>
hidbotctl hello
hidbotctl usb-status
hidbotctl release-all
hidbotctl self-test
```

The first safe interactions are `hello` and `usb-status`; `release-all` is the
explicit safety recovery operation. `self-test` runs the safe control-plane
diagnostic sequence and is not proof that a keyboard or mouse event reached a
host. Primitive keyboard and mouse report
commands are also available, but require the command-local `--unsafe-hid`
opt-in. They send one report through the existing Python API path; this is not
a typing, clicking, dragging, or macro layer.

## Python HID primitive API

The public host surface includes `Client`, `PySerialTransport`,
`Client.connect()`, `ping()`, `info()`, `usb_status()`, `release_all()`,
`keyboard_report()`, and `mouse_report()`. These are explicit primitives,
not high-level keyboard or pointer automation helpers. The same keyboard and
mouse primitives are available from `hidbotctl keyboard-report` and
`hidbotctl mouse-report` only with explicit `--unsafe-hid`; nonzero mouse
buttons can remain held, so use `hidbotctl release-all` for recovery. Signatures,
validation, response states, and retry behavior are authoritative in
[`uart-control-plane.md`](docs/development/uart-control-plane.md) and the
actual host source.

## Safety model

- A successful `hello` creates a session with a 5000 ms lease.
- Lease expiry and published USB lifecycle changes revoke authority and
  require automatic all-up safety handling.
- `release_all` is the public safety recovery operation.
- `submitted` means that TinyUSB accepted report bytes; it does not mean the
  host consumed them or produced an evdev event.
- Suspend, unmount, attach-generation changes, and authority-epoch changes
  prevent stale unsafe work from being replayed.

The cache, epoch, ticket, and lifecycle details are specified only in
[`uart-control-plane.md`](docs/development/uart-control-plane.md).

## Current status

`IMPLEMENTED` means present in the repository. `NATIVE VALIDATED` means
covered by host/native validation. `HARDWARE VALIDATED` means observed in the
documented development fixture and test scope. `HARDWARE DEFERRED` means no
such claim is made yet.

- `IMPLEMENTED`: Composite Keyboard + Mouse HID, UART control plane, session
  lease, authority/lifecycle safety, `hid.release_all` and its
  `hidbotctl release-all` safety command, `hidbotctl self-test` diagnostic,
  keyboard and mouse primitive APIs.
- `NATIVE VALIDATED`: framing, strict protocol parsing, session/cache/lease
  behavior, HID safety state machine, host client, CLI, self-test orchestration,
  and firmware build.
- `HARDWARE VALIDATED`: composite HID enumeration, UART control reliability,
  `release_all`, the F24 keyboard sentinel path, and a small relative `REL_X`
  mouse movement path.
- `HARDWARE DEFERRED`: mouse buttons, wheel/pan, physical report-failure or
  HID-not-ready races, long-duration soak, and board-specific VBUS/backfeed
  conclusions.

The detailed evidence matrix and sentinel policy are in
[`hardware-validation.md`](docs/development/hardware-validation.md).

## Validation

Use the canonical commands and prerequisites in
[`validation-entrypoints.md`](docs/development/validation-entrypoints.md).
CI covers privacy/static checks, host tests, native tests, the IDF-dependent
protocol test, and an ESP-IDF firmware build. CI does not replace real USB,
HID, or physical lifecycle validation.

## Documentation map

- [`AGENTS.md`](AGENTS.md): stable repository and AI-agent invariants.
- [`codex-runbook.md`](docs/development/codex-runbook.md): contributor
  procedure and review gates.
- [`validation-entrypoints.md`](docs/development/validation-entrypoints.md):
  canonical local and CI validation.
- [`hardware-validation.md`](docs/development/hardware-validation.md):
  physical operation, safety, and evidence/deferred scope.
- [`uart-control-plane.md`](docs/development/uart-control-plane.md): normative
  protocol, runtime, host, and safety contract.

## Limitations

There is no raw JSON command mode, type/click/drag helper, macro, VBUS monitor
implementation, or hardware runner in this foundation. Review the hardware
gate before connecting both USB paths or sending an unsafe HID primitive. Use
`hidbotctl release-all` for explicit safety recovery.
