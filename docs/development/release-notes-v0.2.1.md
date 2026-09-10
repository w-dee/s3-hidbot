# s3-hidbot v0.2.1

This maintenance release preserves the v0.2.0 product boundary while adding
retained-bond reconnect hardening, exact bond-deletion recovery, stronger
release provenance, and host, package, qualification-appliance, and CI
maintenance.

Exact release source: `<source-revision>`

Highlights:

- retained bonded reconnect now uses the verified DLE-LAST host-call ordering;
- exact-target bond deletion persists a resumable transaction and fails closed
  during incomplete deletion or recovery;
- the retained bonded reconnect and the non-instrumented production
  retained-bond smoke passed on the supported physical fixture;
- no Pair fallback and no target controller Status/Reason `0x08` were observed
  in the accepted production retained reconnect;
- the production ESP-IDF source and DLE patch are locked, prepared in isolated
  SDK trees, and recorded in reproducible firmware artifacts;
- reference qualification-appliance tooling now has bounded, privacy-safe
  state and a dedicated host CI boundary; and
- post-v0.2.0 maintenance updates GitHub Actions runtimes, Python package
  metadata, and deferred PySerial imports.

Physical evidence remains scoped to the Freenove ESP32-S3 WROOM Board /
FNK0085 with ESP32-S3-WROOM-1 and its 8 MiB flash / 8 MiB PSRAM board
implementation. The canonical firmware minimum is 4 MiB flash and external
PSRAM is not required. Accepted evidence for specific Linux/BlueZ and named
lab Android peers does not qualify other boards, hosts, or operating systems.

Fresh pairing has a physical PASS. Fresh security-anchor, fresh DLE-LAST,
fresh stability, F-post, and combined fresh-to-retained physical PASS are not
established. The release therefore does not claim complete fresh-to-retained
qualification.

s3-hidbot has no project-specific USB-IF VID/PID assignment or Bluetooth SIG
Company Identifier, and has not completed Bluetooth product qualification or
listing. The development identifiers and physical evidence do not grant
certification, trademark, regulatory, manufacturing, redistribution, or
commercial authorization. Those obligations remain with each integrator.

The `v0.2.1` tag is an annotated, unsigned Git tag. Release SHA-256
sidecars provide integrity comparison, not a signature, publisher
authentication, provenance attestation, secure-boot proof, or device
authentication.
