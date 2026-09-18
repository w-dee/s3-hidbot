# Qualification evidence authority, version 2

This is tooling authority for the owner-confirmed physical FNK0099 fixture.
It does not qualify firmware. The product freeze is commit
`8ca6a1e0ce9eea88ec15a716fffa89ffeff0bfad`; no product source or public host API
changes belong to this boundary. The failed official attempt and version 1
rehearsal remain immutable historical records.

## Installation and invocation

An owner installs reviewed bytes using `install_evidence_helper.py --frozen-bundle
BUNDLE`. The installer refuses different bytes in an existing installation;
updates require an explicit owner installation decision. It writes only the two
reviewed modules and root-object configuration under
`/usr/local/lib/s3-hidbot-evidence-v2`, and creates the separate root-owned 0700
`/srv/s3-hidbot-test/private/evidence-authority-v2` store. All installation
ancestors are root owned without group/other write permission. Module and
configuration files are root owned, single-link, regular and read-only.

The runtime command is exactly `/usr/bin/sudo -n /usr/bin/python3 -I -B
/usr/local/lib/s3-hidbot-evidence-v2/privileged_evidence.py OP RUN_ID CAPTURE_ID`.
OP is `capture` or `verify`. There is no shell, PATH lookup, caller-selected
script, caller-selected root, arbitrary raw path, synthetic writer CLI, or
installation action in the runtime protocol. The small environment fixes PATH,
LANG and LC_ALL. The shared module is compiled from the exact protected bytes
whose digest the producer reports; the helper reports its protected source
identity too. Root administration remains the installation trust boundary.

A run ID is ASCII `YYYYMMDDTHHMMSSZ-` plus twelve lowercase hex digits. A
capture ID is thirty-two lowercase hex digits, independent of the run ID.
Strict JSON stdin binds attempt, run, capture, evidence kind, frozen authority
digest, helper/import identity, schema and bounded duration. Duplicate keys,
unknown fields, missing fields, unsupported versions and bool-as-int fail.

## Namespace, object and writer authority

The producer opens every directory component with O_DIRECTORY and O_NOFOLLOW,
retains each FD, and checks that canonical entries still name those objects.
The configured root device/inode is pinned in the protected installation.
Run and capture directories are root owned 0700. A capture transaction flock
prevents simultaneous finalization or verification of a live writer.

A new raw object is created O_EXCL, mode 0600. The producer retains that exact
FD from creation through writer completion, hashing and decoding. btmon gets
`/proc/self/fd/N` with that FD explicitly inherited. All other streams are
/dev/null. Finalization validates regular-file type, owner, link count and
canonical entry identity before any fchmod. Hashing checks full before/after
stat signatures, and checks canonical entry and FD identity again after hashing
and decoding. Replaced parents, replaced raw entries, unexpected links, or
changed bytes cannot yield a valid receipt and cannot redirect outside chmod.

The producer owns the Popen lifetime. Normal accepted stop is requested SIGINT,
exit code **0**, reaped, no earlier exit, and no forced signal. The bounded
fallback is SIGTERM then SIGKILL; either makes evidence FAILED while preserving
forensic bytes/digest when object validation succeeds. Early exit and exit 23
are failures. EOF/deadline also stop and reap but cannot count as a requested
normal stop. Linux parent-death SIGKILL prevents the writer outliving a killed
producer; recovery never signals an arbitrary numeric PID. PID, terminal state,
and capture ID are recorded together, never used as independent recovery
permission. Root btmon decoding returns only the three Q8 counters, no HCI text.

## Capture recovery

`request.json` is exclusive/equality-on-retry before launch. `stopped.json` is
persisted after reaping and contains object, final stat signature and writer
outcome. `receipt.json` is durable after final FD validation. Capture IDs cannot
start a second writer.

