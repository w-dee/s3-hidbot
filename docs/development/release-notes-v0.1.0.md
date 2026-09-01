# s3-hidbot <release-tag>

First public release of s3-hidbot for the Freenove ESP32-S3 WROOM Board /
FNK0085 provisioning profile.

- Firmware and host package version: 0.1.0
- Immutable source revision: <source-revision>
- Firmware bundle: FNK0085 / ESP32-S3 / ESP-IDF v5.5.4; use
  hidbotctl verify-artifact before any authorized programming operation.
- Provisioning uses the CH343 control connector and requires an exact
  post-flash identity MATCH. A verification failure never authorizes a reflash.
- Native HID is unsafe by design: keyboard and mouse reports require explicit
  command-local --unsafe-hid; use release-all for recovery.
- The two USB-C connector roles are distinct. Accepted physical validation is
  Linux-only. VBUS/backfeed/dual-cable behavior and immediate native-USB
  detach sensing remain **UNKNOWN**.
- There is no BLE HID, symbolic typing/clicking/macro layer, or PyPI release.
  HID inputs use raw values only.
- Download the firmware archive, host wheel, and adjacent SHA-256 files from
  this Release; compare each downloaded byte stream to its adjacent checksum.
  SHA-256 is an integrity comparison only: it is not a signature, independent
  publisher authentication, attestation, secure-boot proof, or device
  authentication.
- This v0.1.0 release is unsigned: there is no GPG tag signature,
  Sigstore/cosign signature, or provenance attestation.
- Review the separately attached LICENSE and THIRD_PARTY_NOTICES.md.
