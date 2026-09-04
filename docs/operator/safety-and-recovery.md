# Safety and recovery

## USB and HID boundary

BLE exposure is an independent U7.3 control plane. It is uninitialized and
non-advertising at boot; explicit enable may advertise/connect one peer, and
disable retains the initialized stack in hidden idle. USB and BLE may be
exposed together. BLE exposure never grants HID authority; route v2 separately
selects an already eligible BLE peer only from stable none. Direct USB/BLE
switching, queued selection, and reconnect auto-restore are forbidden. A BLE
`fault` with `recovery_required:true` requires reboot/human review rather than
fabricated recovery.

Bonded hosts may cache the GATT database and HID Report Map across firmware
updates. Firmware records the cache-relevant schema revision per resolved bond.
An older or legacy bond remains HID-route-ineligible while stale; after normal
authenticated reconnect it receives a bounded standard Service Changed
request. A small schema-epoch service before HID changes the HID service handle
tuple, allowing supported hosts to discover a fresh HID instance. The peer
becomes eligible only after it actually reads the current Report Map and the
revision is durably verified. This mechanism is designed to preserve bonds and
does not select a route, but it is not a universal cross-host compatibility
claim until each host stack is qualified. Clearing BlueZ cache, deleting a host
pairing, or erasing device NVS is not the normal repair procedure and must not
be used as an automatic recovery step.

Bond administration is explicitly scoped to firmware-side state. The device
stores at most three bonds and never evicts automatically. `ble-bond-list`
returns opaque exact IDs without addresses or keys. `ble-bond-remove BOND_ID`
is permitted only with BLE initialized and disabled to hidden idle, no BLE
route retirement in progress, no connection or pairing transaction, and a
trustworthy store. It removes only that peer's `OUR_SEC`, `PEER_SEC`, and HID
schema revision, verifies absence, and preserves every other bond. A partial
failure becomes fatal Storage/recovery state and is not reported as success.
The operation does not remove the host OS's separate pairing record; never
infer BlueZ cleanup from firmware success.

The CH343 USB-UART path controls and programs the fixture. The separate native
USB-OTG path presents HID to a DUT host. Provisioning and identity verification
need only CH343. VBUS sourcing, backfeed, general dual-cable power safety,
immediate detach sensing, and board-specific VBUS monitoring are **UNKNOWN**.
Avoid a dual-cable topology unless it has been deliberately validated.

Native USB HID is hidden at boot. CH343 UART is the bootstrap/control path;
`usb-attach` explicitly creates a fresh TinyUSB driver instance and
`usb-detach` performs lifecycle-owned all-up safety handling before public
driver uninstall. Neither accepted command response proves USB enumeration,
uninstall completion, or host-observed all-up. Poll `usb-exposure-status` for
the authoritative lifecycle state; `usb.status` remains only the legacy/basic
readiness view. Repeated attach/detach can allocate and free TinyUSB resources.
Exposure and HID output selection are independent. Mount/resume never selects
USB; `hid-route-set usb` is required, and an actual selection requires a fresh
hello before unsafe reports. `hid-route-set none` performs any required all-up
on the old route and leaves the USB device mounted. BLE retirement first
publishes `releasing`, makes one best-effort all-up attempt per interface,
allows 100 ms for delivery, and reaches stable none only after exact physical
loss. Stack acceptance is not peer acknowledgment.

Detach preserves fail-closed uncertainty. `host_release_uncertain:true` means
the old host state could not be proven released; a later attach requires a
fresh-generation all-up reconciliation before unsafe HID can proceed. A true
`recovery_required` means a public install/uninstall may have crossed a partial
failure boundary: no automatic retry, reconnect, or stack surgery is allowed.
Use reboot or manual operator intervention. `self-test` does not attach USB or
select an HID output route.

`keyboard-report` and `mouse-report` are unsafe HID injection. They require
explicit human authorization and command-local `--unsafe-hid`. Raw usages—not
symbolic key names—are accepted. A successful command does not automatically
release keyboard, modifier, or mouse-button state. `submitted` means the
fixture accepted report bytes, not that the host OS consumed an event.

