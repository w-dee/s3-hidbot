# CLI reference

`hidbotctl` is the host CLI. For normal serial commands, `--port` overrides
`S3_HIDBOT_SERIAL`; `--baud` overrides `S3_HIDBOT_BAUD`, which otherwise
defaults to 115200. `--timeout`, `--attempts`, `--json`, and reserved/no-op
`--verbose` are accepted in either global-option position. Repeated options use
the later value.

`flash-firmware` intentionally ignores `S3_HIDBOT_BAUD` and rejects explicit
`--baud`, `--timeout`, and `--attempts`; its programming and post-flash policy
is fixed. `verify-artifact` remains parse-compatible with generic options but
does not resolve a serial port or use them.

## Command taxonomy

| Category | Commands |
| --- | --- |
| Hardware-free validation | `verify-artifact ARTIFACT` |
| UART read-only diagnostics | `hello`, `ping`, `info`, `usb-status`, `usb-exposure-status`, `ble-exposure-status`, `verify-firmware ARTIFACT` |
| Explicit USB exposure control | `usb-attach`, `usb-detach` |
| Explicit BLE exposure control | `ble-enable`, `ble-disable` |
| Explicit BLE pairing control | `ble-pairing-status`, `ble-pairing-respond --pairing-id ID` |
| BLE bond inspection | `ble-bond-list` |
| Destructive BLE bond administration | `ble-bond-remove BOND_ID` |
| Explicit HID output routing | `hid-route-status`, `hid-route-set none|usb|ble` |
| UART diagnostic with safety action | `self-test` |
| Explicit safety recovery | `release-all` |
| Destructive provisioning | `flash-firmware ARTIFACT` |
| Unsafe HID injection | `keyboard-report`, `mouse-report` |

`self-test` runs hello, ping, info, USB status, and `release-all`; it is not
strictly read-only. `release-all` may submit neutral all-up reports, but never
intentionally injects a key, button, or movement.

## Command syntax and preconditions

