
---

# Authority-v4 qualification preflight and evidence-set amendment

This amendment supersedes any earlier qualification-plan implication that a
safe running firmware instance already has the required Q1 profile selected.
It also supersedes the earlier single-unconditional-Q8-evidence-object terminal
model. All product acceptance criteria and Q1 through Q11 workloads remain
unchanged.

## Mandatory application-reset normalization

Before every future official `QUALIFICATION_ATTEMPT_START`, the protected
preflight must:

1. verify the exact frozen artifact and running product identity;
2. acquire the UART control plane;
3. issue exactly one controlled application reset, even if the current profile
   is already `strict_composite`;
4. reacquire a fresh post-reset UART session and observe a new boot ID;
5. reverify the exact frozen source, ELF, build profile, target, protocol and
   SDK identity;
6. require stable selected profile `strict_composite`;
7. require route stable none, an already-up `hid.release_all` result, no active
   Sequence by the new boot boundary, no pairing transaction or connection,
   BLE hidden in the boot-contract uninitialized/idle state with no recovery
   fault, and native USB hidden with no readiness or safety debt;
8. require the host to remain headless, the adapter powered but neither
   discoverable, pairable nor discovering, and no connected Bluetooth device;
9. verify a healthy, unchanged firmware bond inventory without deleting it.

When the safe boot state is BLE-uninitialized and the bond store therefore
returns `BLE_NOT_READY`, the preflight may briefly initialize BLE solely to read
the non-secret inventory. It first powers off the unique BlueZ adapter to
prevent the retained host from reconnecting, must observe no connection or
pairing, immediately hides BLE again, restores the original adapter power, and
re-establishes hidden idle with no lifecycle fault before it continues. This
bounded inventory probe is preflight setup and creates no phase or qualification
evidence.

The reset occurs after initial identity verification and before the attempt
snapshot/package and start boundary. Failure of reset, control recovery,
post-reset identity, strict default, or any safety postcondition is a preflight
failure. It creates no official attempt, emits no start boundary and must never
be replaced by selecting the strict profile through the control protocol.

The normalized preflight result is embedded in the official attempt's terminal
test details. It is setup evidence and does not replace any Q1 workload or
acceptance criterion.

## Phase execution journal

The coordinator initializes Q1 through Q11 as `NOT EXECUTED`. A phase becomes
`RUNNING` immediately before its protected process is invoked and then becomes
exactly `PASS` or `FAIL`. After the first required failure, every later phase
remains `NOT EXECUTED`.

## Privileged evidence-set activation

Privileged evidence is an initially empty set. Q8's protected
`package_request()` call is the immutable root-journal activation boundary for
its HCI evidence object. Capture or verification is rejected before activation,
and activation is rejected after the terminal test/evidence journal exists.

At terminalization:

* when no evidence object was activated, the empty set is `FINALIZED`, its
  receipt and raw evidence are null, and a failed test commits as `TEST_FAILED`;
* when Q8 activated an object, that exact request must produce an authoritative
  finalized receipt or the overall code is `EVIDENCE_FINALIZATION_FAILED`;
* a finalized Q8 object remains valid when Q8 or a later phase fails, producing
  `TEST_FAILED` while preserving the receipt;
* test `FAIL` is immutable and cannot become `PASS` during prepare, seal, crash
  recovery or retry;
* `SUCCESS` for an official attempt still requires all Q1 through Q11 phases to
  pass, including Q8 and its finalized privileged evidence.

Evidence belonging only to a `NOT EXECUTED` phase is never required. Historical
authority-v3 attempts and their terminal classifications remain immutable.