| Interruption | Retry |
| --- | --- |
| Before a persisted request | No capture authority; package may record evidence FAILED |
| Request exists, no stopped journal | Immutable CAPTURE_INTERRUPTED failure; never recapture or certify unknown writer |
| Writer stopped but stopped journal absent | Same explicit failure; bytes retained |
| Stopped journal exists, no receipt | Reopen only in pinned directory; require recorded inode/stat and reaped status, then finalize |
| Receipt exists | Recompute through the same stopped authority and require exact equality |
| Writer still active | Nonblocking flock rejects verification; no success receipt |
| Namespace/object changed | Reject; retain forensic objects; never substitute new bytes |

## Package recovery and terminal authority

The ordinary package contains only `authority.json`, immutable `test.json`,
`evidence.json`, PREPARED `manifest.json`, PREPARED `index.json` and its lock.
Phase JSON outcomes are embedded in `test.json`; raw digest/size/object come
from the privileged receipt, never a recursive raw-file hash. There is no
caller-supplied receipt parameter on production finalization. It always invokes
the fixed producer to reverify. A persisted evidence failure never upgrades.

Under the package flock, finalization persists the exact test outcome first.
Retrying FAIL as PASS (or changing test details) conflicts. It then obtains and
validates root evidence, stages manifest/index with equality-on-retry, seals
files 0400 and the directory 0500, fsyncs, verifies staged hashes and modes, and
finally atomically publishes `commits/RUN_ID.json` in the package base.
The sibling location allows recovery after the package directory is sealed.
No package-local file independently claims terminal PASS or SEALED.
While holding the transaction lock, retry removes only regular, single-link,
owner-matching `.pending-` staging files from interrupted atomic writes. It
rejects staging symlinks and any unexpected package entry. Thus a crash inside
a write cannot leave an unindexed 0600 temporary file in a committed package.

| Crash cut | State / retry |
| --- | --- |
| After test | Immutable test, incomplete package; same test resumes |
| After manifest | PREPARED; validate existing records and stage index |
| After index | PREPARED; validate and seal |
| After permissions | Sealed but incomplete; read-only equality checks then publish commit |
| Immediately before commit | Same as above |
| Immediately after commit | Verify exact records, root receipt and modes; return identical commit |

Overall `SUCCESS` requires test PASS, evidence FINALIZED and COMMITTED.
Test FAIL with valid evidence is `TEST_FAILED`. Abnormal/missing/unverifiable
capture yields `EVIDENCE_FINALIZATION_FAILED`. Conflicting or externally changed
records fail closed; crash recovery does not authorize overwriting them.
Preserve all incomplete and failed packages for investigation. No automatic
purge is provided.

## Future coordinator and validation

`tools/qualification_campaign/prepare_bundle.py` copies a retained audited
phase snapshot into a new directory, overlays the maintained coordinator, Q8
and evidence modules, and freezes all source and qualification-plan digests in
FROZEN.json. The future coordinator verifies this before attempt start and each
phase. The explicit `--official-qualification` operation requires separate owner
authorization; this repair mission does not run it.

Q8 calls `q8_capture.capture_pair`: arm exact root producer, invoke the existing
host Pair operation, stop/reap/finalize in finally, validate receipt, then check
the existing Pairing Request/Response and absent peripheral Security Request
criteria. The former ordinary raw chmod/decode path is absent. The coordinator
records each phase and final cleanup outcome, then uses the package transaction.
The passive rehearsal uses this same capture boundary and coordinator finish
function with no pairing/HID/firmware/bond operations.

Canonical hardware-free tests include `test_evidence_pipeline.py` and semantic
mutation tests through `test-rp-test-infra.sh`. The explicit
`evidence_root_integration.py` test needs root solely to create synthetic raw
files and drop a child to UID/GID 65534; it installs nothing and uses no devices.
That child proves read/chmod rejection while Q8 metadata reaches all three
coordinator terminal outcomes. This is additional validation, never an implicit
sudo action in CI.
