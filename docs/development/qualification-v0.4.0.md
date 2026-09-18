# v0.4.0 qualification record

Status: **PASS — release preparation evidence**

This record identifies the exact product snapshot and official physical
attempt used for the v0.4.0 release gate. The physical scope is one
owner-identified Freenove FNK0099 ESP32-S3 WROOM Board Lite and the tested
Linux/BlueZ host. It does not qualify every FNK0099 variant, host, or commercial
BLE HID product.

## Product authority

| Item | Authority |
| --- | --- |
| Product version | `0.4.0` |
| Qualified source commit | `137d489d3d1219b203f84633cf8b570d9fe9f19a` |
| Root tree | `929f22a9510e2b0e0ca36cb943796ef7532940e7` |
| Firmware tree | `26819663473a44fa18538bc754f0f188e92b5a1c` |
| Host tree | `9cd778f8cb96fa9a22b8c8be628d3a9f9f0f4953` |
| Source date epoch | `1789735191` |
| Target/profile | `esp32s3` / `freenove-fnk0099` |
| Firmware archive SHA-256 | `2e6b5a1a20a83e20cfeafa2d10835ddb605f4aff0319225b48e340c856b39a35` |
| Application BIN SHA-256 | `8b0a2e4aed27d12ad3644c1e4d02c121890f3d3066075fb7e23acdfdaffd4315` |
| Application ELF SHA-256 | `0a0ac1ded06b1dddce6d014fc1127f9a49f65c23dd289ae89929c4dc41c16c45` |

Two isolated production-SDK builds produced byte-identical archives. The
application uses 706864 bytes of the 738320-byte gate, leaving 31456 bytes.
Static RAM is 40784 bytes of the 72600-byte gate, leaving 31816 bytes.

The production SDK is ESP-IDF v5.5.4 revision
`735507283d5b2f9fb363a1901172dbd9e847945d`, with controller revision
`ebd6043a8e3c3bbde45ee483895303b9c1229ab5`, NimBLE revision
`5c1a43ca3d205c3d5183e57dedce335fb6a81155`, and the locked DLE-LAST patch
SHA-256 `8af824b0c032dc308b8112f9a65ad6c84680674bce0e898bd1c7c60d6aecd126`.

## Attempt authority and result

| Item | Authority |
| --- | --- |
| Qualification authority commit | `37575af34276ec46a8008d8939e3f9a9502fd7fa` |
| Authority-v5 runtime ID | `0dd096ccf9e8e1070768b6daec0037616aafe124a47babf8f871e7cb4706ceae` |
| Coordinator SHA-256 | `0136b07b89d0ec542e4a3f226fe71311de05ce507a9bea6149e3e3ed0b49699c` |
| Plan SHA-256 | `be2fe55726996a8df77d6e2906f57b4be246cb4b615a07028f45902f724a8f3e` |
| FROZEN SHA-256 | `cb2998d2ccbfec8a707209b5078ea59b3c6ab9cc59000668186b63bcb1b0b585` |
| Attempt ID | `20260918T125918Z-064e384c7c5b` |
| Attempt classification | `OFFICIAL_FNK0099_V0_4_0` |
| Attempt Authority SHA-256 | `a7e7c6f949368c4bdb5264e9b790d0bd87068e47012ab1b3fd2101d10b502fa7` |
| Capture ID | `d2b49ee07e368852443f97a014e9074f` |
| Committed package index SHA-256 | `84a2b031d71183762ecadbf660dc7ba682770f331942187550d57ea9e0fbb323` |
| Result | `SUCCESS`; evidence `FINALIZED`; state `COMMITTED` |

The campaign consumed one of its two authorized new official attempts. Q1
through Q11 all passed in that attempt:

1. strict composite;
2. standalone Just Works mouse;
3. standalone authenticated keyboard;
4. Report-ID-7 mouse variation;
5. keyboard LED Output;
6. synthetic BAS/DIS/PnP metadata;
7. logical simulated sleep/wake;
8. host-initiated security and privileged HCI evidence;
9. all-profile switching;
10. USB independence matrix; and
11. representative reconnect/cache behavior.

No required phase was skipped or marked `NOT EXECUTED`. The Q8 root-owned raw
HCI capture is 4114 bytes with SHA-256
`eec5ad39a0cf1b81edc3500014fea80457a4396d14b58e4ed439502320675297`.
It contains one pairing request and one pairing response. The capture writer
was stopped, exited successfully, and was reaped before evidence finalization.

A separate read-only review verified the product, runtime, snapshot, phase
ordering, phase result bytes, raw evidence metadata, package hashes, console
terminal result, and final fixture state. A post-attempt firmware identity read
matched the qualified product; its evidence SHA-256 is
`6a244bf1438993b324739cfd0177ea2b2447cc069e85b21607c291c4030319c7`.

## Final fixture state

The device was left with route `none`, keyboard and mouse all-up, no active
Sequence, BLE hidden and idle, advertising off, no connection, USB hidden and
unmounted, and no recovery fault or safety debt. The BlueZ adapter was powered
but non-discoverable, non-pairable, and not discovering. Its single retained
bond remained healthy.

Earlier authority-v3 failed attempts remain preserved as historical evidence.
They are not rewritten into this authority-v5 PASS and are not counted as new
official attempts in this campaign.
