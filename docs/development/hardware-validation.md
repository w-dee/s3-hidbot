# s3-hidbot hardware validation

## Scope and gate

Physical work is a separate gate and begins only after explicit human
approval. This document is the authoritative place for connector roles,
physical safety instructions, and the scope of evidence. It does not turn a
single development fixture into a universal board guarantee.

The validation vocabulary is deliberately narrow:

- `IMPLEMENTED` — present in the repository.
- `NATIVE VALIDATED` — covered by host/native tests or static checks.
- `HARDWARE VALIDATED` — observed on the documented development fixture for
  the stated test scope.
- `HARDWARE DEFERRED` — not yet established; no hardware claim is made.

## Board and port roles

The validated development fixture is the Freenove ESP32-S3 WROOM Board /
FNK0085. See the [official Freenove board material](https://github.com/Freenove/Freenove_ESP32_S3_WROOM_Board)
for the board's published resources.

```text
Host PC
  |
  +-- USB-UART --------------> CH343 console / control plane / flash / monitor
  |
  +-- native USB-OTG --------> ESP32-S3 HID device toward the DUT USB host
```

The two paths have different jobs. Opening the USB-UART path does not prove
that native USB-OTG is attached, and a HID enumeration does not prove that
the control UART is usable.

### Confirmed

- USB-UART is the flash, monitor, diagnostic, and UART control-plane path.
- Native USB-OTG is the TinyUSB Composite HID device path.
- The fixture has produced the documented Composite HID and UART evidence in
  the matrix below.

### Unresolved / do not infer

- VBUS sourcing, backfeed safety, and dual-cable power behavior beyond the
  tested setup.
- Immediate firmware detach sensing for every physical cable removal.
- A board-specific VBUS comparator or monitor implementation.

Do not connect or disconnect either path during a gate unless that gate's
procedure explicitly calls for it. Keep machine-local port values out of
tracked files.

## Serial and ESP-IDF configuration

Keep the machine-local serial device in shell configuration:

```bash
export S3_HIDBOT_SERIAL=/dev/serial/by-id/<s3-hidbot-uart>
```

The exact serial identifier is intentionally a placeholder here. A device
being absent from the normal sandbox is not evidence that it is physically
disconnected; physical serial access may require elevation.

The firmware baseline is ESP-IDF v5.5.4 with `firmware/` as the project root.
Keep installation and activation paths outside the repository. Use the
canonical build and validation commands in
[`validation-entrypoints.md`](validation-entrypoints.md).

## Hardware evidence matrix

The entries below describe observed scope, not a promise for all hosts,
boards, operating systems, or future firmware.

| Area | Status | Observed scope |
| --- | --- | --- |
| Composite Keyboard + Mouse enumeration | `HARDWARE VALIDATED` | Freenove fixture with Linux host: one device, Boot Keyboard and Boot Mouse interfaces, expected endpoints, driver binding, and stable mount. |
| UART control-plane reliability | `HARDWARE VALIDATED` | Fresh sessions, diagnostic commands, exact retries, sync recovery, bounded response checks, and repeated responses on the USB-UART path. |
| `hid.release_all` | `HARDWARE VALIDATED` | Safety-only command, already-up/submitted outcomes, exact retry/cache behavior, lease refresh, and takeover behavior in the accepted gate. |
| F24 keyboard report path | `HARDWARE VALIDATED` | F24/HID usage `0x73` sentinel report path with Linux evdev machine-observed `KEY_F24` value `1` (DOWN) and value `0` (UP), explicit release recovery, and no claim that F24 is globally side-effect-free. |
| Relative `REL_X` mouse report path | `HARDWARE VALIDATED` | Small positive relative movement observed once per accepted report; no claim about pointer acceleration or exact screen pixels. |
| U5.4.3 raw `REL_X` physical smoke | `HARDWARE VALIDATED` | Linux evdev machine-observed `EV_REL/REL_X/+1` followed by `EV_SYN/SYN_REPORT` after one submitted report; the one-shot path completed with no retry, inverse movement, or reconnect/resend. |
| Mouse buttons | `HARDWARE DEFERRED` | No accepted hardware evidence yet. |
| Wheel / pan | `HARDWARE DEFERRED` | No accepted hardware evidence yet. |
| Physical `report_failed` injection | `HARDWARE DEFERRED` | No accepted hardware injection or recovery evidence yet. |
| Physical HID-not-ready / timeout race | `HARDWARE DEFERRED` | Semantics are covered by native tests; physical race evidence is not established. |
| Long-duration soak | `HARDWARE DEFERRED` | No soak-duration claim. |

The corresponding native/CI coverage is authoritative in
[`validation-entrypoints.md`](validation-entrypoints.md); protocol and safety
semantics are authoritative in [`uart-control-plane.md`](uart-control-plane.md).

## U5.4 read-only event observer and F24 smoke

The observer/discovery and F24 orchestration are implemented and native-tested.
The dedicated physical F24 smoke gate is now **HARDWARE VALIDATED** on the
documented Freenove fixture: the canonical runner completed with exit `0`, and
Linux evdev machine observation recorded `KEY_F24` value `1` (DOWN) followed by
value `0` (UP). Both submissions were accepted, the successful run recorded
`allowed_repeat_count=0` (the tolerance path was not exercised), the
post-release quiet tail was clean, and final `release_all` cleanup succeeded.
This proves the F24 DOWN/UP event path, not that F24 is globally
side-effect-free. Mouse HID operation was not part of this run. Run the
no-hardware tests through `./tools/test-hardware-hid.sh`; they use fake sysfs,
event records, transport, and Client objects and do not access a board.

In physical mode the observer enumerates `/dev/input/eventN`, validates the
USB ancestor, VID/PID, product, interface number, and required capability,
then opens the selected keyboard node with `O_RDONLY` and drains pending
records before the F24 mode can construct the USB-UART transport. It never
uses `EVIOCGRAB`, writes to an input node, or changes host input state.
Ambiguous or incomplete discovery fails closed. The discovery-only mode still
opens and drains exactly one keyboard and one mouse node for the U5.4.1
baseline.

The default temporary bring-up identity is VID `0x303a`, PID `0x4008`, and
product `s3-hidbot`; `--vid`, `--pid`, and `--product` are explicit overrides
for a future identity. Output uses generic event-node paths and does not
print machine-local serial identifiers. An event override does not bypass the
USB ancestry, identity, interface, or capability checks.

The F24 smoke mode is explicitly selected with
`./tools/run-hardware-hid.sh --hardware --keyboard` and requires the serial
port from `--port` or `S3_HIDBOT_SERIAL`. The wrapper creates an ephemeral
virtual environment, installs the repository's `host/` package as a normal
distribution, and removes the environment on exit; dependency retrieval may
require network access, but the caller's Python installation, pip cache, and
pip build temporaries are not persistently modified.
An earlier physical attempt stopped at the runner environment precondition
because the host package was unavailable, before serial or HID activity. A
subsequent retry observed DOWN but failed while waiting for UP after an
unexpected `EV_KEY`; that run did not preserve the unexpected event's type,
code, or value, so it did not prove that the event was an autorepeat. The
runner's bounded repeat handling and immediate-UP timing were then validated
by the successful physical gate described above.

The runner shortens the held interval by submitting UP immediately after DOWN
is observed. During the bounded UP wait it tolerates only pre-release
`KEY_F24` value `2` events, records their count, and still requires a fresh
value `0`. At most two such repeat events are tolerated; a third is an
event-observation failure even if a release appears later in the same batch.
Any other key, modifier, unexpected press/value, or `SYN_DROPPED` remains
fail-closed. After the release is observed, the quiet tail remains strict: any
`EV_KEY`, including a repeat, fails.

Its bounded order is: discover the validated keyboard, open and drain the
read-only observer, construct/open one transport, connect one Client session,
submit one F24-down report, observe `KEY_F24` value `1`, submit one explicit
keyboard all-up report, observe value `0` (with only pre-release F24 repeats
tolerated), request one final `release_all`, then close the Client/transport
and observer. It does not use a subprocess per report, retry HID requests, or
replay a timed-out event. A down report that was accepted but not observed gets
one bounded best-effort all-up attempt before final cleanup. Any failure after
session start attempts at most one final `release_all`; the primary failure is
preserved and a cleanup-only failure is reported separately. `EV_SYN` records
are ignored, and bounded structured event evidence records phase, batch,
position, timestamp, type, code, value, and classification. It separately
reports the total observed pre-release repeat count and any repeat-limit event.
The evidence has a small fixed limit and reports truncation rather than growing
without bound.
The observer and runner remain `IMPLEMENTED / NATIVE VALIDATED`, while the
dedicated F24 DOWN/UP physical smoke is **HARDWARE VALIDATED**. Broader
physical scopes remain deferred below.

F24 is HID usage `0x73` / Linux `KEY_F24` (`194`). It is a diagnostic sentinel,
not guaranteed side-effect-free input. The accepted physical gate observed the
required DOWN and UP events and no unexpected keyboard or mouse side effects;
this remains a scoped fixture result, not a universal host-side guarantee.

## U5.4.3 relative mouse smoke

The dedicated `--hardware --mouse` runner path is now **HARDWARE VALIDATED** on
the documented Freenove fixture. The canonical runner completed with exit `0`
after discovering exactly one validated mouse interface (`interface=1`) with
`REL_X` capability, opening and draining its read-only event node, and
constructing one control session. It submitted exactly one relative report:
`buttons=0`, `x=1`, `y=0`, `wheel=0`, and `pan=0`; the control result was
`submitted`. Linux evdev machine observation recorded `EV_REL/REL_X/+1`
followed by the corresponding `EV_SYN/SYN_REPORT`, so both
`movement_observed=true` and `packet_complete=true` were established. The
physical evidence is `REL_X +1` — **HARDWARE OBSERVED** — and logical
`SYN_REPORT` packet completion — **HARDWARE OBSERVED**. This is distinct from
the existing Relative `REL_X` mouse report path evidence in the matrix above:
that row covers the accepted relative-report behavior, while this slice proves
the raw Linux evdev packet. The successful run had a clean quiet tail and
completed `release_all` cleanup with both devices already up. This physical
proof is the evdev movement and packet observation, not merely the
control-plane submission. The one-shot evidence
also showed no retry, reconnect/resend, inverse movement, or keyboard report.

The observer validates the logical Linux input packet rather than a read
buffer boundary. It requires one `EV_REL/REL_X/+1` followed by
`EV_SYN/SYN_REPORT`; `EV_MSC/MSC_SCAN` metadata is allowed. The physical run
did not encounter a split read; split-read handling remains covered by native
tests. A split read is nevertheless valid, while duplicate or wrong relative
events, key events, unsupported metadata, `SYN_DROPPED`, and other `EV_SYN`
values fail closed.
The bounded quiet tail remains strict and rejects any later input event.
Evidence records movement observation separately from packet completion, so a
`REL_X` seen without its `SYN_REPORT` is reported as observed-but-incomplete,
not as proof that no movement occurred. The raw evdev `REL_X` smoke is now
hardware validated, while mouse buttons, wheel/pan, physical report failure,
HID-not-ready races, suspend/resume or reconnect soak remain deferred.

## Low-interference sentinel policy

For a separately approved smoke test, use the smallest useful signals:

- Keyboard: F24, HID usage `0x73`. It is non-printable, but it is not promised
  to have zero host-side effects.
- Mouse: `buttons=0` and a small relative `REL_X` delta.

Avoid printable keys, modifiers, clicks, Back/Forward buttons, wheel/pan, and
large motion. A sentinel is a diagnostic choice, not a safety proof.

## BOOT-button diagnostic safety

GPIO0 is the active-low BOOT button and a boot strapping pin. Do not hold it
low during reset, power-on, flashing, or bootloader entry unless download boot
is explicitly intended. The controlled Mouse-report diagnostic is build-time
opt-in; default firmware does not configure GPIO0 or send HID reports in
response to a BOOT-button press.

## Related documents

- [`codex-runbook.md`](codex-runbook.md) — review gates and contributor
  procedure.
- [`validation-entrypoints.md`](validation-entrypoints.md) — canonical local
  and CI checks.
- [`uart-control-plane.md`](uart-control-plane.md) — normative protocol and
  runtime safety contract.
