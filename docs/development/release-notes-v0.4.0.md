# s3-hidbot v0.4.0

This release adds a finite, capability-aware BLE HID fixture catalog while
preserving the existing USB behavior and bounded UART control contract.

Exact qualified product source: `<source-revision>`

Highlights:

- eight fixed BLE fixture profiles cover strict composite behavior, a
  standalone Just Works mouse, a standalone authenticated keyboard, a mouse
  using Report ID 7, a keyboard with LED Output, synthetic mouse metadata,
  simulated sleep/wake, and host-initiated security timing;
- the descriptor and GATT variations are selected only from that finite
  catalog. There is no arbitrary descriptor, packet, or ATT script injection;
- the LED profile exposes the host-written five-bit keyboard LED Output value
  through the control API without driving a physical board LED;
- the metadata profile exposes fixed Battery Service and Device Information
  Service/PnP values for interoperability tests. Those values do not describe
  the physical board or a measured battery;
- simulated sleep uses the normal bounded BLE disable and enable lifecycle to
  test logical sleep and wake. It does not put the ESP32-S3 into MCU deep
  sleep;
- the host-initiated security profile lets the host begin Just Works security
  after connection setup, while the other profiles retain their defined
  peripheral-initiated behavior;
- profile-specific bond, schema, and cache identities support retained-host
  reconnect and Service Changed/cache-refresh tests without silently treating
  incompatible profiles as equivalent; and
- an explicit `hid.release_all` on a healthy BLE route sends the neutral
  keyboard and mouse reports while retaining that route. Readiness or safety
  loss still follows the bounded fail-closed retirement path.

The release also includes the bounded HID Sequence Executor introduced on the
v0.3.0 line, native USB logical link-loss fencing, three-bond administration,
and artifact-only verification and flashing flows.

The exact v0.4.0 firmware archive was qualified on one owner-identified
Freenove FNK0099 ESP32-S3 WROOM Board Lite with Linux/BlueZ. One official
authority-v5 attempt passed strict composite, every finite BLE profile,
keyboard LED Output observation, synthetic BAS/DIS/PnP reads, logical
sleep/wake, host-initiated security with privileged HCI capture, profile
switching, USB independence, and representative reconnect/cache behavior.
This evidence is scoped to that fixture, artifact, host environment, and
workload. It is not general BLE HID certification or a claim of universal
commercial-device compatibility.

Deferred work includes RPA/privacy variants, directed advertising, multi-host
slot switching, BLE Boot/Protocol Mode, NKRO, Consumer/System Control, unusual
mouse formats, arbitrary descriptor or ATT scenario injection, and real MCU
deep sleep. These are outside the v0.4.0 compatibility envelope.

s3-hidbot has no project-specific USB-IF VID/PID assignment or Bluetooth SIG
Company Identifier, and has not completed Bluetooth product qualification or
listing. Development identifiers and fixture evidence do not provide
certification, regulatory, trademark, manufacturing, redistribution, or
commercial authorization.

The `v0.4.0` tag is intended to be annotated and unsigned. Adjacent SHA-256
sidecars provide integrity comparison, not a signature, publisher
authentication, provenance attestation, secure-boot proof, or device
authentication.
