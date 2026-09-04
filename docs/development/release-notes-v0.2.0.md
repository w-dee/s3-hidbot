# s3-hidbot v0.2.0 — unreleased draft

Status: **RELEASE-CANDIDATE PREPARATION ONLY**. The source version is 0.2.0.
U7.6D physical soak, final release-candidate decision, tag, and GitHub Release
remain pending.

Changes completed on the development line include:

- explicit output route v2 (`none`, `usb`, `ble`) with stable-none transport
  switching, no dual normal delivery, and no automatic route restoration;
- authenticated BLE pairing with 16-byte keys and a fixed three-bond store
  that never evicts automatically;
- opaque exact-ID bond listing/removal, crash-safe companion metadata cleanup,
  and explicit separation from host-side pairing records;
- bonded HID/GATT cache repair based on a schema epoch and an authoritative
  current Report Map read;
- BLE keyboard/mouse notification delivery and fail-closed lifecycle,
  connection, identity, generation, and session fencing;
- reusable hardware-qualification orchestration and expanded native/static
  regression coverage; and
- release-candidate documentation, immutable Action pins, generic future
  artifact names, and machine-enforced firmware resource limits.

Physical evidence remains scoped to the Freenove ESP32-S3 WROOM Board /
FNK0085 with ESP32-S3-WROOM-1 and its 8 MiB flash / 8 MiB PSRAM board
implementation. The canonical firmware minimum is 4 MiB flash and external
PSRAM is not required. Accepted evidence for specific Linux/BlueZ and named
lab Android peers is not a claim that other boards, hosts, or operating
systems are qualified.

Final release wording and asset identities must be accepted only after the
subsequent authorized qualification gates.
