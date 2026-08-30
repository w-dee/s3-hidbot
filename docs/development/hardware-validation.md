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
| F24 keyboard report path | `HARDWARE VALIDATED` | F24/HID usage `0x73` sentinel report path with explicit release recovery; no claim that F24 is globally side-effect-free. |
| Relative `REL_X` mouse report path | `HARDWARE VALIDATED` | Small positive relative movement observed once per accepted report; no claim about pointer acceleration or exact screen pixels. |
| Mouse buttons | `HARDWARE DEFERRED` | No accepted hardware evidence yet. |
| Wheel / pan | `HARDWARE DEFERRED` | No accepted hardware evidence yet. |
| Physical `report_failed` injection | `HARDWARE DEFERRED` | No accepted hardware injection or recovery evidence yet. |
| Physical HID-not-ready / timeout race | `HARDWARE DEFERRED` | Semantics are covered by native tests; physical race evidence is not established. |
| Long-duration soak | `HARDWARE DEFERRED` | No soak-duration claim. |

The corresponding native/CI coverage is authoritative in
[`validation-entrypoints.md`](validation-entrypoints.md); protocol and safety
semantics are authoritative in [`uart-control-plane.md`](uart-control-plane.md).

## U5.4 read-only event observer and F24 smoke

The observer/discovery and F24 orchestration are implemented and native-tested,
but this slice is **not** a new hardware claim. The historical F24 row in the
evidence matrix records the separately accepted direct-report gate; the smoke
runner itself remains `IMPLEMENTED / NO-HARDWARE VALIDATED` until a dedicated
physical gate is approved. Run the no-hardware tests through
`./tools/test-hardware-hid.sh`; they use fake sysfs, event records, transport,
and Client objects and do not access a board.

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
`./tools/test-hardware-hid.sh --hardware --keyboard` and requires the serial
port from `--port` or `S3_HIDBOT_SERIAL`. Its bounded order is: discover the
validated keyboard, open and drain the read-only observer, construct/open one
transport, connect one Client session, submit one F24-down report, observe
`KEY_F24` value `1`, submit one explicit keyboard all-up report, observe value
`0`, request one final `release_all`, then close the Client/transport and
observer. It does not use a subprocess per report, retry HID requests, or
replay a timed-out event. A down report that was accepted but not observed gets
one bounded best-effort all-up attempt before final cleanup. Any failure after
session start attempts at most one final `release_all`; the primary failure is
preserved and a cleanup-only failure is reported separately. `EV_SYN` records
are ignored, `SYN_DROPPED` and unexpected key events fail closed, and a short
quiet tail catches duplicate or unrelated key events.

F24 is HID usage `0x73` / Linux `KEY_F24` (`194`). It is a diagnostic sentinel,
not guaranteed side-effect-free input. This implementation is no-hardware
validated only; a physical run requires a separate human gate and must verify
the host-side effects before being classified as hardware evidence.

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
