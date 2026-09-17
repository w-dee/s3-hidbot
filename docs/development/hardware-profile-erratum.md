# Physical fixture identity erratum

## Correction and scope

Earlier s3-hidbot qualification documentation identified the physical
development fixture as Freenove FNK0085. The owner later identified that same
physical fixture as **Freenove FNK0099 — ESP32-S3 WROOM Board Lite**. This
erratum corrects the fixture name used to interpret those records.

The correction does not change the recorded behavioral observations. The
identified USB Sequence, BLE Sequence, Native USB Logical Link-Loss, UART,
composite USB HID, F24, relative-mouse, BLE security, bond, and reconnect
observations remain evidence about that same physical fixture within their
original firmware, artifact, host, peer, workload, and timing scope. The exact
source revisions, capsule identities, hashes, and limits remain authoritative
as recorded in [`hardware-validation.md`](hardware-validation.md).

This continuity is an owner-confirmed identity correction, not a new physical
qualification run. It does not qualify every FNK0099 unit or board revision,
and it creates no qualification evidence for actual FNK0085 hardware.

## Historical material remains literal

Frozen evidence objects, published release assets, manifests, hashes, asset
names, historical commands and output, release notes, and Git history remain
unchanged. In those objects, `FNK0085` and `freenove-fnk0085` are historical
literals. They must not be rewritten or interpreted as proof that an FNK0085
camera board was used.

At the time this erratum was introduced, the pre-H-contract firmware,
artifact, archive, release, and provisioning contracts still emitted or
required `freenove-fnk0085`. Forward source now emits `freenove-fnk0099`, while
historical artifacts retain the old literal. Runtime/artifact comparison
remains exact; the two profile values are not aliases.

## Fixture memory and product envelope

A non-destructive query of the owner-confirmed FNK0099 fixture detected 8 MiB
flash and reported the ESP32-S3 embedded-PSRAM feature as 8 MiB. The fixture
therefore corresponds to the FNK0099 N8R8 memory configuration. The query
preserved the installed firmware identity before and after it.

That measurement applies to this fixture. It does not assert that every
FNK0099 has the same flash capacity. The product firmware contract remains an
ESP32-S3 target with a minimum 4 MiB flash envelope and no required external
PSRAM. The physical fixture's additional capacity does not expand that
contract or its resource gates.

## Native USB VBUS observation

On the physical fixture now identified as FNK0099, while the USB-UART
connection continued powering the board, removing the native ESP32-S3 USB-OTG
cable did not provide an independently observable native-port VBUS-loss
condition usable by the product. Its provenance is
`DIRECT_PHYSICAL_OBSERVATION_ON_FNK0099`.

This observation does not establish schematic topology, direct VBUS-rail
connectivity, backfeed behavior, general dual-supply safety, or identical
behavior across every FNK0099 revision. No board schematic is an authority for
the observation. The existing Native USB Logical Link-Loss qualification
concerns logical USB lifecycle fencing; no independent electrical native-port
VBUS detector was established on the tested fixture.

## Manufacturer references and board facts

Freenove identifies FNK0099 as the ESP32-S3 WROOM Board Lite and documents its
CH343 programming/UART connection and separate native ESP32-S3 USB capability:

- [FNK0099 documentation](https://docs.freenove.com/projects/fnk0099/en/latest/)
- [FNK0099 Preface](https://docs.freenove.com/projects/fnk0099/en/latest/fnk0099/codes/C/Preface.html)
- [Freenove ESP32-S3 WROOM Board Lite resources](https://github.com/Freenove/Freenove_ESP32_S3_WROOM_Board_Lite)

Freenove also documents a monochrome blue onboard LED on GPIO2, active high
(`HIGH` on and `LOW` off), and one onboard WS2812/NeoPixel on GPIO48. These are
board facts, not new s3-hidbot LED functionality.

The official Lite pinout is useful as a physical-location reference, but the
inspected image reverses the GPIO19/20 USB D-/D+ labels relative to
[Espressif's ESP32-S3 mapping](https://docs.espressif.com/projects/esp-hardware-design-guidelines/en/latest/esp32s3/schematic-checklist.html).
s3-hidbot continues to treat GPIO19 as USB D- and GPIO20 as USB D+. The
Freenove image is not used as a schematic or as authority for electrical
topology.
