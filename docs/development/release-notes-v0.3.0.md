# s3-hidbot v0.3.0

This feature release adds bounded low-level HID Sequence Executor v1 behavior
while preserving UART control protocol v1 and existing command compatibility.

Exact release source: `<source-revision>`

Highlights:

- the firmware can execute one bounded, prevalidated sequence of HID usages,
  mouse buttons, and waits using MCU-local monotonic timing;
- the host package adds immutable sequence values, a bounded builder, and
  typed start and status methods;
- the exact qualified F24 and left-button workload passed through both USB and
  BLE HID delivery with no unexpected event;
- native USB logical link loss now retires the old route authority, and link
  recovery does not resurrect it without a fresh explicit route selection;
- UART protocol v1 remains current, with Sequence Executor exposed as the
  additive optional `hid.sequence-v1` capability; and
- the USB Sequence, BLE Sequence, Native USB Logical Link-Loss, and existing
  BLE security, bond, and reconnect scopes have sealed physical evidence on
  the supported development fixture.

Firmware sequences are low-level HID programs, not keyboard-layout-aware text
entry, symbolic typing, or a general macro language. Their usages, button
values, waits, token count, duration, held-state behavior, and execution
deadline remain bounded by the documented control contract.

On the Freenove ESP32-S3 WROOM Board / FNK0085, the two USB connectors share
the board power rail and there is no independently observable native-port VBUS
signal. Native USB Logical Link-Loss is software and lifecycle fencing, not
electrical VBUS detection. Its 100 ms SOF-stall constant is not a universally
physically qualified unplug-detection latency.

Physical evidence remains scoped to the documented FNK0085 fixture and tested
Linux/BlueZ environment. It does not qualify arbitrary sequences, every host,
every board, other mouse buttons, wheel or pan, long-duration soak, or the
still-deferred complete fresh-to-retained BLE campaign.

s3-hidbot has no project-specific USB-IF VID/PID assignment or Bluetooth SIG
Company Identifier, and has not completed Bluetooth product qualification or
listing. Development identifiers and fixture evidence do not provide
certification, regulatory, trademark, manufacturing, redistribution, or
commercial authorization.

The `v0.3.0` tag is expected to be annotated and unsigned. Adjacent SHA-256
sidecars provide integrity comparison, not a signature, publisher
authentication, provenance attestation, secure-boot proof, or device
authentication.
