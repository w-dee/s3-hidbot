# s3-hidbot

`s3-hidbot` is an ESP32-S3 fixture that exposes two intentionally separate
interfaces:

- a bounded JSON UART control plane for diagnostics and provisioning; and
- a native USB Composite HID device (Boot Keyboard + Boot Mouse), hidden until
  explicitly exposed over UART.

It is a low-level diagnostic fixture, not a typing, clicking, dragging, or
macro-automation product.

## Supported fixture

The physically qualified fixture is the **Freenove ESP32-S3 WROOM Board /
FNK0085**, using its ESP32-S3-WROOM-1 module and board implementation with
8 MiB flash and 8 MiB PSRAM.
Evidence applies to that board and the documented scope only; it is not a
claim about arbitrary ESP32-S3 boards.

The canonical firmware targets a minimum 4 MiB flash envelope and does not
require external PSRAM. Those build minima do not qualify another board.

With the board viewed from the front and the ESP32-S3 module at the top:

- the **left USB-C**, below **EN/RST**, is the CH343 USB-UART connector for
  programming and UART control;
- the **right USB-C**, below **BOOT** and beside GPIO19/GPIO20, is the native
  USB-OTG connector for HID.

The official [board photograph](https://github.com/Freenove/Freenove_ESP32_S3_WROOM_Board/blob/main/Board.jpg)
and [pinout](https://github.com/Freenove/Freenove_ESP32_S3_WROOM_Board/blob/main/ESP32S3_Pinout.png)
are the physical-reference evidence. Provisioning and identity verification
need only the left CH343 connector; native USB is not required.

Keep the paths separate:

```text
Host / controller
  |
  +-- left USB-C, CH343 ------> flash + UART control
  |
  +-- right USB-C, USB-OTG --> native HID toward the DUT host
```

VBUS sourcing, backfeed behavior, general dual-cable power safety, immediate
detach sensing, and board-specific VBUS monitoring are **UNKNOWN**. Avoid
dual-cable operation unless its topology has been deliberately validated.

## Safety boundary

`keyboard-report` and `mouse-report` can create real keyboard or mouse input.
They require command-local `--unsafe-hid` and explicit human authorization.
`release-all` is the explicit all-up recovery command. A report result of
`submitted` means firmware accepted bytes; it does not prove that a host OS
consumed the event.

The CH343 UART control plane is the bootstrap path. Current development
firmware starts with the native TinyUSB stack uninstalled; `usb.attach` creates
a fresh public TinyUSB instance and `usb.detach` performs lifecycle-owned
safety handling before public uninstall. `usb.exposure.status` is the
authoritative lifecycle view, while legacy `usb.status` retains its basic
readiness schema. USB exposure does not select an HID output: even after mount,
unsafe reports return `HID_NOT_READY` until `hid.route.set {"route":"usb"}`.
`hid.route.set {"route":"none"}` safely removes the route while leaving USB
mounted. Accepted detach does not prove host-observed all-up, and
uncertainty remains fail-closed across a later attach. v0.1.0 is historical
always-exposed behavior; it cannot expose a future U7.1B firmware because it
does not advertise `usb.exposure-control-v1`.

Development firmware also has an explicit BLE exposure foundation. BLE is
uninitialized and non-advertising at boot; `ble-enable` lazily starts the
BLE-only NimBLE stack and advertises the project-owned, discoverable HID
Service database to at most one peer. `ble-disable` hides BLE while retaining
the initialized stack for later reuse. USB and BLE may be exposed
simultaneously. Current route-v2 firmware can explicitly select an eligible
secured BLE HID peer from stable none, while pairing remains an explicit
passkey transaction. Firmware keeps at most three verified bonds without
automatic eviction. The UART host can list opaque firmware bond IDs and remove
one exact disconnected bond while BLE is hidden; that removal does not alter a
host OS pairing database or an independent USB route.

`flash-firmware` is destructive provisioning. It programs only a verified,
supported plan and returns success only after an exact runtime identity match.

## Distribution status

When a published version is available, obtain its versioned firmware archive,
host wheel, adjacent SHA-256 files, `LICENSE`, and `THIRD_PARTY_NOTICES.md`
from the GitHub Releases page. The host package is not published on PyPI.

For development or an unreleased commit, use matching temporary
`firmware-artifact` and `host-package` GitHub Actions artifacts. They are
retained for 14 days and are not stable releases.

## USB/Bluetooth identifiers and qualification

s3-hidbot has not obtained a project-specific USB-IF VID/PID assignment or a
project-specific Bluetooth SIG Company Identifier. It has not completed
Bluetooth product qualification or listing for this project. v0.1.0 does not
implement BLE HID.

Any USB identifiers present in development firmware are solely for development
and interoperability testing. They do not represent a project-owned USB-IF
allocation or certification, a production or commercial identifier allocation,
or identifiers suitable for a user's product or redistribution.

The presence of current or future Bluetooth functionality in the source does
not represent Bluetooth SIG qualification or listing, trademark authorization,
or assignment of a project-specific Company Identifier.

The existing MIT License does not add a non-commercial-use restriction.
Open-source copyright permission is separate from external identifier,
qualification, listing, regulatory, trademark, membership, and other
authorization obligations. Anyone incorporating, redistributing, manufacturing,
selling, or otherwise using this software or firmware is responsible for
determining and obtaining the authorizations required for their intended use.
Requirements depend on intended use, product configuration, jurisdiction,
distribution model, and applicable rules. Publishing this firmware does not
provide those approvals. This statement is not legal advice.

## Clean-room path

For a development fixture, start with the operator documentation:

1. Select an exact source revision and successful Actions run.
2. Download matching `firmware-artifact` and `host-package` artifacts.
3. Verify their provided checksums and install the host wheel with `[flash]`.
4. Connect only the CH343 USB-UART connector, verify the artifact, then run
   `flash-firmware`.
5. Require exit 0 and `FLASHED_AND_VERIFIED` / `MATCH` before treating the
   fixture as provisioned.

The complete, Linux-first procedure is in
[`docs/operator/quick-start.md`](docs/operator/quick-start.md).

## Documentation

External operators and agents should start at
[`docs/operator/README.md`](docs/operator/README.md):

- [quick start](docs/operator/quick-start.md)
- [CLI reference](docs/operator/cli-reference.md)
- [safety and recovery](docs/operator/safety-and-recovery.md)
- [automation contract](docs/operator/automation.md)

Contributors should use the development documentation instead:

- [development runbook](docs/development/codex-runbook.md)
- [validation entrypoints](docs/development/validation-entrypoints.md)
- [hardware evidence and limits](docs/development/hardware-validation.md)
- [UART and HID protocol contract](docs/development/uart-control-plane.md)
- [firmware artifact contract](docs/development/firmware-artifacts.md)

## Validation status

- `IMPLEMENTED`: UART control, Composite HID, safety lease/lifecycle handling,
  artifact verification, identity comparison, and bounded provisioning.
- `NATIVE VALIDATED`: host, protocol, safety state machine, CLI and firmware
  build coverage.
- `HARDWARE VALIDATED`: FNK0085 UART control, composite enumeration,
  `release-all`, F24 keyboard sentinel, and one small relative mouse movement
  on Linux; scoped BLE HID, secure bond lifecycle, three-bond capacity,
  StoreFull/no-eviction behavior, and exact slot reuse on the documented lab
  peers.
- `HARDWARE DEFERRED`: mouse buttons, wheel/pan, physical failure races,
  long-duration soak, and board electrical conclusions.

The package is intended to be portable, but CI, provisioning instructions,
and host-side evdev qualification remain Linux-first. Named Android devices
appear only as scoped BLE peer evidence in the hardware matrix; this is not a
general Android operator-platform claim. Platform-specific macOS and Windows
operator instructions are not validated.
