# s3-hidbot v0.2.0 — unreleased draft

Status: **PREPARATION ONLY**. The coordinated product version bump, exact
candidate artifact, final physical release-candidate qualification, soak, tag,
and GitHub Release have not occurred.

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

Final release wording and asset identities must be regenerated only after the
U7.6C2 coordinated version bump and subsequent authorized qualification gates.
