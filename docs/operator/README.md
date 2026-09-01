# s3-hidbot operator documentation

This documentation is for someone operating an existing FNK0085 fixture or
automating one. It does not require ESP-IDF knowledge or a source checkout.

Start with the [Linux-first quick start](quick-start.md). It explains the
published-release and development-artifact paths, checksum verification,
host-wheel install, CH343 serial selection, verified provisioning, and the
evidence to retain.

Then use:

- [CLI reference](cli-reference.md) for command syntax, environment variables,
  JSON output, and exits;
- [safety and recovery](safety-and-recovery.md) before provisioning or any HID
  operation; and
- [automation contract](automation.md) for scripts and AI agents.

## Fixture boundary

Only the Freenove ESP32-S3 WROOM Board / FNK0085 is supported. With the board
front-facing and its ESP32-S3 module at the top, the left USB-C under EN/RST is
the CH343 programming/control port. The right USB-C under BOOT is native
USB-OTG/HID. Provisioning and `verify-firmware` use only the left port.

Do not infer electrical safety from this mapping. VBUS sourcing, backfeed,
general dual-cable power safety, immediate detach sensing, and a board-specific
VBUS monitor are **UNKNOWN**. Avoid dual-cable operation when it is not needed.

## Stable versus development distribution

When a published version is available, use its GitHub Release page for the
firmware archive, host wheel, adjacent checksums, `LICENSE`, and
`THIRD_PARTY_NOTICES.md`. The host package remains unavailable on PyPI.

Development artifacts come from a successful GitHub Actions run for an exact
commit SHA. `firmware-artifact` and `host-package` are temporary, 14-day
artifacts; they are not stable releases.

For protocol internals, fixture evidence, artifact format, and maintainer
validation, use the linked [development documentation](../development/codex-runbook.md).
