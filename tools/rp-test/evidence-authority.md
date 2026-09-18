# Qualification evidence authority, version 5

This boundary changes qualification tooling, with product commit
`137d489d3d1219b203f84633cf8b570d9fe9f19a` unchanged. The physical fixture is
owner-confirmed FNK0099. Previous official FAILs, forensic indexes, interrupted
manifests, v1/v2/v3/v4 rehearsals, v2/v3/v4 installations and raw captures are
historical and must not be edited or normalized. A rehearsal is never
qualification. Version 5 uses a separate fixed namespace so installing it
cannot replace the engine used by any historical attempt or rehearsal.

## Authority chain

Frozen product → protected source runtime → immutable Attempt Authority Snapshot
→ persisted capture request and test journal → privileged receipt → ordinary
metadata stage → root-private final bytes → terminal commit.

Every arrow carries exact equality or a SHA-256 digest. No later step refreshes
its expectation from FROZEN.json or the current working tree. SHA-256 provides
identity here; root ownership and fixed invocation provide provenance, not a
claim that an untrusted self-described hash is authenticated.

`prepare_bundle.py` copies the retained reviewed phase source, overlays the
maintained boundary and appends the reviewed v5 amendment to the exact frozen
v3 base qualification plan. BUNDLE.json
lists **every file**, its digest, the product, coordinator, plan, FROZEN and five
privileged engine module identities. The runtime ID is the SHA-256 of the exact
canonical BUNDLE.json bytes. FROZEN.json is an ordinary measured member, never
a renewable source of truth. Bytecode and cache directories are rejected even
if listed; symlinks and hardlinks are rejected. Unknown files change membership.
No generated runtime is committed in the source repository.

An explicit administrator preparation runs `install_authority_runtime.py
--bundle BUNDLE`. It writes fixed destinations only:

- `/usr/local/lib/s3-hidbot-authority-v5`: protected engine modules and per-digest
  runtimes, files 0444, runtime directories 0555;
- `/var/lib/s3-hidbot-authority-v5`: root-owned state; `attempts` and `captures`
  0700, `staging` 0755 with per-run ordinary-user 0700 directories;
- protected `roots.json`: the state/capture directory device and inode identities.

It refuses an existing different engine or existing runtime ID; never updates
an installation during an attempt. New runtime installation remains an explicit
administrative operation. It is not an RPC and installs no dependency. All
installation ancestors must be root owned without group/other write permission.
The previous v2 installer remains historical tooling, not the future entrypoint.

## Source-only execution

The only future entrypoint is:

```text
/usr/bin/python3 -I -S -B /usr/local/lib/s3-hidbot-authority-v5/runtime_launcher.py RUNTIME_ID ENTRY CONTEXT [ARGS...]
```

The launcher verifies complete runtime membership, bytes, ownership and modes.
`-I -S` excludes current directory, user site, PYTHONPATH, sitecustomize and .pth
startup. `-B` prevents writes but is not relied upon to prevent reads. A source
loader reads/hashes/compiles exact protected source and never consults pyc.
Entry scripts are also compiled from measured source bytes. Local imports are
restricted to the protected bundle; the existing root-owned `/usr/lib/python3*`
standard library and installed Debian runtime dependencies are explicitly
trusted system dependencies. Python version and executable SHA-256 are in the
snapshot. System administrator changes remain outside the ordinary-user threat
model; no claim is made to measure every OS shared library or defeat root.

CONTEXT is `-` only for coordinator, reset-normalized preflight, rehearsal,
metadata resume and read-only runtime probe. Each phase instead receives the original handle as JSON argv;
the launcher loads and validates that original root journal before executing.
The snapshot contains product commit/tree/archive/BIN/ELF and host tree, exact
runtime manifest, coordinator/plan/helper/contract identities, schema, storage
identities, run/capture IDs, classification, duration, owner UID and interpreter.
Its canonical digest is `attempt_authority_sha256`. An exclusive root attempt
journal stores snapshot, handle and request before attempt start/capture. The
handle retained by the coordinator never refreshes. Q8 obtains the launcher
context and request through this same journal; environment variables are not
an authority source.

## Narrow root operations

The ordinary boundary invokes the absolute command, with controlled environment:

```text
/usr/bin/sudo -n /usr/bin/python3 -I -S -B /usr/local/lib/s3-hidbot-authority-v5/authority_service.py OP RUN_ID
```

