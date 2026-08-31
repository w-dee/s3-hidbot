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
| UART read-only diagnostics | `hello`, `ping`, `info`, `usb-status`, `verify-firmware ARTIFACT` |
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
| `hidbotctl usb-status` | UART | Observe native USB lifecycle/readiness; it does not require native USB to be attached. |
| `hidbotctl self-test` | UART | Safe diagnostic sequence including `release-all`. |
| `hidbotctl release-all` | UART | Explicit keyboard/mouse all-up recovery. |
| `hidbotctl verify-firmware ARTIFACT` | Artifact + UART | Verify artifact first, then fresh hello and system info identity comparison. No flash or HID. |
| `hidbotctl flash-firmware ARTIFACT` | Artifact + UART + `[flash]` | Destructive verified provisioning. Native USB is not required. |
| `hidbotctl keyboard-report --unsafe-hid --modifiers N [--key USAGE ...]` | UART + approved native HID topology | One unsafe keyboard report. |
| `hidbotctl mouse-report --unsafe-hid --buttons N --x N --y N --wheel N --pan N` | UART + approved native HID topology | One unsafe mouse report. |

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
