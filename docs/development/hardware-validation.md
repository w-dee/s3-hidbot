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

The validated development fixture is the Freenove FNK0099 ESP32-S3 WROOM
Board Lite with an ESP32-S3-WROOM-1 module. A non-destructive query of this
fixture detected 8 MiB flash and reported 8 MiB embedded PSRAM, corresponding
to its N8R8 configuration. This is a measurement of the fixture, not a claim
about every FNK0099 variant. The canonical firmware independently targets a
minimum 4 MiB flash and does not require external PSRAM; these minima do not
qualify other boards. The maintained
[fixture identity erratum](hardware-profile-erratum.md) corrects the earlier
FNK0085 label without rewriting historical evidence.

See the [official FNK0099 documentation](https://docs.freenove.com/projects/fnk0099/en/latest/),
[Freenove Lite resources](https://github.com/Freenove/Freenove_ESP32_S3_WROOM_Board_Lite),
[board photograph](https://github.com/Freenove/Freenove_ESP32_S3_WROOM_Board_Lite/blob/main/Board.png),
and [Lite pinout](https://github.com/Freenove/Freenove_ESP32_S3_WROOM_Board_Lite/blob/main/ESP32S3_Lite_Pinout.png).
The inspected Freenove pinout reverses the GPIO19/20 USB signal labels relative
to Espressif's mapping. This project uses GPIO19 as USB D- and GPIO20 as USB D+
and does not treat the Freenove pinout as a schematic.

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
- Freenove documents the monochrome blue onboard LED on GPIO2 as active high:
  `HIGH` is on and `LOW` is off. The existing firmware behavior matches this;
  this documentation adds no LED behavior.
- Freenove documents one onboard WS2812/NeoPixel on GPIO48. The project does
  not currently use it as product functionality.

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
| BLE HID route and repaired Report Map | `HARDWARE VALIDATED` | FNK0099 fixture with a Linux/BlueZ 5.72 host: exact repaired descriptor, F24 DOWN/UP, no repeat, stable route retirement, and bond-preserving reconnect. This is not a general Linux or HOGP-host claim. |
| Authenticated bond lifecycle | `HARDWARE VALIDATED` | FNK0099 same-fixture lab evidence covers authenticated pairing, 16-byte keys, exact opaque-ID removal, crash-safe persistent absence, stale host-record behavior, and exact slot reuse. |
| Three-bond capacity / no eviction | `HARDWARE VALIDATED` | One Linux/BlueZ peer plus named lab Xperia, Lenovo, and Moto Android fixtures were used across the accepted sequence: three verified bonds, `store_full` with the set preserved, exact removal, slot reuse, reconnect, and reboot persistence. No blanket Android/device qualification is claimed. |
| Mouse left button in HID Sequence Executor v1 | `HARDWARE VALIDATED` | The USB-only sequence checkpoint observed exactly one `BTN_LEFT` down/up pair after the F24 pair, with no extra input events and a final all-up cleanup. This does not qualify the other mouse buttons or arbitrary sequences. |
| BLE HID Sequence Executor v1 | `HARDWARE VALIDATED` | `PASS_BLE_SEQUENCE_EXECUTOR_PHYSICAL_QUALIFICATION` on firmware `c2dcfcb14d555da2c8d5b472f91dd5d2824af3a0` and artifact cache key `f711094a91eedc7cd3a38dc3d5285ad90d0046926cc1d68602c71758da2e8db3`: one accepted execution of the exact seven-token F24/left-button workload produced the four expected BLE evdev transitions in order and no unexpected event. This closes BLE delivery only for the scoped Sequence Executor v1 workload on the documented fixture; it does not replace the USB-only checkpoint or qualify arbitrary sequences. |
| Native USB Logical Link-Loss | `HARDWARE VALIDATED` | `PASS_NATIVE_USB_LINK_LOSS_PHYSICAL_CHECKPOINT` on firmware `c2dcfcb14d555da2c8d5b472f91dd5d2824af3a0` and artifact cache key `f711094a91eedc7cd3a38dc3d5285ad90d0046926cc1d68602c71758da2e8db3`: `PRIMARY_SUSPEND` fenced route generation 1 to stable none generation 2; signed host-remove-to-fence was -278.817 ms and the acceptance-transformed value was 0 ms against the frozen 250 ms limit. Reconnect did not resurrect the route, and fresh explicit selection recovered USB use. |
| Other mouse buttons | `HARDWARE DEFERRED` | No accepted hardware evidence yet for right, middle, backward, or forward buttons. |
| Wheel / pan | `HARDWARE DEFERRED` | No accepted hardware evidence yet. |
| Physical `report_failed` injection | `HARDWARE DEFERRED` | No accepted hardware injection or recovery evidence yet. |
| Physical HID-not-ready / timeout race | `HARDWARE DEFERRED` | Semantics are covered by native tests; physical race evidence is not established. |
| Long-duration soak | `HARDWARE DEFERRED` | No soak-duration claim. |

## Retained-reconnect production qualification

The exact transferred production source and artifact established the following
scoped results on the documented fixture:

```text
BONDED_RECONNECT_PRODUCT_BEHAVIOR_PHYSICAL_PASS = YES
NONINSTRUMENTED_PRODUCTION_SMOKE_PASS = YES
PRODUCTION_RETAINED_BOND_RECONNECT_PASS = YES
PRODUCTION_0X08_TIMEOUT_OBSERVED = NO
PRODUCTION_PAIR_FALLBACK_OBSERVED = NO
RETAINED_DLE_LAST_PHYSICAL_PASS = YES
RETAINED_STABILITY_PHYSICAL_PASS = YES
```

These results do not close the separate fresh-to-retained campaign. Preserve
its unestablished scopes exactly:

```text
FRESH_PAIR_PHYSICAL_PASS = YES
FRESH_SECURITY_ANCHOR_PHYSICAL_PASS = NOT_ESTABLISHED
FRESH_DLE_LAST_PHYSICAL_PASS = NOT_ESTABLISHED
FRESH_STABILITY_PHYSICAL_PASS = NOT_ESTABLISHED
F_POST_PHYSICAL_PASS = NOT_ESTABLISHED
COMBINED_FRESH_TO_RETAINED_PHYSICAL_PASS = NOT_ESTABLISHED
```

Accordingly, this repository does not claim a full fresh-to-retained
qualification PASS. Product retained-reconnect success was established before
the later stability/final-containment verdict; those are separate durable
campaign facts, so a later containment failure would not erase an already
established product verdict.

## USB-only HID Sequence Executor v1 checkpoint

The bounded USB-only sequence executor passed its scoped physical checkpoint
on the documented Freenove fixture:

```text
classification = PASS_USB_SEQUENCE_PHYSICAL_QUALIFICATION
firmware source authority = 7e19052f12d00ad2d2173b17911c24db0ab0c103
firmware subtree = c43486957327ab39fe98f8d304fcc697fbedae7a
qualification tooling authority = cfc9ead8f18180497bfb37352617baa14a3752ed
artifact cache key = 66c6f8b2c47f0346b9dc572adcb965fae7af1510e2e32fe8e091a7e0be09a667
artifact application SHA-256 = 80ccd1562e626c1197c14145c8c47e483c700a926527a31058a084009c544040
artifact archive SHA-256 = 4889315f3355b77b62c08a972c29ea9616df6934d00554f1e588bf1cddd19656
```

The committed firmware authority and qualification-tooling authority are
distinct. The mission wrapper was ephemeral evidence, not committed source:

```text
wrapper logical identity = usb-sequence-v1-physical-v2
wrapper SHA-256 = 6ffdd7c2f272ad11b16743e7367acbab4f1b4811b6d15d9c56bf7a825eee4a79
remote capsule identity = 20260911T123953Z-8ea9c296f7da
remote capsule runner digest = 54899e3bbe83afa6a4bcbbcd1e8d6cbbd5177a865d43f6a3642c455eaac1ac83
remote capsule lifecycle = PASS / SUCCESS -> RESOLVED -> ACKNOWLEDGED
```

The exact workload was
`w500;d150;kp115;kr115;w700;mpL;mrL`: seven tokens with a declared
scheduled duration of 1650 ms. Exactly one `hid.sequence.start` was issued,
with no retry. The accepted sequence ID was 1. Earlier preparation stopped
before sequence admission and did not consume another physical sequence
attempt. USB exposure was already mounted with both HID endpoints ready in
the passing capsule; the prerequisite attach had occurred once, without
retry, before that capsule.

The host made zero control calls during the 2401.049 ms post-admission silence
interval. Linux input observation then established exactly this order, with no
unexpected event:

```text
KEY_F24 down -> KEY_F24 up -> BTN_LEFT down -> BTN_LEFT up
acceptance -> KEY_F24 down = 443.242 ms
KEY_F24 down -> KEY_F24 up = 151.974 ms
KEY_F24 up -> BTN_LEFT down = 859.164 ms
BTN_LEFT down -> BTN_LEFT up = 143.969 ms
```

These observations satisfy the approved timing windows, including the
MCU-local long-gap criterion. They are not an exact 700 ms measurement because
the observer timestamps also include USB, host, and input-delivery latency.
The sole post-silence status returned `completed`, `started=true`,
`executed=7`, `failed_token=null`, and `code=null`.

Cleanup returned Keyboard and Mouse `already_up`, restored a stable `none`
route, and completed a clean 250 ms quiet tail. Final held state was `ALL_UP`,
the boot ID remained continuous, and no reset, panic, or fatal runtime fault
was observed. No flash occurred in the passing run.

This checkpoint covers whole-sequence admission, MCU-local timing, USB F24 and
left-button output, host-silence independence, terminal status, and final
all-up cleanup for this one workload. It does not newly qualify BLE execution,
pairing, retained reconnect, bond lifecycle, other sequence programs, other
mouse buttons, held-state cases, mid-sequence interruption, or invalid input.

```text
BLE_REQUALIFICATION_PERFORMED = NO
```

The corresponding native/CI coverage is authoritative in
[`validation-entrypoints.md`](validation-entrypoints.md); protocol and safety
semantics are authoritative in [`uart-control-plane.md`](uart-control-plane.md).

## BLE HID Sequence Executor v1 checkpoint

The bounded BLE HID Sequence Executor v1 passed its scoped physical checkpoint
on the documented Freenove fixture. This closes the previously deferred BLE
delivery qualification for this exact Sequence Executor workload. It does not
repeat or replace the earlier USB-only Sequence Executor checkpoint:

```text
classification = PASS_BLE_SEQUENCE_EXECUTOR_PHYSICAL_QUALIFICATION
firmware source authority = c2dcfcb14d555da2c8d5b472f91dd5d2824af3a0
artifact cache key = f711094a91eedc7cd3a38dc3d5285ad90d0046926cc1d68602c71758da2e8db3
artifact ELF SHA-256 = ffc8e35fb7740e6e44820e43f2e8ebf572987ed24d657d327a9f68e4c4a2aca9
```

The firmware source and cached artifact are the executable authorities. The
documentation commit recording this checkpoint is evidence-record authority
only and is not firmware source authority. No firmware or diagnostic build,
flash, source edit, or artifact mutation occurred during the qualification.

The qualification reused the existing bond. Before the attempt the target was
connected, encrypted, authenticated, bonded with Secure Connections and a
16-byte key, and had both Keyboard and Mouse CCCDs ready. The bond store was
healthy, all three existing records were verified, and no runtime fault was
present. Pairing, bond deletion, `RemoveDevice`, and NVS or other destructive
bond mutation were each zero.

The initial stable route was none at generation 4. Explicit selection reached
desired and active BLE, stable and ready, at generation 5. Independent
read-only BLE HID Keyboard and Mouse observers were selected without retaining
their device paths or Bluetooth address. Their 250 ms quiet baseline passed
with no relevant input event.

The exact workload was
`w500;d150;kp115;kr115;w700;mpL;mrL`: seven tokens with a declared
schedule of 1650 ms, covering F24 press/release followed by left-button
press/release. Exactly one physical Sequence attempt and one
`hid.sequence.start` were consumed, with no retry. Sequence ID 6 was accepted.

The host then made zero control-plane Client calls during 2400.687 ms of
sender silence. The independent observers recorded exactly this order and
timing:

```text
KEY_F24 down -> KEY_F24 up -> BTN_LEFT down -> BTN_LEFT up
acceptance -> KEY_F24 down = 473.631 ms
KEY_F24 down -> KEY_F24 up = 146.143 ms
KEY_F24 up -> BTN_LEFT down = 828.687 ms
BTN_LEFT down -> BTN_LEFT up = 146.237 ms
```

All intervals passed the frozen qualification windows. There were zero
unexpected events, repeats, mouse motion, wheel, pan, or `SYN_DROPPED` events.
After the final button-up, approximately 805.989 ms of the sender-silence
interval remained quiet. This establishes MCU-timed execution without host
polling or pacing for the qualified workload.

The sole post-silence status observation returned sequence ID 6,
`state=completed`, `started=true`, `executed=7`, `failed_token=null`, and
`code=null`. Boot continuity passed; no reset, panic, fatal runtime fault, or
recovery fault was observed.

Cleanup confirmed `release_all` results of Keyboard `already_up` and Mouse
`already_up`, then completed the generation-5 BLE to generation-6 none route
transition. The final route was desired none, active none, and stable; the
final HID state was `ALL_UP`. The bond and store remained unchanged, the host
Bluetooth adapter was restored from OFF to OFF, the physical lock was
released, and the post-run host doctor returned `HOST_DOCTOR_READY`.

Reversible environment repair reacquired fresh UART sessions after long BlueZ
and cleanup operations, corrected host-side sysfs capability-bitmap
interpretation, and handled an observer fd removed by BLE disconnection. The
single product attempt was not rerun, and the frozen acceptance contract was
unchanged.

Qualification accounting was:

```text
firmware build = 0
diagnostic build = 0
flash = 0
physical Sequence attempts = 1
hid.sequence.start = 1
pairing = 0
bond deletion = 0
ordinary keyboard reports outside the Sequence = 0
ordinary mouse reports outside the Sequence = 0
source edits during qualification = 0
artifact mutation = 0
```

This checkpoint qualifies BLE delivery, MCU-local timing, terminal status,
and final all-up cleanup for this exact Sequence Executor v1 workload on the
documented fixture. It does not newly qualify pairing, bond administration,
arbitrary Sequence programs, other mouse buttons, held-state cases,
mid-sequence interruption, invalid input, another host, or another board.

## Native USB Logical Link-Loss checkpoint

The post-checkpoint product implementation adds a software-only SOF-stall
safety fallback. TinyUSB suspend and unmount remain authoritative. Only while
the stable active HID route is USB does the existing control executor sample a
32-bit SOF heartbeat every 10 ms. One unchanged 100 ms interval causes an
exact-generation/authority claim followed by route, ticket, and authority
fencing. A stale watchdog snapshot cannot retire a replacement route or
invalidate its tickets/session.

If a route writer is temporarily busy, the SOF request is generation-scoped
and cancelable only by its exact watchdog token. Suspend, unmount, and runtime
fail-close invalidations use a separate durable generation-scoped handoff;
resume cannot discard a suspend retirement that arrived while the watchdog
held the writer.

Suspend, unmount, and runtime fail-close also publish a route-publication cut
before relying on a coherent route snapshot. If one overlaps an in-flight USB
route publication, that exact publication is aborted or retired before its
admission gate opens. A suspend/resume or unmount/remount round trip cannot
erase the cut; later reuse still requires a fresh explicit route selection.

This result means `USB link activity lost`; it does not mean that firmware
observed a physical cable removal, per-port VBUS loss, unmount, or suspend. On
the tested FNK0099 fixture, while USB-UART continued powering the board,
removing the native cable did not provide an independently observable
native-port VBUS-loss condition usable by the product. This is
`DIRECT_PHYSICAL_OBSERVATION_ON_FNK0099`, not a schematic or rail-connectivity
claim. The fallback publishes neither fake `mounted=false` nor fake
`suspended=true`, and normal SOF/lifecycle recovery does not restore the old
route.

The native USB logical link-loss physical checkpoint is now **HARDWARE
VALIDATED** on the documented Freenove fixture. It qualifies logical USB
link-loss fencing, not electrical native-port VBUS-loss detection:

```text
classification = PASS_NATIVE_USB_LINK_LOSS_PHYSICAL_CHECKPOINT
PER_PORT_VBUS_MONITORING = NOT_FEASIBLE_ON_CURRENT_BOARD
firmware source authority = c2dcfcb14d555da2c8d5b472f91dd5d2824af3a0
artifact cache key = f711094a91eedc7cd3a38dc3d5285ad90d0046926cc1d68602c71758da2e8db3
artifact archive SHA-256 = 5b2bff1af17356c1a6f817f2b8ab3fb1b27a6576d9d3cd14041c1b9eb76586fb
artifact application SHA-256 = fa8d972f26d4657d40553b78706a6c25c7da4c43b769f5a7e1302a83d952e192
artifact ELF SHA-256 = ffc8e35fb7740e6e44820e43f2e8ebf572987ed24d657d327a9f68e4c4a2aca9
artifact manifest SHA-256 = a2e4b1dc86d82ad1c21a579b7b37d2b536cccb8917d17e9b1ddd17681f9a12c5
application = 668416 / 672784 bytes; headroom = 4368 bytes
static RAM = 39520 / 39832 bytes; headroom = 312 bytes
```

The exact cached production artifact was flashed once, with no reflash. The
programming/control USB remained connected throughout one native cable-removal
attempt and one reconnect; there was no retry. Before removal, native USB was
exposed and mounted, not suspended, both endpoints were ready, and the route
was stable USB at generation 1. That state remained stable for the 250 ms
quiet baseline.

The host obtained `T0_HOST_REMOVE` from a passive kernel USB remove uevent
matched only to the native connection. Existing production UART output was
captured read-only at a 20 ms observation/ping cadence for the firmware cause
and already-effective post-fence signal. All external timestamps used the
host `CLOCK_MONOTONIC` clock; no firmware instrumentation was added. The
acceptance limits were frozen before removal:

```text
MAX_HOST_REMOVE_TO_FENCE_MS = 250
MAX_CAUSE_TO_FENCE_OBSERVATION_MS = 100
SOF_STALL_TIMEOUT_MS = 100
executor sampling period = 10 ms
T1_LAST_SOF_DIRECTLY_OBSERVABLE = NO
```

`PRIMARY_SUSPEND`, rather than the SOF-stall fallback, was the first firmware
cause. The observation-level timestamps and results were:

```text
T0_HOST_REMOVE = 210361049214770 ns
T2_CAUSE = 210360770397677 ns
T3_FENCE = 210360770397677 ns
SIGNED_L_HOST_FENCE = -278.817 ms
ACCEPTANCE_L_HOST_FENCE = max(0, SIGNED_L_HOST_FENCE) = 0 ms <= 250 ms; PASS
L_INTERNAL_OBS = 0.000 ms <= 100 ms; PASS
```

The signed result is retained: the firmware suspend/fence observation preceded
the host kernel remove uevent, which is a later external observation point and
not an electrical-edge timestamp. Cause and fence used the same production
post-fence signal, so `L_INTERNAL_OBS` is an observation-level delta, not zero
MCU instruction latency.

Suspend invalidated the old route authority and moved route generation 1 to
generation 2, stable none and not ready. The production cause arrived before
the next ping, so an explicit old-session `SESSION_MISMATCH` response was not
directly observed. The generation change, stable-none state, and successful
fresh session nevertheless established authority fencing and control-plane
continuity without reboot. Boot identity remained continuous; reset, panic,
and fatal runtime fault were not observed. Final runtime state retained
`recovery_required=false` and `last_error=null`.

After the single reconnect, mounted/unsuspended state and both endpoint-ready
signals recovered while generation 2 remained stable none for at least 250
ms. Thus readiness did not resurrect the old route. A fresh explicit
`hid.route.set usb` selected stable, ready USB at generation 3. Final cleanup
issued `hid.route.set none` once and reached stable none at generation 4, with
the native cable connected.

This narrow checkpoint issued no HID sequence, keyboard report, mouse report,
`release_all`, or BLE operation. Its distinct physical evidence authority is:

```text
wrapper logical identity = native-usb-link-loss-physical-v1
wrapper SHA-256 = 92edb31cda5de7a93885e1185d4be1e6f55132c9fd233ae305be08ae39221fcb
remote capsule runner aggregate = 5bed0c419b14c945b887fd911d734ea1a3f86a73af9dcae644145f450d14deb5
remote capsule lifecycle = PASS / SUCCESS -> RESOLVED -> ACKNOWLEDGED
```

Exact last SOF was not directly observable from the production artifact, no
independently observable electrical native-port VBUS-loss condition was
established on this tested FNK0099 fixture, and the old-session mismatch
response was not directly observed in this run. This does not establish a
direct-rail topology, backfeed behavior, dual-supply safety, or behavior of
every FNK0099 revision.
These are evidence limits, not qualification failures. Machine-local device,
session, lock, boot-identity, and filesystem identifiers and raw UART output
remain private.

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
explicitly select route USB, establish a fresh session, submit one F24-down
report, observe `KEY_F24` value `1`, submit one explicit
keyboard all-up report, observe value `0` (with only pre-release F24 repeats
tolerated), request one final `release_all`, return the route to none while USB
remains mounted, then close the Client/transport
and observer. It does not use a subprocess per report, retry HID requests, or
replay a timed-out event. A down report that was accepted but not observed gets
one bounded best-effort all-up attempt before final cleanup. Any failure after
session start attempts at most one final `release_all` and one route-none
cleanup when USB was selected; the primary failure is preserved and a
cleanup-only failure is reported separately. `EV_SYN` records
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
The current Model B runner explicitly selects route USB, establishes a fresh
session before the one relative report, then performs `release_all` and returns
the route to none without detaching USB.
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

## Historical U7.3 BLE qualification gate

The text in this section records the pre-U7.4/U7.5 gate and its early failure;
it is not current capability status. At that time, software implementation
stopped before the gate. The planned run was required to use the exact feature artifact and not infer
success from software tests. It must confirm cold boot is non-advertising,
explicit enable/service discovery, one connection, CCCD observation without
notifications, disconnect/re-advertise, stack-retained disable, five cycles,
and simultaneous USB exposure without route mutation. Pairing/bonding and BLE
HID output were out of scope for that historical gate. Current scoped BLE
evidence is recorded in the matrix above.

The first physical attempt at source revision
`dac08c7dfd9819c4a3dbb6e38b06ec227b369e37` remains **FAIL / SERVICE DB
MISMATCH**: BlueZ connected without pairing and reported services resolved,
but exposed an empty service collection. The repair candidate must therefore
prove the live local 0x1812 database before advertising and then repeat the
entire physical gate; software validation does not overwrite that historical
failure. Standard stack services 0x1800 (GAP) and 0x1801 (GATT, including
Service Changed) are expected infrastructure and are not project service
leakage. BAS, DIS/PnP ID, Protocol Mode, and Boot characteristics remain
forbidden.

BlueZ 5.72 exposed the live name, 0x1812 UUID, and appearance but not raw
advertising Flags or AD-type completeness, while HCI monitor access was not
available under the existing host permissions. A later requalification may
record **RAW AD FLAGS/TYPE PHYSICAL OBSERVABILITY WAIVED / SOFTWARE CONTRACT
VERIFIED** when those semantic live fields and the static encoded advertising
contract both pass. No privileged host configuration is required for this
waiver.

Firmware logs project-owned internal 8-bit heap checkpoints at cold boot,
immediately before first enable, advertising, connected,
disconnect/re-advertising, and hidden idle; repeated hidden-idle checkpoints
provide the fifth-cycle evidence. Each reports free, minimum-ever-free, and
largest block without an address or private identity. Review targets are at
least 80 KiB free/32 KiB largest while advertising and 64 KiB free/24 KiB
largest while connected. Until measured on the fixture these are **PENDING
PHYSICAL MEASUREMENT**, never a software PASS.

## Related documents

- [`codex-runbook.md`](codex-runbook.md) — review gates and contributor
  procedure.
- [`validation-entrypoints.md`](validation-entrypoints.md) — canonical local
  and CI checks.
- [`uart-control-plane.md`](uart-control-plane.md) — normative protocol and
  runtime safety contract.
- [`qualification-harness.md`](qualification-harness.md) — repository-owned
  bounded qualification primitives and privacy-safe evidence format for later
  approved physical gates.