| Command | Requires | Meaning |
| --- | --- | --- |
| `hidbotctl verify-artifact ARTIFACT` | Artifact only | Validate a bundle directory or `.tar.gz` archive locally. No serial, HID, or hardware access. |
| `hidbotctl hello` | UART | Establish a fresh session and show capabilities. |
| `hidbotctl ping` | UART | Bounded diagnostic ping. |
| `hidbotctl info` | UART | Device information and available firmware identity. |
| `hidbotctl usb-status` | UART | Legacy/basic USB readiness shape (`mounted`, `suspended`, and endpoint readiness). It never exposes native USB. |
| `hidbotctl usb-exposure-status` | UART + `usb.exposure-control-v1` | Observe the authoritative native USB lifecycle state. It accepts no parameters and does not install or uninstall anything. |
| `hidbotctl usb-attach` | UART + `usb.exposure-control-v1` | Explicitly request a fresh public TinyUSB driver install. The immediate result is an accepted lifecycle snapshot, not mount, enumeration, or endpoint readiness proof. Establish a fresh session before later commands. |
| `hidbotctl usb-detach` | UART + `usb.exposure-control-v1` | Explicitly request old-generation all-up safety handling followed by public TinyUSB driver uninstall. The immediate result does not prove host-observed release or completed removal. Establish a fresh session before later commands. |
| `hidbotctl ble-exposure-status` | UART + `ble.exposure-control-v1` | Read the exact BLE exposure lifecycle snapshot without exposing an address, connection handle, CCCD, or security state. |
| `hidbotctl ble-enable` | UART + `ble.exposure-control-v1` | Lazily initialize BLE on first use and request connectable HID Service advertising. It does not select an HID route. |
| `hidbotctl ble-disable` | UART + `ble.exposure-control-v1` | Stop advertising/disconnect a peer and enter hidden idle while retaining the initialized stack. It does not change USB or its HID route/session. |
| `hidbotctl ble-pairing-status` | UART + `ble.pairing-transaction-v1` | Read one strict pairing transaction snapshot. It does not poll, connect, or pair automatically. |
| `hidbotctl ble-pairing-respond --pairing-id ID` | UART + controlling TTY + `ble.pairing-transaction-v1` | Prompt without echo for one six-ASCII-digit passkey and submit it to the exact nonzero pairing ID. There is deliberately no passkey argv, environment, config-file, or stdin-pipe mode. |
| `hidbotctl ble-bond-list` | UART + initialized BLE + `ble.bond-administration-v1` | List up to three firmware-side bonds in deterministic opaque-ID order, including non-secret persistence/schema state. It does not change firmware or host pairing state. |
| `hidbotctl ble-bond-remove BOND_ID` | UART + BLE hidden idle + `ble.bond-administration-v1` | Destructively remove exactly the 32-lowercase-hex firmware bond ID and its companion schema metadata. It never selects by name, prefix, or list position. |
| `hidbotctl hid-route-status` | UART + route v2, or v1 fallback | Read `desired`, `active`, `generation`, `transition`, and `ready`; the host prefers `hid.output-route-v2`. |
| `hidbotctl hid-route-set none` | UART + route v2, or v1 fallback | Safely retire the active USB or BLE route. BLE remains `releasing` until exact disconnect completes. |
| `hidbotctl hid-route-set usb` | UART + route v2, or v1 fallback | Select an already mounted, safety-clear USB transport from stable none. |
| `hidbotctl hid-route-set ble` | UART + `hid.output-route-v2` | Select an already connected, secured, composite-subscribed BLE peer from stable none. V1-only firmware is rejected locally. |
| `hidbotctl self-test` | UART | Safe diagnostic sequence including `release-all`. |
| `hidbotctl release-all` | UART | Explicit keyboard/mouse all-up recovery. |
| `hidbotctl verify-firmware ARTIFACT` | Artifact + UART | Verify artifact first, then fresh hello and system info identity comparison. No flash or HID. |
| `hidbotctl flash-firmware ARTIFACT` | Artifact + UART + `[flash]` | Destructive verified provisioning. Native USB is not required. |
| `hidbotctl keyboard-report --unsafe-hid --modifiers N [--key USAGE ...]` | UART + approved native HID topology | One unsafe keyboard report. |
| `hidbotctl mouse-report --unsafe-hid --buttons N --x N --y N --wheel N --pan N` | UART + approved native HID topology | One unsafe mouse report. |

Native USB HID is hidden at boot. The CH343 UART path is therefore the
bootstrap/control path: run `usb-attach` only when the native USB topology is
intended, then poll `usb-exposure-status`. `usb-attach` and `usb-detach` take
only omitted parameters or `{}` on the wire; non-empty parameters are rejected.
Repeated attach/detach can allocate and free TinyUSB resources. `self-test`
does not attach native USB or select a route. USB may remain mounted with route
`none`; attach, mount, resume, reattach, and reconnect never select USB.

`usb-exposure-status` is the lifecycle authority and has exactly `desired`,
`observed`, `generation`, `mounted`, `suspended`, `keyboard_ready`,
`mouse_ready`, `safety_pending`, `host_release_uncertain`,
`recovery_required`, and `last_error`. `last_error` is `null` or an
`operation`/signed-`code` object. A true `recovery_required` means no automatic
retry is attempted; reboot or manual operator intervention is required.
`usb.status` stays wire-compatible as the legacy/basic readiness command.

`ble-exposure-status` has exactly `desired`, `observed`, `generation`,
`stack_ready`, `advertising`, `connected`, `recovery_required`, and
`last_error`. BLE is uninitialized/non-advertising at cold boot. USB and BLE
may be exposed simultaneously. Route v2 adds explicit BLE output selection
without changing BLE exposure or pairing. It never enables, advertises, pairs,
queues a future selection, or claims delivery merely because `ready=true`;
NimBLE `stack_accepted` is not a peer acknowledgment. Direct USB-to-BLE and
BLE-to-USB selection is rejected: first select none and wait for observable
`stable`, then select the new route. Disconnect, restored security, CCCD
Restore, and reconnect never auto-select BLE. Route-v1 remains `none|usb`;
new clients fall back for those values only.