Operations are begin, resume, load, activate, capture, verify, record, prepare
and seal. `activate` writes the immutable Q8 evidence requirement before a
capture can start and is rejected after terminal test/evidence recording.
No arbitrary path, writer executable, ownership target, source code, receipt,
installation or raw file operation is accepted. The root service derives the
caller UID from sudo and checks it against the immutable snapshot. Test-only
constructor seams are absent from the CLI. Requests/receipts use duplicate-free,
closed JSON schemas, ASCII IDs, strict types and exact handle equality. The
capture envelope explicitly carries `attempt_authority_sha256`; the preserved
v2 capture request carries the identical value as `authority_sha256`.

The unchanged v2 Producer retains fixed root, configured root object, ancestor
FDs, O_NOFOLLOW, O_EXCL raw creation, retained raw FD, inode and stat checks,
root-only 0600 ownership, flock and writer lifetime policy. btmon receives only
`/proc/self/fd/N`. Root decoding returns three counters, never raw HCI text.
Normal acceptance requires requested SIGINT, exit 0, reaped, no early exit and
no forced signal. EOF/deadline, exit 23, early exit, TERM/KILL are failures.
Parent-death SIGKILL prevents an orphaned writer; recovery never signals a
numeric PID. A persisted request without a stopped journal becomes immutable
CAPTURE_INTERRUPTED. Stopped journal/receipt recovery revalidates exact raw FD
identity, bytes and writer state. Capture IDs are never recaptured.

## Sealing and crash recovery

The root attempt flock serializes each operation around the **original** handle.
`record` persists the test result before evidence finalization. PASS/FAIL and
all details are immutable. `prepare` seals a FINALIZED empty evidence set when
no object was activated. When Q8 was activated, it directly reverifies the root
producer and persists its exact receipt; an evidence failure cannot upgrade. It constructs exact
expected authority, request, test, evidence, PREPARED manifest and index bytes.
The ordinary coordinator writes these metadata mirrors into the fixed stage.
Raw data never enters that tree or an ordinary recursive hash.

`seal` compares stage membership, regular single-link file identity, UID, 0400
mode, stable hashes and expected contents. It creates new root-owned copies in
the attempt's private package; it never chmods/chowns caller files. Stage is
revalidated after copying. Final root files are 0400 and the package is 0500.
After fsync, it opens and hashes every actual final file, including index and
manifest, checks object identity/type/owner/mode and expected semantic bytes,
and repeats validation immediately before publication. The root-only sibling
`commit.json` is written last and contains actual per-file hashes/object IDs,
the actual index hash, original handle and engine identity. The ordinary user
cannot replace this namespace. Stage changes after that transfer cannot change
terminal evidence. No PREPARED file alone claims terminal success.

| Crash | Recovery |
| --- | --- |
| Incomplete begin before snapshot/request | Fail closed; no attempt-start authorization |
| Snapshot and request durable | Resume that original journal, never regenerate authority |
| Test durable | Same outcome/details only; opposite outcome conflicts |
| Prepared or partially copied metadata | Same prepared authority and canonical stage only |
| Inside atomic write | Remove only root-owned regular single-link pending files; retry |
| Sealed package, no commit | Rehash exact final files and publish commit last |
| Commit exists | Reverify runtime, snapshot, raw receipt, final objects/modes/hashes and exact commit equality |
| Runtime or journal conflicts | Reject; never refresh/repair authority |

`evidence_resume.py RUN_ID` restores the original root journal through the
protected launcher and finalizes only an already recorded test result. It
never resumes or reruns hardware phases. If no test result exists, it fails
closed for manual investigation. All incomplete/failed records are retained.
No automatic purge exists.

SUCCESS requires test PASS plus FINALIZED required evidence. TEST_FAILED retains
test FAIL with either a valid finalized object or a finalized empty set.
EVIDENCE_FINALIZATION_FAILED is reserved for an activated object that is
abnormal or unverifiable. Q8 pairing criteria and all official phase criteria
remain unchanged. A future official attempt requires separate owner instruction;
this repair campaign uses only passive NOT_QUALIFICATION capture.

## Validation

`test-rp-test-infra.sh` includes real writer/FD races, v4 snapshot/stage/final-byte
regressions, exception and actual process-exit crash cuts, reset-normalized
preflight, the full early/late failure packaging matrix, Q8/coordinator
integration and assertion-killed semantic mutants. Root/nobody integration
in `evidence_root_integration.py` uses synthetic writers, no devices and no
installation; it separately proves raw/runtime/final-package permissions and
all three terminal codes. Installed-runtime probes test source loader identity
with hostile PYTHONPATH/current directory/user site and bytecode injections in
separate synthetic bundles. The accepted passive rehearsal is never mutated.