Use `release-all` for explicit recovery. Never outer-retry an unsafe report
after a timeout, session loss, client close, or other ambiguous outcome. The
fixture lease/session/lifecycle mechanisms invalidate unsafe authority and
drive internal safety handling, but they do not prove immediate release of
state a host OS may already believe is pressed.

## Supported safe/quiescent state

The supported quiescent target is an observable stable route `none`, no held
device-side keyboard or mouse-button state, and BLE hidden in `idle` with no
advertising or connection. Native USB may remain mounted because exposure and
normal HID authority are separate.

1. Establish a fresh hello session and inspect route and exposure status.
2. Run `release-all`, then request `hid-route-set none`.
3. Poll `hid-route-status` until `desired:none`, `active:none`,
   `transition:stable`, and `ready:false`.
4. Run `ble-disable`, then poll `ble-exposure-status` until `desired:hidden`,
   `observed:idle`, `advertising:false`, and `connected:false`.
5. Stop if `recovery_required:true`, host-release uncertainty persists, or the
   state does not converge. Reboot/manual diagnosis may be required; NVS erase
   and remove-all Bluetooth cleanup are not routine recovery.

If a response was lost, first create a fresh session and read state. Do not
blindly replay a normal keyboard or mouse report: timeout, transport loss,
session loss, or asynchronous `report_failed` can leave delivery ambiguous
even though unsafe authority has been revoked.

## Operator-visible error taxonomy

These are the exact public remote codes and pairing result terms emitted by
the current implementation. A correlated remote response is CLI exit 5;
transport, session/protocol, and timeout exits remain 3, 4, and 6.

| Exact term | Safe response |
| --- | --- |
| `SESSION_MISMATCH`, `CLIENT_NONCE_CONFLICT` | Do not replay under old authority. Establish a fresh hello session and re-read state. |
| `HID_NOT_READY`, `HID_BUSY`, `HID_SAFETY_PENDING`, `HID_ROUTE_V2_REQUIRED` | Read route/exposure state and wait for the required lifecycle convergence. Do not direct-switch USB/BLE or replay ambiguous HID. |
| `BLE_NOT_READY` | Initialize or converge BLE using the documented exposure flow, then re-read state. Repeated failure requires diagnosis. |
| `BLE_PAIRING_NOT_PENDING`, `BLE_PAIRING_FAILED` | Re-read pairing status and use only its current non-null pairing ID. Never reuse a stale passkey transaction. |
| `BLE_BOND_NOT_FOUND`, `BLE_BOND_AMBIGUOUS`, `BLE_BOND_BUSY` | Re-list bonds and verify exact identity/eligibility. Ambiguity or busy state requires operator/lifecycle resolution, not a broader delete. |
| `BLE_BOND_STORAGE` | Stop. Persistent state is not trustworthy; require operator diagnosis and follow `recovery_required` if set. |
| `INVALID_REQUEST`, `INVALID_PARAMS` | Correct the request; unchanged retry is not useful. |

`ble-pairing-status.last_result` uses lowercase values. `store_full` preserves
all existing bonds: an operator may later authorize removal of one exact,
eligible opaque bond ID, but firmware never evicts automatically. `storage`
and `queue_overflow` require diagnosis and may accompany
`recovery_required:true`. `smp_failed`, `timeout`, `peer_disconnected`,
`repeat_pairing`, and `security_policy` are connection/transaction outcomes;
re-read status and establish a new explicit pairing attempt only after the
cause and lifecycle state are understood. `succeeded` is the only successful
terminal result; `none` is not success.

Transport exit 3 is retryable only after the UART cause is resolved and a
fresh session is established. Timeout exit 6 is not blanket replay authority.
Read-only diagnostics may be repeated after state inspection, but any normal
HID report with an ambiguous outcome must not be blindly retried. A lifecycle
`recovery_required:true` or non-null fatal `last_error` requires operator
intervention rather than automatic loops.

## External identifiers, qualification, and distribution responsibility

s3-hidbot has not obtained a project-specific USB-IF VID/PID assignment or a
project-specific Bluetooth SIG Company Identifier. It has not completed
Bluetooth product qualification or listing for this project. v0.1.0 does not
implement BLE HID.

