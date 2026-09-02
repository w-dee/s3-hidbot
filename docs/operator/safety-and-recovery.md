# Safety and recovery

## USB and HID boundary

BLE exposure is an independent U7.3 control plane. It is uninitialized and
non-advertising at boot; explicit enable may advertise/connect one peer, and
disable retains the initialized stack in hidden idle. USB and BLE may be
exposed together. BLE exposure never grants HID authority, never changes the
USB route/session, and never emits keyboard or mouse notifications. Pairing,
passkeys, bonding, and formal HOGP/security compliance remain deferred. A BLE
`fault` with `recovery_required:true` requires reboot/human review rather than
fabricated recovery.

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
on the old USB route and leaves the USB device mounted.

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

## External identifiers, qualification, and distribution responsibility

s3-hidbot has not obtained a project-specific USB-IF VID/PID assignment or a
project-specific Bluetooth SIG Company Identifier. It has not completed
Bluetooth product qualification or listing for this project. v0.1.0 does not
implement BLE HID.

Any USB identifiers present in development firmware are solely for development
and interoperability testing. They do not represent a project-owned USB-IF
allocation or certification, a production or commercial identifier allocation,
or identifiers suitable for a user's product or redistribution. If future
Bluetooth functionality appears in the source, that alone does not represent
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

The accepted fixture evidence is Linux-only. F24 keyboard and a small relative
mouse movement have scoped physical evidence; mouse buttons, wheel/pan, failure
races, long soak, and electrical conclusions remain deferred. The detailed
matrix is in [hardware validation](../development/hardware-validation.md).
