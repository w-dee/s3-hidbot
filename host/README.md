# s3-hidbot-host

`s3-hidbot-host` is the pure-Python host client for the s3-hidbot UART
control plane. It provides bounded transport, framing, protocol, session, and
HID primitive APIs for a connected s3-hidbot device.

The project is maintained at
<https://github.com/w-dee/s3-hidbot>. Firmware setup, hardware safety, and the
normative control-plane contract are documented in the project repository.

## Requirements and installation

Python 3.11 or newer is required. Install the package with pip:

```bash
python3 -m pip install s3-hidbot-host
```

For a checkout-based installation, run `python3 -m pip install ./host` from
the repository root. The serial device is selected through the machine-local
`S3_HIDBOT_SERIAL` environment variable or an explicit `--port` argument; do
not put a machine-specific device path in project files.

## Safe diagnostic and recovery commands

The `hidbotctl` command exposes the five safe diagnostic commands `hello`,
`ping`, `info`, `usb-status`, and `self-test`, plus the safety recovery command
`release-all`. The first safe interactions with a device are `hello` and
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
