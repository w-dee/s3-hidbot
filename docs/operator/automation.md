# Automation contract

## Normative rules for agents and scripts

1. Read-only UART diagnostics (`hello`, `ping`, `info`, `usb-status`, `usb-exposure-status`, `ble-exposure-status`, and
   `verify-firmware`) may run without unsafe-HID authorization once the fixture
   is in scope.
2. `usb-attach` and `usb-detach` are explicit lifecycle operations, never an
   implicit prerequisite of diagnostics or `self-test`. After either result,
   establish a fresh control session before later commands.
3. USB exposure never selects HID output. Require explicit `hid-route-set usb`
   only after mounted/both-endpoint readiness, then establish a fresh session.
   Use `hid-route-set none` to retire HID output without detaching USB.
4. `release-all` and `self-test` are safety actions, not arbitrary HID
   injection; `self-test` includes `release-all`.
5. Destructive provisioning requires explicit provisioning authorization.
6. `keyboard-report` and `mouse-report` require explicit human authorization
   and command-local `--unsafe-hid`.
7. Never infer host-OS consumption from `submitted`.
8. Never outer-retry an unsafe HID command after an ambiguous result.
9. Never automatically erase flash, change baud, mutate the plan, or repeat
   flash indefinitely.
10. `flash-firmware` owns at most three identical programming attempts.
11. Once JSON reports `flash.classification:"FLASHED"`, a verification failure
   must not trigger reflash; use `verify-firmware` with the same artifact when
   appropriate.
12. Stop for human review on unresolved exit 8, unresolved post-flash exits
    3–7, provenance doubt, serial ambiguity, unknown dual-cable topology, or
    HID uncertainty, `host_release_uncertain`, or `recovery_required`.
13. Never commit or share machine-local serial paths or local configuration.
14. Treat `ble-enable` and `ble-disable` as BLE exposure only. They do not
    select or revoke the USB HID route/session, and `route=ble` remains invalid.
15. U7.3 permits simultaneous USB and BLE exposure but never BLE HID output;
    do not infer report delivery from advertising, connection, or subscription.

See [safety and recovery](safety-and-recovery.md) for the required bounded
responses and [CLI reference](cli-reference.md) for exact syntax.

## JSON contract

Use `--json` and consume both stdout JSON and the process exit status. Do not
scrape human-readable output, retry messages, or stderr; stderr is diagnostic
text, not a stable schema.

On successful command execution, stdout contains one compact JSON object.
Most configuration, artifact, transport, protocol, and programming errors may
produce empty stdout and diagnostics on stderr.

Post-programming verification is the important exception. If programming
succeeds but verification fails, JSON still reports a phase-aware result:

```json
{
  "ok": false,
  "classification": "FLASHED_VERIFICATION_FAILED",
  "flash": {"classification": "FLASHED"}
}
```

Its nonzero exit (3–7) is not a programming failure. Do not reflash
automatically. For `verify-firmware`, `ok:true` and `match:false` means the
comparison completed successfully but the identities did not match. Inspect
the exit status, `match`, and `classification` together.

## Evidence record

For every provisioning, upgrade, or incident, retain:

- timestamp and logical fixture/board identifier;
- source revision plus Actions run identifier, or future release identifier;
- outer firmware artifact SHA-256;
- firmware version, application ELF SHA-256, build profile, target, IDF
  version, and protocol version;
- host package version, Python version, and esptool version when flashing;
- command, exit code, JSON classification, and JSON stdout;
- flash and reconnect attempts when present;
- mismatch list or unavailable reason; and
- sanitized stderr.

Do not put the exact machine-local serial path in shared project data or bug
reports.

## Stable and development acquisition

When a published version is available, acquire versioned assets from its
GitHub Release and record the tag plus source revision. For development or an
unreleased commit, acquire matching temporary Actions artifacts by exact
commit SHA and run ID. The host package remains unavailable on PyPI. In either
path, SHA-256 is byte-integrity comparison against the adjacent checksum; it
is not a signature, independent publisher authentication, attestation,
secure-boot proof, or device authentication.
