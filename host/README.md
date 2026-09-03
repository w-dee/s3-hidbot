# s3-hidbot-host

`s3-hidbot-host` is the pure-Python client and `hidbotctl` CLI for the
s3-hidbot UART control plane. It provides bounded transport, framing, protocol,
session, artifact-verification, identity-comparison, and explicit HID primitive
APIs for an FNK0085 fixture.

Python 3.11 or newer is required.

## Distribution and installation

The package is not published on PyPI. When a published version is available,
obtain its wheel and adjacent checksum from GitHub Releases. For development,
install either a checkout or the temporary `host-package` wheel from a
matching GitHub Actions run (retained for 14 days):

```bash
python3 -m pip install ./host
python3 -m pip install './host[flash]'
```

The optional `flash` extra installs supported `esptool >=4.12,<5`. A downloaded
wheel can be installed equivalently with `'<wheel-path>[flash]'`. Verify its
adjacent checksum before installation. The complete clean-room acquisition and
installation flow is in the project
[operator quick start](../docs/operator/quick-start.md).

## CLI categories

- Hardware-free validation: `verify-artifact ARTIFACT`.
- UART read-only diagnostics: `hello`, `ping`, `info`, `usb-status`, `usb-exposure-status`, `ble-exposure-status`, and
  `verify-firmware ARTIFACT`.
- Explicit USB exposure control: `usb-attach` and `usb-detach`.
- Explicit BLE exposure control: `ble-enable` and `ble-disable`.
- Explicit BLE pairing control: `ble-pairing-status` and
  `ble-pairing-respond --pairing-id ID`.
- Explicit HID output routing: `hid-route-status` and `hid-route-set none|usb|ble`.
- UART diagnostic with safety action: `self-test`.
- Explicit safety recovery: `release-all`.
- Destructive provisioning: `flash-firmware ARTIFACT`.
- Unsafe HID injection: `keyboard-report` and `mouse-report`.

The serial port comes from `--port` or machine-local `S3_HIDBOT_SERIAL`.
For ordinary serial commands, `--baud` overrides `S3_HIDBOT_BAUD`, then 115200
is used. `flash-firmware` deliberately ignores that environment variable and
uses its fixed policy.

Native USB HID is hidden by default. Use CH343 UART to request `usb-attach`,
then poll `usb-exposure-status`; do not infer native USB enumeration from an
accepted attach response. Exposure and routing are separate: mount leaves the
route at `none`, so use `hid-route-set usb`, establish a fresh hello, and only
then submit an authorized unsafe report. `hid-route-set none` safely releases
the old route but leaves USB mounted. `usb-detach` attempts safety all-up handling before
public driver uninstall but cannot prove host-observed release. A lifecycle
result may retire control authority, so reconnect before later commands. The
legacy `usb-status` command remains a basic readiness view.

BLE is separately uninitialized and hidden at boot. `ble-enable` lazily starts
BLE advertising; `ble-disable` returns it to hidden idle. Once a peer is
connected, secured, composite-subscribed, and unsuspended, route-v2 firmware
allows explicit `hid-route-set ble` from stable none. The client negotiates
`hid.output-route-v2`, falls back to v1 for none/USB, and rejects BLE locally
on v1-only firmware. Switching USB and BLE always requires an explicit stable
none boundary, and reconnect or restored eligibility never auto-selects BLE.
`ready` is route eligibility, not proof that a peer received a report.

Pairing is an explicit operator transaction:

1. Run `hidbotctl ble-pairing-status` and note the current `pairing_id`.
2. Run `hidbotctl ble-pairing-respond --pairing-id ID` with that ID.
3. Enter the six-digit passkey at the controlling-terminal prompt; input echo
   is disabled and the passkey is never accepted through argv, environment, a
   config file, or ordinary stdin.
4. Run `hidbotctl ble-pairing-status` again to inspect the result.

The CLI avoids shell-history/argv exposure, and the library does not
intentionally log or retain the passkey beyond the one request and its exact
transport retries. Python strings and bytes are immutable, however, so they
cannot be guaranteed zeroized. Typed API callers remain responsible for the
lifetime of their original passkey `str`; pyserial, interpreter allocator, and
kernel copies are outside the library's erasure guarantees.

## Artifact and identity commands

`hidbotctl verify-artifact ARTIFACT` accepts a bundle directory or `.tar.gz`
archive and validates it without resolving a serial port or accessing hardware.
It exits 0 for `VALID` and 2 for invalid/unverifiable input.

`hidbotctl verify-firmware ARTIFACT` verifies the artifact first, then uses
fresh hello and system-info requests over UART to compare project, target,
protocol, firmware version, source revision, ELF SHA-256, build profile, and
IDF version. It never flashes or sends HID. Exit 0 is `MATCH`; exit 7 is
`MISMATCH` or `IDENTITY_UNAVAILABLE`.

`hidbotctl flash-firmware ARTIFACT` is destructive. It accepts only the
verified supported policy, owns up to three identical programming attempts, and
never erases, changes baud, or mutates the plan. After programming succeeds it
never automatically reflashes because verification failed; exit 0 means both
programming and exact runtime identity `MATCH` succeeded. See
[safety and recovery](../docs/operator/safety-and-recovery.md).

## JSON and unsafe HID

Use `--json` for scripts. Successful execution emits one compact stdout object;
most errors use stderr and may leave stdout empty. `verify-firmware` can return
`ok:true` with `match:false`, so inspect exit status, `match`, and
`classification`. A post-programming verification failure reports
`FLASHED_VERIFICATION_FAILED` with `flash.classification:"FLASHED"`; it is not
authority to reflash.

`keyboard-report` and `mouse-report` require command-local `--unsafe-hid` and
explicit human authorization. They use raw HID values, not symbolic key names.
`submitted` does not prove the host consumed input, and keyboard/modifier/button
state can remain active. Successful commands, timeouts, and client close do
not automatically release it; use `hidbotctl release-all` for explicit safety
recovery and do not outer-retry ambiguous unsafe requests.

## Further documentation

The repository's [operator documentation](../docs/operator/README.md) is the
external contract for connector mapping, development-artifact acquisition,
exit codes, recovery, evidence, and automation. Development internals and the
normative protocol live in the project repository.
