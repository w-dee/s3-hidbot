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
- v0.1.0 does not implement BLE HID, symbolic typing/clicking/macro behavior,
  or a PyPI release. HID inputs use raw values only.
- Identifier and qualification notice: s3-hidbot has not obtained a
  project-specific USB-IF VID/PID assignment or a project-specific Bluetooth
  SIG Company Identifier, and it has not completed Bluetooth product
  qualification or listing for this project. USB identifiers in development
  firmware are solely for development and interoperability testing, not a
  project-owned USB-IF allocation or certification, a production or commercial
  identifier allocation, or identifiers suitable for a user's product or
  redistribution. Future Bluetooth source presence alone does not represent
  Bluetooth SIG qualification or listing, trademark authorization, or a
  project-specific Company Identifier assignment.
- The existing MIT License does not add a non-commercial-use restriction.
  Open-source copyright permission is separate from external identifier,
  qualification, listing, regulatory, trademark, membership, and other
  authorization obligations. Anyone incorporating, redistributing,
  manufacturing, selling, or otherwise using this software or firmware is
  responsible for determining and obtaining the authorizations required for
  their intended use. Requirements depend on intended use, product
  configuration, jurisdiction, distribution model, and applicable rules;
  publishing this firmware does not provide those approvals. This statement is
  not legal advice.
- Download the firmware archive, host wheel, and adjacent SHA-256 files from
  this Release; compare each downloaded byte stream to its adjacent checksum.
  SHA-256 is an integrity comparison only: it is not a signature, independent
  publisher authentication, attestation, secure-boot proof, or device
  authentication.
- This v0.1.0 release is unsigned: there is no GPG tag signature,
  Sigstore/cosign signature, or provenance attestation.
- Review the separately attached LICENSE and THIRD_PARTY_NOTICES.md.
