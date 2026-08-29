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

## Safe diagnostic commands

The `hidbotctl` command is intentionally limited to the four diagnostic
commands `hello`, `ping`, `info`, and `usb-status`. The first safe interactions
with a device are `hello` and `usb-status`:

```bash
export S3_HIDBOT_SERIAL="<serial-device>"
hidbotctl hello
hidbotctl usb-status
```

The package also exposes Python primitive APIs such as `Client`,
`PySerialTransport`, `release_all()`, `keyboard_report()`, and
`mouse_report()`. These are bounded protocol primitives, not a high-level
typing, clicking, dragging, or pointer-automation layer. Review the
[project safety and protocol documentation](https://github.com/w-dee/s3-hidbot/blob/main/docs/development/uart-control-plane.md)
before using an unsafe primitive. The package is distributed under the
[MIT License](LICENSE).