Any USB identifiers present in development firmware are solely for development
and interoperability testing. They do not represent a project-owned USB-IF
allocation or certification, a production or commercial identifier allocation,
or identifiers suitable for a user's product or redistribution. The presence
of current or future Bluetooth functionality in the source does not represent
Bluetooth SIG qualification or listing, trademark authorization, or assignment
of a project-specific Company Identifier.

The existing MIT License does not add a non-commercial-use restriction.
Open-source copyright permission is separate from external identifier,
qualification, listing, regulatory, trademark, membership, and other
authorization obligations. Anyone incorporating, redistributing, manufacturing,
selling, or otherwise using this software or firmware is responsible for
determining and obtaining the authorizations required for their intended use.
Requirements depend on intended use, product configuration, jurisdiction,
distribution model, and applicable rules. Publishing this firmware does not
provide those approvals. This statement is not legal advice.

## Destructive provisioning policy

`flash-firmware ARTIFACT` is destructive provisioning. It accepts only the
supported verified FNK0085 plan and requires `s3-hidbot-host[flash]` with
`esptool >=4.12,<5`.

- Programming owns at most three identical attempts, each bounded to 300
  seconds.
- It never automatically erases flash, changes baud, mutates the plan, falls
  back to another plan, or rebuilds.
- After programming succeeds, it **never automatically reflashes** because
  verification failed.
- Post-flash verification uses bounded reconnect, a fresh session, and exact
  runtime identity comparison over the CH343 UART.
- Exit 0 means `FLASHED_AND_VERIFIED` and `MATCH`; native USB is not required.

## Bounded recovery decision tree

| Situation | Bounded response |
| --- | --- |
| Artifact verification fails | Stop. Reacquire the intended artifact/checksum/run. Do not flash. |
| `[flash]` or esptool missing | Install the matching host package with `[flash]`; do not invoke a different flasher manually. |
| Serial port missing | Check the left CH343 connector, cable, enumeration, permissions, and selected port. Do not substitute native USB or speculate with baud changes. |
| Serial port busy | Close monitor, IDE, or other tty owner, then retry after contention is removed. |
| Exit 8 | Stop. The internal programming budget is exhausted. Do not blindly retry, erase, change baud, or mutate the plan. Preserve diagnostics and request review. |
| Flash exits 3–7 with `flash.classification:"FLASHED"` | Programming already succeeded. Do not reflash automatically. Repair UART selection/access if applicable, run `verify-firmware` with the **same** artifact, and preserve the result. |
| Exit 7 / identity mismatch | Compare the intended Actions run/artifact identity with reported device identity. Stop for review rather than reflashing. |
| Unsafe HID uncertainty | Run `release-all`. If safety cannot be established, keep route `none`, stop unsafe automation, and require human review. |
| BLE pairing `last_result:"store_full"` | Preserve all bonds. Re-list, identify an eligible exact bond, and obtain explicit removal authorization; never auto-evict. |
| BLE pairing `last_result:"storage"` or `"queue_overflow"` | Stop automatic pairing/retry and diagnose persistent state or lifecycle recovery. |
| Firmware bond removed but host record remains | Do not infer host cleanup. A separately authorized operator action may be required on that one host record. |
| `usb.exposure.status.recovery_required:true` | Stop. Do not retry attach/detach automatically; reboot or obtain manual operator intervention. |
| `usb.exposure.status.host_release_uncertain:true` | Stop unsafe HID. A later authorized attach must first complete fresh-generation all-up reconciliation. |

No unchanged automatic outer retry loop is allowed. After a concrete root cause
is identified and corrected, a human may explicitly authorize a new programming
operation.

## Upgrade procedure

```text
new intended artifact
  -> verify checksum and verify-artifact
  -> flash-firmware
  -> require exit 0 + FLASHED_AND_VERIFIED + MATCH
  -> retain evidence
```

If programming completed but verification did not, repair the verification path
and use `verify-firmware` against the same artifact; that command is not
automatic reflash authority.

## Physical-validation scope

The accepted provisioning and host-side evdev evidence is Linux-first. F24
keyboard and a small relative mouse movement have scoped physical evidence;
named Android devices have only the BLE peer evidence stated in the hardware
matrix. No general Android, macOS, or Windows qualification is claimed. Mouse
buttons, wheel/pan, failure races, long soak, and electrical conclusions remain
deferred. The detailed matrix is in
[hardware validation](../development/hardware-validation.md).
