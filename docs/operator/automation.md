# Automation contract

## Normative rules for agents and scripts

1. Read-only UART diagnostics (`hello`, `ping`, `info`, `usb-status`, and
   `verify-firmware`) may run without unsafe-HID authorization once the fixture
   is in scope.
2. `release-all` and `self-test` are safety actions, not arbitrary HID
   injection; `self-test` includes `release-all`.
3. Destructive provisioning requires explicit provisioning authorization.
4. `keyboard-report` and `mouse-report` require explicit human authorization
   and command-local `--unsafe-hid`.
5. Never infer host-OS consumption from `submitted`.
6. Never outer-retry an unsafe HID command after an ambiguous result.
7. Never automatically erase flash, change baud, mutate the plan, or repeat
   flash indefinitely.
8. `flash-firmware` owns at most three identical programming attempts.
9. Once JSON reports `flash.classification:"FLASHED"`, a verification failure
   must not trigger reflash; use `verify-firmware` with the same artifact when
   appropriate.
10. Stop for human review on unresolved exit 8, unresolved post-flash exits
    3–7, provenance doubt, serial ambiguity, unknown dual-cable topology, or
    HID uncertainty.
11. Never commit or share machine-local serial paths or local configuration.

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
