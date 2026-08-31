# s3-hidbot

`s3-hidbot` is an ESP32-S3 fixture that exposes two intentionally separate
interfaces:

- a bounded JSON UART control plane for diagnostics and provisioning; and
- a native USB Composite HID device (Boot Keyboard + Boot Mouse).

It is a low-level diagnostic fixture, not a typing, clicking, dragging, or
macro-automation product.

## Supported fixture

The validated fixture is the **Freenove ESP32-S3 WROOM Board / FNK0085**.
Evidence applies to that board and the documented scope only; it is not a
claim about arbitrary ESP32-S3 boards.

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

`flash-firmware` is destructive provisioning. It programs only a verified,
supported plan and returns success only after an exact runtime identity match.

## Current distribution status

This project is not published on PyPI and has no GitHub Release or public
stable tag/release yet. Current development firmware and host wheels are
temporary GitHub Actions artifacts retained for 14 days. U6.6 is expected to
establish the durable release path.

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
  on Linux.
- `HARDWARE DEFERRED`: mouse buttons, wheel/pan, physical failure races,
  long-duration soak, and board electrical conclusions.

The package is intended to be portable, but CI and accepted physical fixture
validation are currently Linux-only. Platform-specific macOS and Windows
operator instructions are not yet validated.
