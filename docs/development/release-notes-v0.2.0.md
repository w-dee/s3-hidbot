# s3-hidbot v0.2.0 — published release

Status: **PUBLISHED**. The authoritative release is
[`v0.2.0`](https://github.com/w-dee/s3-hidbot/releases/tag/v0.2.0). Its
annotated tag object `a2522a90b6ad58fdfb075f6325102b576b1d636e` peels to source
commit `b2f87b8c52e155d1bd4acf0bc765f33dad172dfc`. Release build run
`33913669093` completed successfully, and the exactly 10 public assets were
re-downloaded and verified byte-for-byte against its output.

This development-branch copy was historicalized after publication. The
immutable v0.2.0 tag retains the pre-publication version of this file; updating
this copy does not change that tag, the published GitHub Release body, or any
public asset or checksum.

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

Final qualification on the repaired executable line completed with route
50/50, BLE reconnect 30/30, BLE retirement 30/30, reset persistence 5/5, and
no observed TinyUSB runtime diagnostic, queue overflow, malformed machine
frame, request-correlation anomaly, unexpected reset, recovery/lifecycle
fault, bond change, stale replay, dual delivery, automatic route restoration,
or cleanup failure. The released resource gates were Application
`648,640 / 664,592` (margin `15,952`) and Static RAM `37,088 / 39,832`
(margin `2,744`).