The pairing workflow is deliberately manual: query `ble-pairing-status`, note
its non-null `pairing_id`, run `ble-pairing-respond --pairing-id ID`, enter the
passkey at the no-echo controlling-terminal prompt, then query status again.
The prompt is written directly to the controlling terminal, including in
`--json` mode, so stdout remains one machine-readable result. Without a
controlling TTY the command fails before sending `ble.pairing.respond` and
never reads ordinary stdin.

This avoids passkey exposure in argv and shell history. The host does not
intentionally log or retain the passkey beyond the request/retry lifetime, but
immutable Python `str` and `bytes` objects cannot be guaranteed zeroized.
Typed API callers own the lifetime of their original passkey string; no claim
is made about interpreter allocator, pyserial, or kernel copies.

The firmware stores at most three bonds and never evicts one automatically.
Use `ble-bond-list` to obtain the exact opaque ID. Before removal, select route
`none` if BLE is active, wait for stable retirement, and run `ble-disable` until
BLE is hidden, idle, disconnected, and non-advertising. A stable USB route may
remain active and is not changed by removal. Successful removal verifies that
both NimBLE security records and the exact peer's HID schema revision are gone;
unrelated bonds are preserved. The host OS maintains a separate pairing
record, so this command does not unpair BlueZ or any other host stack. The next
encounter requires firmware-side pairing again and may require a separately
authorized host-side cleanup if that host retains stale bond state.

## Artifact and firmware identity

`verify-artifact` accepts a verified bundle directory or `.tar.gz` archive.
It exits 0 with `VALID`, or 2 for missing, malformed, or unverifiable input.
JSON returns `ok`, `classification`, and `artifact` identity fields:
project, target, protocol version, firmware version, source revision,
application ELF SHA-256, build profile, and IDF version.

`verify-firmware` verifies the artifact before opening UART, then uses a fresh
hello and `system.info`. It compares those exact identity fields. It does not
flash, access native USB, query USB status, or send HID. Exit 0 is `MATCH`; exit
7 is `MISMATCH` or `IDENTITY_UNAVAILABLE`.

In JSON, `ok:true` with `match:false` means the comparison completed but did
not match. Automation must inspect process exit status, `match`, and
`classification`, not `ok` alone.

## Unsafe report grammar

Both unsafe commands require command-local `--unsafe-hid`; there is no
environment or remembered opt-in. Values are raw HID values, not symbolic key
names. Keyboard modifiers are `0..255`; provide zero through six decimal or
`0x`-prefixed usages in strict ascending, distinct order. Mouse buttons are
`0..31`; `x`, `y`, `wheel`, and `pan` are each `-127..127`, and all five mouse
fields are required.

## JSON and exits

Use `--json` for automation. A successful command emits one compact JSON
object on stdout. Most configuration, transport, protocol, and programming
errors have empty stdout and human diagnostics on stderr; stderr is not a
stable schema. See [automation](automation.md#json-contract).

| Exit | Meaning | Applies to |
| ---: | --- | --- |
| 0 | Success; flash means programmed and exact runtime identity `MATCH` | All |
| 2 | Syntax, configuration, artifact, policy, or dependency error | All |
| 3 | Transport unavailable | Serial commands; post-flash verification |
| 4 | Protocol, compatibility, or session-semantic failure | Serial commands; post-flash verification |
| 5 | Correlated remote/device error | Serial commands; post-flash verification |
| 6 | Bounded request/readiness timeout | Serial commands; post-flash verification |
| 7 | `MISMATCH` or `IDENTITY_UNAVAILABLE` | `verify-firmware`, `flash-firmware` |
| 8 | Programming/esptool failure | `flash-firmware` only |
| 130 | Keyboard interrupt | Executing commands |

For provisioning phases and safe recovery, read
[safety and recovery](safety-and-recovery.md).
