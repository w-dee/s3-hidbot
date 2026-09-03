# UART Control Plane v1

## Scope and transport invariant

The control plane uses the onboard USB-UART path. Native USB-OTG remains the
separate TinyUSB HID Device path. A host must never assume that opening a serial
device proves that the HID USB path is attached, or vice versa.

The current v1 implementation extends the bounded JSON control core with the mandatory session lease,
HID runtime safety foundation, explicit native-USB exposure control, explicit
HID output routing, the safety-only `hid.release_all` command, and the public absolute
`hid.keyboard.report` and relative `hid.mouse.report` commands. It implements
`protocol.hello`, `system.ping`, `system.info`, `usb.status`,
`usb.exposure.status`, `usb.attach`, `usb.detach`, `hid.route.status`,
`hid.route.set`, `hid.route.v2.status`, `hid.route.v2.set`, `hid.release_all`,
`hid.keyboard.report`, `hid.mouse.report`, `ble.exposure.status`, `ble.enable`,
`ble.disable`, `ble.pairing.status`, `ble.pairing.respond`, `ble.bond.list`, and
`ble.bond.remove`. There is still no keyboard
helper, high-level keyboard/mouse automation, asynchronous event, GPIO action,
or reset command. Primitive report CLI commands are documented below and
remain explicitly unsafe.

U7.3 adds `ble.exposure.status`, `ble.enable`, and `ble.disable` under
`ble.exposure-control-v1`; protocol remains 1. With the later pairing and route
extensions, the full identity hello has 15 unique capabilities. BLE is
uninitialized/non-advertising at boot and lazy
initialization occurs only after accepted enable. Normal disable retains the
stack in hidden idle. BLE lifecycle generation is independent of USB route and
session authority, so simultaneous USB and BLE exposure is valid and neither
transport's exposure command mutates the other's desired state.

U7.5A fixes the internal BLE security prerequisite at authenticated passkey
input, bonding, 16-byte keys, Secure Connections with authenticated legacy
fallback, and a three-bond persistent NVS store. Capacity exhaustion is
fail-closed and never evicts an existing bond. Project bond readiness requires
an identity-qualified reread of both NimBLE `OUR_SEC` and `PEER_SEC` records;
the NimBLE bonded bit alone is insufficient. This foundation adds no UART
pairing command or capability and is not yet connected to HID output routing.

The internal Slice B controller serializes security work on the existing HID
control executor. Its only live states are `idle`, `securing`, and
`waiting_input`; completion and failure are retained separately as a bounded
sticky last result. A boot-local nonzero monotonic pairing ID fences a pending
input action by BLE generation and connection handle, with a 25-second input
deadline and fail-closed wrap behavior. NimBLE callbacks enqueue only bounded,
non-secret metadata into the fixed shared control queue. Queue overflow is a
recovery-required BLE lifecycle fault; ordinary store capacity, timeout,
repeat-pairing, or peer failure disconnects without global recovery. This
internal model is exposed in Slice C by the firmware-only
`ble.pairing-transaction-v1` capability and the `ble.pairing.status` and
`ble.pairing.respond` commands. At that Slice C boundary, the full identity
hello had 13 unique
capabilities. Slice D adds the strictly typed host API and explicit no-echo
CLI for those frozen commands, without a firmware change. No BLE route or HID
notification output was publicly reachable at that U7.3 boundary.

U7.5B adds `ble.bond-administration-v1` without changing protocol v1. Its
read-only `ble.bond.list` inventory and destructive `ble.bond.remove` mutation
both run through the existing serialized BLE control executor. The public bond
ID is the first 128 bits of SHA-256 over the domain
`s3-hidbot/bond-id/v1`, resolved identity address type, and six identity bytes,
rendered as exactly 32 lowercase hexadecimal characters. It is stable for a
stored bond but exposes neither the raw address nor security material. A
collision makes the inventory unhealthy and removal ambiguous; no list index,
name, prefix, wildcard, oldest, or first-record selection exists.

U7.4A adds an internal-only BLE HID output prerequisite without changing this
public protocol. Keyboard and mouse CCCD evidence and HID Control Point suspend
state are owned by the serialized HID control executor and fenced by the exact
BLE generation, connection handle, and registered characteristic handle. A
composite link is ready only when both notification subscriptions and all
U7.5A security/store/lifecycle conditions are current and the peer is not
suspended. The executor is also the sole writer of compound BLE security state.
A NimBLE store callback atomically inhibits readiness for the exact published
connection epoch before it queues failure evidence; the executor then commits
that evidence. `ble.disable` performs only lifecycle Stage A in the UART/control
context: the new hidden/disabling generation immediately fails the exact
lifecycle/peer readiness predicate, while compound security retirement waits
for the queued executor action. This leaves one compound-state writer even
when verification and disable ordering overlap. A retired epoch cannot inhibit
a reused handle. StoreFull remains connection-local and exact-identity fenced;
a fatal persistent-store failure is subsystem-global, inhibits every later
connection until reboot, and is committed as a recovery-required lifecycle
fault even if disable or disconnect retired its originating identity first.
The global callback latch also blocks every re-advertise path and is reconciled
idempotently before generation-scoped overflow processing at each executor
boundary. It therefore remains authoritative when the detailed fatal event is
lost to a full queue or a concurrent disable advances generation. Compound
security snapshots use a single-writer/read-many
sequence counter: executor serialization supplies the one writer and concurrent
status/HID readers retry incoherent observations. The fixed control queue is
depth 12. Queue-full publication uses one lock-free, generation-authority
sticky token rather than a separately published generation/connection tuple.
Generation zero uses a separate lock-free presence bit so zero can remain the
inactive value of the primary token without losing wrap fail-closed behavior.
Only an event that still targets the current lifecycle (and, for peer events,
the current connection) may set or replace a stale token; producers never
clear it. Readiness observes the token immediately. The executor clears only a
stale token by compare/exchange or the exact token after fail-closing that
authority, so concurrent and stale producers cannot erase current uncertainty
or carry it into a later authority. Overflow remains a fail-closed,
recovery-required BLE lifecycle fault. StoreFull itself remains local and does
not set the boot-global persistent-store latch, although losing its detailed
event to queue overflow invokes the independent generic recovery policy. The
Reset/Sync ownership transfer has a separate boot-lifetime, lock-free failure
latch because the NimBLE backend can legitimately publish the post-Reset
generation before the executor consumes the retired-generation Reset event. A
failed Reset, Sync, or one-shot lifecycle-timeout enqueue sets this latch before
the Sync watchdog is cancelled. Every successful action enqueue and every
actionable lifecycle or generic-overflow fallback publication gives the static
executor task a direct task notification. The notification is independent of
queue capacity, nonblocking in the actual task callback contexts, and retained
when given before `ulTaskNotifyTake(pdTRUE, portMAX_DELAY)`. Multiple wakes may
coalesce because the FIFO queue and sticky fallback atomics, rather than the
notification count, contain the authoritative work. On each wake the executor
reconciles before normal action handling, drains the bounded queue without
waiting, and performs a final fallback reconciliation before waiting again. A
publication racing after that final check leaves its notification pending, so
there is no check-then-sleep lost-wakeup window on either ESP32-S3 core.
A lock-free timer ownership protocol ensures that Sync and Disconnect cannot
replace one another's watchdog. Same-purpose re-arm is also rejected. Cancel
and timeout use transitional exact-owner states until physical timer stop or
durable timeout publication completes, so the timer handle cannot be reused in
either handoff gap. A Disconnect callback cancels only its own watchdog and
does so only after its event is queued. Disconnect establishes and arms its
typed watchdog before calling `ble_gap_terminate()`. A timer-arm or synchronous
initiation failure releases that exact purpose; `BLE_HS_EALREADY` retains the
watchdog because the existing termination still owns an eventual callback.
Immediate completion can therefore cancel an already-armed watchdog, and the
caller never rearms it. Nonfatal peer-security teardown propagates inability to
establish this bounded operation into a recovery-required lifecycle fault;
successful StoreFull teardown remains connection-local and may re-advertise.
The next executor boundary commits one
recovery-required queue fault and suppresses all later BLE callback actions
until reboot. Thus a lost Sync or terminal timeout cannot strand the lifecycle
in `enabling`, stale ordinary Sync events cannot complete a newer generation,
and generation wrap does not hide the handoff failure. A successful Connect
whose event cannot enter the queue is not adopted as executor peer state. The
Connect callback immediately requests termination of its exact NimBLE handle;
the configured one-connection limit and absence of deferred handle state
prevent that teardown from reaching a later peer. Logical queue-overflow
recovery remains authoritative even if the termination request is rejected or
the resulting orphan Disconnect event is itself stale. The
compiled notification adapter uses
`ble_gatts_notify_custom()` with exact 8-byte keyboard and 5-byte mouse values;
acceptance means only local NimBLE stack acceptance. It has no UART command,
BLE route, delayed retry, or peer-delivery claim in U7.4A.

U7.4B connects that adapter to the existing fixed HID report tickets through
an internal-only BLE route. Activation is an executor-owned operation accepted
only while the exact current BLE generation, connection, registered keyboard
and mouse characteristic handles, composite CCCD state, Control Point state,
security verification, persistent store, lifecycle, and fallback state are
ready. The public `hid.output-route-v1` contract remains exactly `none` or
`usb`; U7.4D adds a separate route-v2 surface rather than changing v1.

Each BLE ticket is permanently bound to its HID authority epoch, route
generation, BLE generation, exact connection, report-kind characteristic
handle, report kind, and ticket identity. The serialized control executor
validates this identity on admission, and the runtime and BLE adapter validate
it again immediately before the single notification call. Route `none`
submits nowhere, USB submits only through the TinyUSB SOF path, and the
internal BLE route submits only through the notification adapter. A successful
BLE result means `stack_accepted` by the local NimBLE/ATT/HCI path, not peer or
host delivery.

Normal BLE reports are never retained or retried. In particular, failed
relative mouse X/Y/wheel/pan values are discarded rather than accumulated,
merged, inverted, or replayed. A stale or not-ready item is terminal, and a
resource failure or stack rejection retires the narrow HID/route authority and
marks the affected state uncertain. Callback-side generation-fenced inhibits
prevent a readiness-losing event queued behind a report from allowing that
earlier report to submit. The executor then retires the active internal route;
later CCCD restoration, Exit Suspend, security recovery, or reconnect can
restore link readiness but never restores route authority automatically.
U7.4C adds BLE route retirement without changing that public contract. The
serialized owner publishes `desired=none, active=ble, transition=releasing`,
revokes normal authority, and retains only an exact old authority tuple for
one 8-byte Keyboard all-up attempt and one 5-byte Mouse all-up attempt. These
notifications contain no Report ID; local stack acceptance is only a bounded
delivery opportunity, never peer acknowledgment. An interface whose exact old
connection, value handle, CCCD, lifecycle, and verified security are no longer
usable is skipped without retry.

After the two best-effort attempts, a dedicated one-shot 100 ms timer publishes
an exact release-epoch/route-generation/BLE-generation/connection event. Its
sticky due bit and independent executor wake survive a full action queue. A
current expiry invokes the existing hardened BLE disconnect operation and its
typed watchdog; the grace timer never shares or overwrites that watchdog.
Spontaneous exact Disconnect, including an exact callback observation whose
queue publication overflows, cancels/fences grace and is the final physical
safety boundary. Only then is the retained release authority cleared and
stable none committed. A disconnect-initiation failure leaves the route
releasing and lifecycle recovery-required rather than claiming physical
retirement. Stale expiry, Disconnect, notification work, and reused numeric
connection handles cannot act on a newer tuple.

Direct USB-to-BLE and BLE-to-USB route changes remain rejected: switching is
only USB-to-none-to-BLE or BLE-to-none-to-USB. BLE exposure may advertise and
reconnect after retirement, but reconnect, CCCD Restore, resumed Control Point,
or restored security never reselects BLE.

U7.4D exposes the existing executor-owned route through
`hid.output-route-v2`, `hid.route.v2.status`, and `hid.route.v2.set`. Both v1
and v2 remain advertised. V2 adds `ble`, but activation still runs in the
serialized owner and succeeds only from stable none with the exact eligible
BLE tuple. It never enables advertising, pairs, queues a future selection, or
auto-restores. V1 status returns `HID_ROUTE_V2_REQUIRED` while BLE is active or
releasing; v1 `set none` may safely begin the same U7.4C retirement, while v1
`set usb` cannot replace BLE directly.

`ble.pairing.status` accepts omitted or empty params. Its exact result fields
are `state`, `generation`, `connected`, `pairing_id`, `action`,
`remaining_ms`, `encrypted`, `authenticated`, `bonded`,
`secure_connections`, `key_size`, and `last_result`. The pairing ID, action
`passkey_input`, and remaining time are non-null only in `waiting_input`.
Status is reconciled in the executor owner before serialization. Public
`bonded` means the current identity and policy have a project-verified
persisted `OUR_SEC` plus `PEER_SEC` pair, never only NimBLE's transient bit.

`ble.pairing.respond` requires exactly a nonzero uint32 `pairing_id` and a
six-byte ASCII-decimal `passkey` string. Success means only that the current
executor-owned transaction reached `ble_sm_inject_io()` and NimBLE accepted
the injection. A no-longer-current tuple maps to
`BLE_PAIRING_NOT_PENDING`; an attempted but rejected injection maps to
`BLE_PAIRING_FAILED`. Neither response nor diagnostic output contains the
passkey.

The sensitive retry identity is:

```text
HMAC-SHA256(K,
  ASCII("s3-hidbot/ble.pairing.respond/v1") ||
  uint32_be(normalized_payload_length) ||
  exact_framing_normalized_JSON_payload)
```

`K` is a 32-byte boot-lifetime RAM-only key generated before the UART receive
task starts. On ESP32-S3, firmware temporarily enables the supported
bootloader random entropy source, calls `esp_fill_random`, then disables that
source. Failure to initialize this key prevents the UART control plane from
starting, so pairing advertisement and secret-command acceptance fail closed.
The retry cache stores the payload length and HMAC, not the raw sensitive
request. The executor queue carries only a fixed mailbox token; the mailbox
and all project-owned parser, framing, and NimBLE passkey temporaries are
explicitly wiped after use.

`ble.bond.list` accepts only omitted or empty params and returns exactly
`capacity`, `count`, `available`, `healthy`, and `bonds`. Capacity is always
three and `available` is `3-count`. Entries are sorted by exact `bond_id` and
contain only `bond_id`, `our_sec`, `peer_sec`, `verified`, nullable
`schema_revision`, `schema_current`, and `connected`. A half bond is never
verified. Missing schema metadata is represented by `null` and is not by
itself security-store corruption; malformed records, enumeration failure, or
an already-fatal persistent storage state fail closed as `BLE_BOND_STORAGE`.
Cold-boot uninitialized BLE returns `BLE_NOT_READY`.

`ble.bond.remove` requires exactly
`{"bond_id":"<32-lowercase-hex>"}`. The executor permits mutation only after
BLE has been initialized and explicitly disabled to hidden idle, with no
advertising or connection, no active/releasing BLE route or route uncertainty,
no active pairing transaction, and no fatal storage state. A stable USB route
is independent and is not modified. Exact unknown, collision,
unsafe-live-state, and storage/postcondition failures are respectively
`BLE_BOND_NOT_FOUND`, `BLE_BOND_AMBIGUOUS`, `BLE_BOND_BUSY`, and
`BLE_BOND_STORAGE`.

Removal uses NimBLE's exact-peer deletion through the wrapped store callback,
which also deletes the same resolved identity's `hid_schema` revision. Success
is returned only after rereading `OUR_SEC`, `PEER_SEC`, and schema metadata as
absent, relisting one fewer bond, and proving all other public bond IDs remain.
A partial deletion is therefore an exposed fatal storage failure, never false
success. StoreFull remains connection-local and does not evict; a successful
manual removal frees one slot for a future pairing. Firmware-side removal does
not modify a host OS pairing database. A host that retains its own record may
require a separately authorized host-side lifecycle action before pairing can
succeed again.

Normal stop-and-wait retry caching applies to `ble.bond.remove`: the same
session, request ID, authority epoch, and exact serialized request bytes replay
the frozen successful response without invoking storage a second time. A
different payload with the same ID is a conflict.

Native USB is hidden by default: boot initializes HID/runtime state and its
lifecycle task, then starts the UART control plane without calling
`tinyusb_driver_install()`. CH343 UART is the bootstrap path. Only the
dedicated lifecycle task calls the public esp_tinyusb
`tinyusb_driver_install()` and `tinyusb_driver_uninstall()` APIs; no
`tud_connect()`, `tud_disconnect()`, deferred TinyUSB function, or private
TinyUSB lifecycle API is used.

The configured ESP-IDF console UART is used without application hardcoding of
its UART number, pins, or baud rate. The UART RX control task is the sole
project-owned consumer of its RX bytes. It does not use stdin, scanf, or a
line-oriented VFS read.

## Framing and synchronization

The JSON control wire format is:

```text
@HIDBOT <compact-json>\n
```

- `@HIDBOT ` must start at the beginning of a line.
- LF terminates a request. One CR immediately before LF is accepted and
  removed from the payload.
- An inbound request line is at most 512 bytes excluding LF, including the
  prefix and any trailing CR.
- Lines without the exact prefix are ordinary diagnostic text and are ignored.
- An overlong prefixed line is discarded through LF and receives a
  `LINE_TOO_LONG` response with `id:null`.

`TRANSPORT_SYNC` is exactly four consecutive raw NUL bytes:

```text
00 00 00 00
```

It clears only partial framing and overlong-discard state. It does not alter
USB, HID, MCU, UART configuration, session, request cache, or lease state.
One to three NUL bytes are ordinary non-sync input. Five or more NUL bytes are
one synchronization interval; extra NUL bytes are ignored until a non-NUL
byte starts fresh framing. Sync generates no JSON response, does not force a
new hello, and does not change control-session state.

## Strict JSON envelope

Requests must be bounded JSON objects with no duplicate object key. The
firmware uses the ESP-IDF cJSON parser followed by project-owned recursive
duplicate-key and depth/member/string-limit checks. Every envelope allows only
its listed fields; unknown fields fail closed.

```json
{"v":1,"id":12,"cmd":"protocol.hello","params":{"client_nonce":"0123456789abcdef0123456789abcdef"}}
```

```json
{"v":1,"id":12,"session":"0123456789abcdef0123456789abcdef","cmd":"system.ping","params":{}}
```

- `v` is an integer exactly equal to `1`.
- `id` is an integer in `0..2147483647`.
- `cmd` is a bounded non-empty string.
- `client_nonce`, `session`, generated `boot_id`, and generated sessions are
  exactly 32 lowercase hexadecimal characters (`[0-9a-f]{32}`), representing
  128 bits.
- `params` may be omitted for a normal request and then means `{}`. `null`,
  arrays, and scalar values are `INVALID_PARAMS`.
- `protocol.hello` requires exactly `{ "client_nonce": "..." }` params.
  The read-only diagnostic commands accept only omitted or empty-object params.

Success and error frames use these envelopes:

```json
{"type":"response","v":1,"id":12,"session":"0123456789abcdef0123456789abcdef","ok":true,"result":{}}
{"type":"response","v":1,"id":null,"session":null,"ok":false,"error":{"code":"MALFORMED_JSON","message":"..."}}
```

Every response has exactly `type`, `v`, `id`, `session`, `ok`, and exactly one
of `result` or `error`. If an `id` cannot be extracted as valid, the response
uses `"id":null`. The `session` field is never omitted: it is either a valid
current/new session token or JSON `null`. Error messages are bounded human
diagnostics; host behavior must rely on `error.code`.

## Machine-readable output and logs

Protocol responses and future events must use the common machine writer. The
logical machine-frame maximum is 1023 bytes including prefix, JSON, and LF.
With the current CRLF console configuration, the maximum UART wire form is
1024 bytes (`...\r\n`) when `CONFIG_LIBC_STDOUT_LINE_ENDING_CRLF=y`. The
writer holds the stdout FILE lock, flushes stdout,
then performs one `write(fileno(stdout), frame, length)` through the configured
console VFS before unlocking. This makes the complete frame share the UART VFS
per-UART write lock with normal stdout, stderr, and ESP_LOG output.

The configured console VFS may mirror the frame to the secondary USB
Serial/JTAG console. That is accepted console behavior, not a second control
endpoint; the supported control transport remains the configured USB-UART.

The serialization guarantee covers normal VFS-backed stdout/stderr/ESP_LOG
bytes: they cannot appear inside a machine frame. ROM boot output,
panic/fatal-crash output, future direct UART writers, and physical UART
corruption are outside the guarantee. The writer does not insert a leading LF
and therefore cannot repair a pre-existing LF-less partial console line. The
project's normal diagnostic output is LF-terminated ESP_LOG; future
project-owned diagnostic output should preserve that invariant.

The previous implementation called `uart_write_bytes()` directly after
`fflush()`. Although the UART driver `tx_mux` serialized that call, it bypassed
the UART VFS write lock held by normal console output and could split a
diagnostic line around a machine frame. The current writer replaces that
bypass.

The protocol response buffer is fixed at 1024 bytes. All formatters use
bounded `vsnprintf` serialization and fail closed to a bounded
`INTERNAL_ERROR` when a formatter cannot serialize; because the NUL terminator
occupies the last storage byte, generated logical responses are at most 1023
bytes. The hello format has a compile-time maximum calculation based on
bounded metadata, four 32-hex values (top-level/result session, boot ID, and
client nonce), fixed capabilities, maximum ID, prefix, and LF; host-native
tests assert every success and error response is within the logical bound.

Machine frames must not use ESP_LOG or printf, and project code must not add a
separate direct UART writer. ESP-IDF ROM boot output, panic output, and any
writer that bypasses stdout locking are outside this guarantee. Host parsers
must therefore accept only complete prefixed frames and ignore other lines.

Stack hardening keeps the request JSON parse buffer and reusable response
formatting scratch on the instance-owned `Protocol` object. The UART RX task
remains the sole protocol consumer, so this reuse is non-reentrant and does
not add a second synchronization mechanism. The stdout console-VFS single-write
path described above does not change the UART TX-buffer setting, sdkconfig, RX
framing, or protocol/session semantics.

The current implementation and validation do not include a synthetic concurrent
writer stress task. That remains a separate, bounded, default-disabled hardware
validation gate.

## Boot epoch and control session

At each MCU boot, the firmware generates a 16-byte `boot_id` using ESP-IDF
`esp_fill_random()`, rendered as 32 lowercase hexadecimal characters. In
ESP-IDF v5.5.4 this API has no error return. The value is an epoch marker, not
an authentication credential, and is returned from every successful hello.

A valid hello with a new `client_nonce` generates a new session, revokes the
previous session, clears normal-request retry state, activates the new session,
caches the hello response, and returns a response whose top-level `session` is
the new token. The result retains the same `session` and includes an exact
`client_nonce` echo, project, protocol version, boot ID, and the explicit
initial capability list:

```text
  protocol.hello-v1, system.ping-v1, system.info-v1, usb.status-v1,
  usb.exposure-control-v1, hid.lease-v1, hid.release-all-v1, hid.keyboard-report-v1,
  hid.mouse-report-v1, firmware.identity-v1, hid.output-route-v1,
  hid.output-route-v2,
  ble.exposure-control-v1, ble.pairing-transaction-v1,
  ble.bond-administration-v1
```

For a successful hello, top-level `session` equals `result.session`. Both are
the generated control-session token. The echoed nonce identifies this hello
attempt; `boot_id` identifies the MCU boot epoch.

The complete successful-hello shape is:

```json
{"type":"response","v":1,"id":1,"session":"<new-session>","ok":true,"result":{"project":"s3-hidbot","protocol_version":1,"client_nonce":"<request-client-nonce>","boot_id":"<boot-id>","session":"<new-session>","lease_ms":5000,"capabilities":["protocol.hello-v1","system.ping-v1","system.info-v1","usb.status-v1","usb.exposure-control-v1","hid.lease-v1","hid.release-all-v1","hid.keyboard-report-v1","hid.mouse-report-v1","firmware.identity-v1","hid.output-route-v1","hid.output-route-v2","ble.exposure-control-v1","ble.pairing-transaction-v1","ble.bond-administration-v1"]}}
```

The angle-bracket values above are documentation placeholders only; wire
values are fixed-length lowercase hexadecimal tokens.

Adding a capability does not itself require changing protocol version `v`;
hosts must use the advertised capability list. The safety release, absolute
keyboard-report, and relative mouse-report capabilities are advertised without
changing `v`. All normal requests require the exact current session; USB mount
does not automatically create one.

The `firmware.identity-v1` capability is additive and does not change protocol
version `v` or the hello result fields. When advertised, `system.info` includes
the exact nested `firmware` object described below. `app_elf_sha256` is the
SHA-256 of the running linked ELF, not a flash BIN or package artifact digest.

The response `session` field is a correlation/epoch identity, not an
authentication credential. A `session:null` response cannot be trusted as
belonging to an authenticated/current control session. Only a response
produced after exact current-session validation may contain the current token.
Before that boundary, `session:null` is used. This includes framing/JSON/
envelope/version errors, malformed or missing session, `SESSION_MISMATCH`,
hello validation failures, `CLIENT_NONCE_CONFLICT`, and params-type errors
found before session comparison.
After the boundary, normal successes, exact retries, ID conflict/stale errors,
unknown-command errors, and command-level parameter/execution errors contain
the current token. A successful hello and exact hello retry contain the new
session token.

Hello and normal-request retry state are separate one-entry fixed-capacity
caches. Both are scoped to the current HID authority epoch as well as their
existing request identity. The hello cache holds the client nonce, exact
normalized JSON bytes, serialized response, resulting session, and authority
epoch:

```text
same nonce + same bytes       replay the cached response only
same nonce + different bytes  CLIENT_NONCE_CONFLICT
new nonce                     establish a new session
```

Exact hello retry neither regenerates a session nor repeats takeover work, but
only in the same authority epoch. After a suspend, resume, unmount, or mount,
the same nonce and exact hello bytes are a fresh handshake with a new session,
fresh lease, and fresh response. This lets a host recover without changing its
nonce while preventing an old cached hello from resurrecting authority.

The normal-request cache holds an ID, exact normalized JSON bytes, complete
serialized result, and authority epoch. With the active session in the same
authority epoch, requests follow these rules:

```text
id > last_id                         new request
id == last_id, exact same bytes      cached response replay
id == last_id, different bytes       REQUEST_ID_CONFLICT
id < last_id                          REQUEST_ID_STALE
```

ID gaps are permitted. IDs never wrap; after `2147483647`, the host establishes
a new session. A cache hit replays only the result and never repeats a HID
action. The session and authority-epoch check occurs before normal-cache
replay. An old-epoch exact retry therefore returns `SESSION_MISMATCH`, does not
refresh the lease, and never replays a cached success. Pre-command parse,
schema, or session errors, stale IDs, and ID conflicts do not replace the
completed-request cache.

## HID lifecycle authority boundaries

Native USB HID lifecycle publication is a control-session safety boundary.
Physical cable removal is not guaranteed to yield an immediate TinyUSB unmount
on every board because this firmware does not yet include a board-specific
VBUS monitor. The guarantee begins when firmware publishes a suspend, resume,
unmount, or mount lifecycle event, not when a host physically notices cable
removal.

`hid_runtime` owns a lock-free fixed-width atomic authority epoch. It advances
on every suspend, resume, unmount, and mount. A successful hello captures that
epoch in its control session. A normal request compares the captured epoch to
the current acquire-loaded epoch before semantic processing, cache replay, or
lease refresh. That request-side comparison is the linearization point for
read-only commands: a response may finish after a later lifecycle publication
only if it had already passed the comparison. Unsafe HID work has an additional
executor-side epoch barrier.

The mailbox's attach generation remains a distinct mount/unmount token. It
prevents old-attach work from reaching a new attach. Mailbox entries carry both
the attach generation and authority epoch; report completion/failure records
carry them too, so a late callback cannot clear safety state for a later epoch.
Unmount clears old attach logical state, uncertainty, queued work, and pending
all-up safety work. Mount starts a clean all-up attach.

Suspend is different: configuration may resume against the same host. Suspend
cancels unsafe queued work, treats held, in-flight, and uncertain interface
state as needing an all-up safety release, and publishes
`mounted=true, suspended=true, keyboard_ready=false, mouse_ready=false`.
It sends no report while suspended. Resume advances authority again and retains
that safety requirement. The SOF executor sends all-up safety work before any
unsafe work and blocks unsafe submission until required safety work has
completed. The UART RX cleanup notification is deliberately eventual and
coalesced; atomic epoch comparison is the correctness barrier.

A hello takeover first revokes old authority, starts any required safety
release, and then activates the new session. The runtime provides that internal
safety boundary.

## Read-only diagnostic commands

- `system.ping` returns `{ "pong": true }`.
- `system.info` returns bounded static project, target, ESP-IDF version, and
  protocol version. The current identity-v1 firmware additionally returns a
  nested `firmware` object containing the validated descriptor version,
  optional configured source revision (`null` when unset), linked ELF SHA-256,
  and build profile. It must not include a user name, host path, serial
  identifier, or build-host information.

The identity-v1 `system.info` result has exactly these fields:

```json
{
  "project": "s3-hidbot",
  "target": "esp32s3",
  "idf_version": "v5.5.4",
  "protocol_version": 1,
  "firmware": {
    "version": "0.1.0",
    "source_revision": null,
    "app_elf_sha256": "<64 lowercase hex>",
    "build_profile": "freenove-fnk0085"
  }
}
```

`source_revision` is either JSON `null` for an unset build input or the full
40-character lowercase hexadecimal `S3_HIDBOT_SOURCE_REVISION` value. It is
never an inferred Git value. Hosts must validate the descriptor fields and
must not treat `boot_id` or the control session as firmware identity.

- `usb.status` returns current TinyUSB `mounted`, `suspended`,
  `keyboard_ready`, and `mouse_ready` booleans. It queries status only and
  never submits a HID report. Its four-field schema is retained for legacy
  compatibility and does not describe desired exposure, install state, or
  recovery.

## Explicit BLE exposure control

The three BLE commands accept only omitted or empty-object params. Their exact
result has no additional fields:

```json
{"desired":"hidden|exposed","observed":"uninitialized|enabling|idle|advertising|connected|disabling|fault","generation":0,"stack_ready":false,"advertising":false,"connected":false,"recovery_required":false,"last_error":null}
```

`last_error`, when present, contains exactly `operation` (`enable`, `disable`,
or `runtime`) and signed integer `code`. Addresses, handles, MTU, CCCD, and
pairing/security data are never public. Accepted transitions return a frozen
Stage-A snapshot and use the existing normal retry cache. Ordinary BLE
enable/disable does not invalidate a USB HID session. `hid.route.set` still
accepts only `none|usb`; BLE connection/subscription does not make HID ready.

The project-owned 0x1812 database is a discoverable BLE HID Service foundation:
HID Information bytes `11 01 00 00` mean HID 1.11, country 0, and no remote
wake/normally-connectable flag; the Report Map defines keyboard Report ID 1
(8-byte logical input) and mouse Report ID 2 (5-byte logical input). Both input
reports read neutral zeros and expose CCCD/Report Reference, but U7.3 has no
notification call path. HID Control Point `0`/`1` changes only BLE-local
suspend state. Protocol Mode, boot characteristics, BAS, fabricated battery,
DIS/PnP ID, pairing/passkey control, bonding, and security-required attribute
flags are absent. This is a HOGP-oriented scaffold, not a formal HOGP/security
compliance claim.

The NimBLE server also exposes the standard 0x1800 Generic Access service and
0x1801 Generic Attribute service. GAP publishes the same `s3-hidbot` name and
Generic HID appearance `0x03c0` as the advertisement; GATT provides the
stack-standard Service Changed infrastructure. Before every project HID
advertising attempt, firmware uses the live local GATT database to prove that
the standard GATT, project-owned HID Schema Epoch, and 0x1812 services occupy
the revision-1 handle topology and that required characteristic handles were
registered. A missing, incomplete, or reordered database fails the BLE
lifecycle closed with no visible HID advertisement.

The fixed registration order is GAP, GATT, HID Schema Epoch, then HID. The
internal epoch Primary Service UUID is
`5f7d0a10-7e38-4ed1-b97b-1fa4e83c2a10`; its sole read-only characteristic is
`5f7d0a11-7e38-4ed1-b97b-1fa4e83c2a10` and returns exactly one unsigned byte,
currently `1`. The service has three attributes and no write, notification,
indication, or CCCD. It occupies `0x000e..0x0010`, moving the HID Primary
Service start from the legacy `0x000e` to `0x0011`. Reading this informational
value has no side effect and is not evidence that the peer consumed the HID
Report Map.

Bonded-client cache compatibility uses an explicit one-byte GATT schema
revision. Revision 1 identifies the current cache-relevant HID Report Map and
database semantics. The revision is stored in the `hid_schema` NVS namespace
under a key derived from the resolved peer identity address; it is deliberately
separate from NimBLE's fixed `OUR_SEC`/`PEER_SEC` record formats. Exact bond
security-record deletion also deletes its companion revision, without eviction
or cache-reset commands. A missing key is a valid legacy bond with
unknown/stale schema, not storage corruption.

After the existing authenticated, identity-resolved, persisted-bond checks, a
stale peer with a restored/enabled Service Changed CCCD receives one
`ble_svc_gatt_changed(0x0001, 0xffff)` request for that connection. The
conservative full-database range avoids depending on fragile generated handle
numbers. Send acceptance or indication confirmation is not cache-current
evidence. Only a successful read of this firmware's Report Map by the exact
generation/connection, reconciled with the qualified bond identity, permits a
write-and-reread of revision 1. A Report Map read that precedes identity
resolution is retained only as connection-local executor state until security
becomes qualified. Stale schema therefore inhibits composite BLE HID readiness
even when security and both HID CCCDs are otherwise ready. Disconnect and
queue-overflow handling clear or fail-close all uncommitted evidence; neither
cache repair nor reconnect selects the BLE route automatically. Increment the
schema revision and update its host-side descriptor fingerprint whenever the
HID Report Map, cache-relevant GATT layout, or other bonded-client cached
semantics change. Every future cache-relevant revision must also append one or
more fixed revision-marker attributes before HID (without removing or
reordering older epoch attributes) so that the HID Primary Service start
changes monotonically. The firmware still exposes one compile-time database
to every peer; per-peer
revision records never select a dynamic topology.

ESP-NimBLE v5.5.4 persists a bonded CCCD by peer identity and characteristic
value handle. During restore it applies flags only when that handle is one of
the current database's configurable characteristics, but it still emits a
RESTORE event and does not purge an unmatched old record. The project accepts
subscription evidence only when generation, connection, interface, and the
current registered value handle all match. Revision 1 therefore ignores the
legacy keyboard `0x0016` and mouse `0x001a` records; neither aliases the new
keyboard `0x0019` or mouse `0x001d`. The persistent CCCD capacity is 15 so all
three supported bonds can retain Service Changed plus two legacy and two
current HID records during this migration without bond deletion or eviction.

Advertising is legacy connectable undirected with an exact 22-byte payload:
Flags `0x06`, complete UUID list `0x1812`, Generic HID appearance `0x03c0`, and
complete name `s3-hidbot`; no scan response is used. Interval is 40 ms. The
single connection is offered 15–30 ms interval, latency 0, and 4 s supervision
timeout. NimBLE chooses a public identity if available or a supported static
random identity; it is never returned over UART.

The topology epoch is designed to make a retained-host service rediscovery
remove the old HID tuple and probe the new one. Bond-preserving cache migration
is not claimed for every host stack until each target stack is physically
qualified.

## Explicit USB exposure control

The additive `usb.exposure-control-v1` capability exposes three commands:
`usb.attach`, `usb.detach`, and `usb.exposure.status`. Each accepts only
omitted parameters or `{}`; a non-empty parameter object is `INVALID_PARAMS`.
The protocol version remains `1`.

`usb.exposure.status` returns exactly:

```json
{
  "desired": "hidden|exposed",
  "observed": "driver_not_installed|disconnected|attaching|mounted|suspended|detaching",
  "generation": 0,
  "mounted": false,
  "suspended": false,
  "keyboard_ready": false,
  "mouse_ready": false,
  "safety_pending": false,
  "host_release_uncertain": false,
  "recovery_required": false,
  "last_error": null
}
```

`last_error` is `null` or exactly `{ "operation": "install|uninstall",
"code": <signed integer> }`. At cold boot the state is `hidden` plus
`driver_not_installed`, generation zero, all runtime booleans false, no
safety/uncertainty/recovery, and `last_error:null`.

An accepted `usb.attach` revokes unsafe authority, advances the USB generation
once, publishes `exposed`/`attaching`, and queues one install for the dedicated
lifecycle task. A successful driver install becomes `disconnected` unless a
mount callback already observed `mounted`; mount does not advance generation.
Duplicate attach while exposed is a no-op. A proven clean pre-start install
failure returns `driver_not_installed` and permits a new attach retry; an
ambiguous failure remains `attaching` with `recovery_required:true`.

An accepted `usb.detach` immediately publishes `hidden`/`detaching`, revokes
unsafe authority, and preserves the old USB generation solely for
lifecycle-owned fixed all-up work. After bounded success, failure, or timeout,
the task records old-generation uncertainty when required, advances generation
exactly once, and calls public driver uninstall. A successful uninstall is
`driver_not_installed`; an uninstall failure stays `detaching` with
`recovery_required:true`, no automatic retry, and the exact signed error in
`last_error`. An explicit teardown unmount callback is observational only and
does not advance generation or clear uncertainty.

Accepted attach/detach responses are cached as exact bytes even though their
authority transition retires the normal control session. Exact same-ID/same-
bytes retries replay that response without a second lifecycle action. Other
subsequent control requires a fresh hello. `usb.attach` during detaching and
`usb.detach` during attaching return the established `HID_BUSY` taxonomy; no
in-flight transition is reversed.

Unexpected unmount while still exposed revokes unsafe authority, advances the
generation once, and publishes `disconnected`; a later mount uses that fresh
generation. If host release is uncertain, a reattach creates fresh-generation
all-up reconciliation before unsafe HID is admitted. It never replays old
relative motion or old all-up work.

Lifecycle transitions do not produce asynchronous machine events.

## Explicit HID output routing

The additive `hid.output-route-v1` capability exposes `hid.route.status` and
`hid.route.set` without changing protocol version 1. USB exposure and HID
output selection are independent: `usb.attach`, mount, resume, reattach, and
reconnect never select USB. Unsafe keyboard and mouse reports return
`HID_NOT_READY` while the route is none.

`hid.route.status` accepts only omitted params or `{}`. `hid.route.set` accepts
exactly `{"route":"none"}` or `{"route":"usb"}`. Missing/additional fields,
non-string values, unknown strings, and `"ble"` are `INVALID_PARAMS`; v1
syntax remains unchanged even when BLE exposure and route v2 are available.

Both successful commands return exactly:

```json
{
  "desired": "none|usb",
  "active": "none|usb",
  "generation": 0,
  "transition": "stable|releasing",
  "ready": false
}
```

Cold boot is stable none, generation zero, not ready, and is queryable over
UART while native USB remains hidden. Route generation is an opaque uint32
authority epoch: consumers may compare only equality/inequality; it is not an
ordered value or transition count, wrap is permitted, and a preempted
authority retirement may consume an epoch.

Selecting USB requires stable none, exposed/mounted USB, no suspend, both HID
endpoints ready, no safety pending, no host-release uncertainty, and no
recovery requirement. A clean selection is synchronous, advances route
generation, revokes unsafe/session authority, and returns a frozen stable USB
snapshot. Hidden, attaching, disconnected, suspended, detaching, or recovery
states return `HID_NOT_READY`; safety or host uncertainty returns
`HID_SAFETY_PENDING`; a shared lifecycle/route transition returns `HID_BUSY`.

USB-to-none first publishes the frozen Stage-A response `desired=none`,
`active=usb`, old generation, `transition=releasing`, `ready=false`. It revokes
unsafe authority and cancels stale work, then uses the single control executor
to send any required all-up over the old USB identity. Stable none is published
only after clean or fail-closed terminalization; USB remains installed and
mounted. Failure retains uncertainty in USB transport state and never selects
another route. Suspend/unmount immediately close unsafe authority and
terminalize none without waiting for the control task; resume/reconnect do not
restore the route.

Accepted route mutations share the exact serialized control-transition retry
cache with USB attach/detach. Same original session, ID, and request bytes may
replay the immutable accepted response after session revocation; asynchronous
completion cannot change it, and a fresh hello retires the proof. Stable
none-to-none and ready USB-to-USB are no-ops with no generation, authority,
session, release, safety, or executor change.

The separate `hid.output-route-v2` capability uses `hid.route.v2.status` and
`hid.route.v2.set` with the same five fields and retry/session rules. Its route
set is exactly `none|usb|ble`. BLE selection requires an already connected,
authenticated, persistently verified, composite-subscribed, unsuspended peer
and stable none; failure returns `HID_NOT_READY` without changing the route.
USB-to-BLE and BLE-to-USB return `HID_BUSY` until an explicit `set none` has
completed. During BLE retirement, v2 reports `desired=none`, `active=ble`,
`transition=releasing`, and `ready=false`; stable none is not reported before
exact physical loss. Local `stack_accepted` results never claim peer delivery.
While BLE is active or releasing, v1 status returns
`HID_ROUTE_V2_REQUIRED` instead of fabricating none/USB. V1 `set none` may
start the same BLE retirement and returns that version error for its
unrepresentable frozen snapshot; its exact retry remains cached and the
meaningful mutation invalidates the session. V1 `set usb` returns `HID_BUSY`
until stable none, so an old client cannot directly replace BLE.

## Absolute keyboard reports

`hid.keyboard.report` is the first public unsafe HID operation. Its only
accepted parameter object has exactly these fields:

```json
{"v":1,"id":7,"session":"<token>","cmd":"hid.keyboard.report","params":{"modifiers":2,"keys":[4,5]}}
```

`modifiers` is an integer bitmap in `0..255` (Left Control, Left Shift,
Left Alt, Left GUI, Right Control, Right Shift, Right Alt, and Right GUI from
least to most significant bit). `keys` is an ascending array of zero through
six distinct integer usages. The implementation permits exactly `0x04..0xA4`
and `0xB0..0xDD`; modifier usages and reserved/error ranges are rejected. The
firmware never sorts caller input. The report is the unchanged Boot keyboard
8-byte layout: modifier byte, reserved zero byte, then the six usages padded
with zero; report ID and keyboard instance are both zero.

Every `hid.keyboard.report`, including an all-up payload, is unsafe. It is
accepted only for the current session/authority and attach generation while
the device is mounted, unsuspended, keyboard-ready, and globally safety-clear.
Mouse safety, uncertainty, release work, or an incompatible keyboard
operation therefore returns `HID_SAFETY_PENDING`; an occupied ticket or
in-flight report returns `HID_BUSY`; an inactive endpoint or a false TinyUSB
submission returns `HID_NOT_READY`.

The success result is exactly one of:

```json
{"state":"submitted"}
{"state":"already_set"}
```

`submitted` means only that `tud_hid_n_report(instance=0, report_id=0,
report, length=8)` accepted the bytes. It does not mean USB host polling,
report completion, or an evdev event. `already_set` requires the same
confirmed state, with no queued, claimed, in-flight, uncertain, or safety
work; a provisional state before report completion is never sufficient.

The control task publishes a fixed-size heap-free `KeyboardReportTicket`.
The TinyUSB SOF executor claims it with an acquire/CAS transition and performs
one immediate task-affine submission. A ticket that remains `PUBLISHED` for
100 ms may be canceled with `PUBLISHED -> CANCELED` and returns
`HID_NOT_READY`; if the executor wins the claim, it resolves the immediate
outcome instead of returning a timeout that could permit a ghost keypress.
Lifecycle or authority changes cancel published work and return
`SESSION_MISMATCH`. Canceled work is never replayed on a later SOF.

Submitted state is provisional until the matching generation/authority,
interface, payload, and in-flight identity receives `report_complete`. A
matching failure does not confirm the keys: it marks host state uncertain,
requires the global all-up safety path, revokes control authority, and never
retries the failed unsafe payload. `hid.release_all` remains the only public
safety recovery command. Successful and operational-error responses are
cached for exact same-ID/bytes retries; retries never submit a second report,
and an old authority epoch is rejected before cache replay. Lease refresh
applies to successful and operational outcomes, but not invalid parameters,
ID conflicts/stale IDs, or session mismatch.

Hosts that use `Client.keyboard_report(modifiers, keys)` perform the same
fail-fast validation and preserve caller order. No keyboard type/chord helper,
raw HID API, or keyboard CLI is provided; the separate explicit mouse primitive
is `Client.mouse_report(buttons, x, y, wheel, pan)`.

## Host correlation contract

For a normal request, the host accepts a response only when its type,
protocol version, expected ID, expected current session, and response schema
all match. A wrong-session or malformed frame cannot satisfy the request; the
host should discard it within bounded limits and continue waiting.

For hello, the host additionally requires the expected ID, `ok:true`, exact
`result.client_nonce`, valid top-level and result session tokens that are equal,
valid `boot_id`, expected project/protocol version, exact `lease_ms:5000`, and
a valid capabilities list. The host compatibility baseline is the six safe
control-plane capabilities: `protocol.hello-v1`, `system.ping-v1`,
`system.info-v1`, `usb.status-v1`, `hid.lease-v1`, and
`hid.release-all-v1`. Keyboard, mouse, and explicit output-route capabilities
are optional; the corresponding Client methods fail locally before allocating
an ID or writing a frame when the peer does not advertise them. Unknown
additional capabilities are preserved and do not make an otherwise compatible
peer fail.
A wrong-nonce/stale frame must never establish a session. The client applies
the bounded discard limit and timeout recovery (`TRANSPORT_SYNC` followed by a
fresh hello) in the generic host core.

`usb.exposure-control-v1` is an optional additive capability. When it is
advertised, `Client.usb_exposure_status()` validates the strict lifecycle
schema, while `Client.usb_attach()` and `Client.usb_detach()` perform only the
explicit requested transition. Each successful transition invalidates the
client's local session conservatively; the caller must establish a fresh hello
before issuing a subsequent control request. No host method automatically
attaches native USB.

`Client.info()` continues to return the raw result object for wire-compatible
callers. External consumers that need typed compatibility inspection may use
the pure host validators `validate_system_info()` and
`evaluate_compatibility()`. Without `firmware.identity-v1`, `system.info`
must have its legacy four-field shape. With the capability, the current
firmware produces the strict nested identity-v1 shape above. New hosts accept
both forms, so old firmware remains compatible with identity unavailable.

The v1 wire hardening keeps protocol `v` at `1` and uses the advertised
capability list. The nonce identifies a hello attempt, the boot ID identifies
an MCU boot epoch, and the request ID identifies a request within the current
control session.

## Host client and pure transport core

The host package contains a generic byte-transport core and a board-specific
serial adapter with a thin CLI in `host/src/hidbot`; protocol logic depends
only on the generic transport interface. Host tests use fake transports and
never open a real tty.

The receive framer is byte-oriented and bounded to the 1024-byte machine-frame
limit. It accepts only an exact `@HIDBOT ` prefix at the beginning of a line,
supports arbitrary chunks, multiple lines per chunk, LF or CRLF termination,
and recovers from an overlong prefixed line at the next LF. Prefix-less lines
are diagnostic logs and are discarded by default. A caller may provide a
bounded log sink; no unbounded line or trace buffer is permitted.

The host response model rejects duplicate JSON keys, non-finite numbers,
missing envelope fields, invalid IDs/tokens, inconsistent `ok` versus
`result`/`error`, unknown envelope fields, and structures beyond the host's
bounded depth/member/string limits. Normal completion requires the expected
ID and current session. Hello additionally requires the expected nonce,
matching top-level/result sessions, a valid boot ID, `s3-hidbot` identity, the
six-capability baseline (`protocol.hello-v1`, `system.ping-v1`,
`system.info-v1`, `usb.status-v1`, `hid.lease-v1`, and
`hid.release-all-v1`), exact lease metadata, and a unique bounded capability
list. Keyboard, mouse, and explicit output-route capabilities are optional;
the corresponding Client methods fail locally before allocating an ID or
writing a frame when the peer does not advertise them. Unknown additional
capabilities are preserved and do not make an otherwise compatible peer fail. A stale hello
with the wrong nonce never establishes a session. `session:null` errors are
retained only as bounded untrusted diagnostics and cannot complete a request.

The client is stop-and-wait with one outstanding logical request and a simple
mutex for same-instance callers. The default deadline is 1.0 seconds per
attempt, with three total attempts and a 50 ms transport polling interval.
Only the same session, ID, and exact serialized frame bytes may be retried.
Fresh hello creates a new 32-lowercase-hex nonce. Normal IDs start at zero for
each fresh session and never wrap; hello IDs use a separate process-local
monotonic space. When normal IDs are exhausted, a fresh hello is established
before another normal request. A session loss, reset, or fresh hello never
automatically replays an old logical command; its result is left to the caller
as unknown/session-lost.

After `connect()`/hello, the host client exposes `ping()`, `info()`,
`usb_status()`, optional `usb_exposure_status()`, `usb_attach()`, and
`usb_detach()`, optional `hid_route_status()` and `hid_route_set()`, the
optional one-shot `ble_pairing_status()` and `ble_pairing_respond()` methods,
typed `ble_bond_list()` and exact-ID `ble_bond_remove()` methods,
safety-only `Client.release_all()` API, and the explicit
`Client.keyboard_report()` and `Client.mouse_report()` primitive APIs. The CLI
also exposes `usb-exposure-status`, `usb-attach`, `usb-detach`,
`ble-bond-list`, `ble-bond-remove BOND_ID`, `hid-route-status`,
`hid-route-set none|usb|ble`, and explicit `keyboard-report`
and `mouse-report` commands, each with command-local
`--unsafe-hid`; no arbitrary raw command API exists. The hello result exposes
read-only `lease_ms` metadata. Closing the client only closes the injected
transport and invalidates local session state; it sends no UART command.

The current Freenove FNK0085 materials identify CH343 as the USB-UART bridge.
Hardware characterization observed safe idle as DTR=true and RTS=true
on the tested board/host. DTR=false-to-true and a transition through both false
could reset the application; an RTS-only transition had no observed effect.
The exact circuit mapping was not inferred, and software cannot prove that an
electrical glitch is impossible. Production transport code therefore sets both
lines true before open, never changes them during normal use, restores both to
true immediately before close, and does not expose line override options.

## Serial transport and CLI

`PySerialTransport` is a bounded implementation of the generic byte transport.
It uses pyserial `>=3.5,<4` with explicit 8N1, no software or hardware flow
control, a small read timeout (50 ms by default), a bounded write timeout, and
Linux exclusive ownership. It is constructed with `port=None`, sets DTR=true
and RTS=true, assigns the resolved port, and only then opens the tty. Opening
performs no sync, hello, reset, input drain, or line pulse. Closing restores
both lines to true before closing; repeated close and failed-open cleanup are
safe. Partial writes complete through a bounded deadline and serial I/O
failures become `TransportError`; request deadline expiry remains
`RequestTimeoutError` in the generic client.

The CLI entry point is `hidbotctl`. It exposes `hello`, `ping`, `info`,
`usb-status`, `usb-exposure-status`, explicit `usb-attach` and `usb-detach`,
explicit `ble-pairing-status`, `ble-pairing-respond --pairing-id ID`,
`ble-bond-list`, and `ble-bond-remove BOND_ID`,
explicit `hid-route-status` and `hid-route-set none|usb|ble`,
and the safe `self-test` control-plane diagnostic through the diagnostic
client methods, the safety recovery command `release-all` through
`Client.release_all()`, artifact-only validation via `verify-artifact ARTIFACT`,
verified artifact-to-runtime identity comparison via `verify-firmware ARTIFACT`, and the explicit unsafe primitive
commands `keyboard-report` and `mouse-report` through the existing Client
methods. Device commands use `Client.connect()` first; `verify-artifact` is
artifact-only and returns before resolving serial configuration. `--port` overrides
`S3_HIDBOT_SERIAL`; there is no tracked default port. `--baud` overrides
`S3_HIDBOT_BAUD`, then the current tested host default of 115200 is used.
`--timeout`, `--attempts`, `--json`, and `--verbose` are available before or
after the command; if an option is repeated, the later value wins. DTR/RTS
overrides and raw JSON/UART commands are intentionally not available.
`--json` emits one compact result object on stdout; diagnostics and errors use
stderr. Exit codes are 0 success, 2 configuration/input, 3 transport, 4
protocol/compatibility/session, 5 remote command, 6 timeout, and 7 for a
completed `verify-firmware` comparison that is a mismatch or has unavailable
identity.

Pairing response input is read only from the controlling terminal with echo
disabled; there is no passkey argv, environment, configuration, or ordinary
stdin fallback. The CLI drops its local immutable string reference after one
validated call. The client serializes once and keeps the same immutable frame
only through terminal success, remote error, or retry exhaustion, then drops
its reference. It does not intentionally log the request or secret. Python
caller strings and immutable byte copies cannot be reliably zeroized, so API
callers own their original string lifetime and no erasure claim covers CPython
allocator remnants, pyserial copies, or kernel buffers.

`hidbotctl verify-artifact ARTIFACT` validates either an archive or extracted
bundle directory with the canonical artifact verifier, then renders the
verified runtime-comparable artifact identity. It is non-destructive and
serial-independent: it does not resolve a port, construct a transport, open a
device, send a protocol command, or access HID. Valid input exits 0; missing,
malformed, or unverifiable input exits 2. `--json` emits one compact object
with `ok:true`, `classification:"VALID"`, and `artifact`; `VALID` describes
internal artifact validation only, not a signature, authentication, secure
boot, or attestation.

`hidbotctl verify-firmware ARTIFACT` verifies an archive file or extracted
bundle directory before it constructs or opens a serial transport. It then
runs exactly `protocol.hello` and `system.info`, validates the returned
identity shape against hello capabilities, and compares the verified artifact
identity with the runtime identity. It does not call `usb.status`,
`hid.release_all`, or any HID report command. Hello and info still create and
refresh the normal control-session lease. `--json` returns one compact object
with `ok`, `match`, `classification`, `artifact`, `device`, `mismatches`, and
`unavailable_reason`; `ok` means the comparison operation completed, not that
the identities matched. The comparison is provenance evidence only, not
physical device authentication, peer authentication, secure boot, or proof of
signed firmware authenticity.

Primitive report CLI grammar is command-local and has no remembered or
environment opt-in:

```text
hidbotctl [GLOBAL...] keyboard-report [GLOBAL...] --unsafe-hid \
  --modifiers N [--key USAGE ...]
hidbotctl [GLOBAL...] mouse-report [GLOBAL...] --unsafe-hid \
  --buttons N --x N --y N --wheel N --pan N
```

`--unsafe-hid` is required. Keyboard modifiers are `0..255`; `--key` is
repeatable for zero through six raw usages and accepts decimal or `0xNN`.
Canonical keyboard validation enforces the allowed usage ranges, strict
ascending order, and no duplicates. Mouse buttons are `0..31` and persistent;
the four relative fields are each `-127..127`, with all five fields required.
The buttons, wheel, and pan paths are implemented and natively validated, but
their physical hardware evidence remains deferred.
Invalid syntax or values fail before transport construction with argparse's
normal exit 2. A valid primitive sends exactly one corresponding HID request
after hello and never sends an automatic release-all. Use the safe
`release-all` command explicitly when persistent state needs recovery.

`hidbotctl self-test` is CLI-only orchestration over one connection and one
session. It runs exactly `hello`, `system.ping`, `system.info`, `usb.status`,
and `hid.release_all` in that order, returning one structured object with
`hello`, `ping`, `info`, `usb_status`, and `release_all` members. It fails fast
on the first exception and performs no cleanup request or reconnect. This
proves only the UART/control-plane operations; it does not prove keyboard or
mouse delivery, evdev events, or physical HID behavior. `hid.release_all` may
submit all-up HID reports when safety state requires it, but the self-test does
not intentionally inject a key, button, or movement.

The characterization and this transport policy were established for the
Freenove ESP32-S3 WROOM Board / FNK0085 CH343 path. The measured true/true
open/close sequence was repeated five times without reset, download boot, or
native HID disconnect. It is a board/host policy, not an authentication
mechanism; OS permissions and exclusive ownership are the local coordination
boundary. The accepted hardware evidence and its limits are maintained in
[`hardware-validation.md`](hardware-validation.md).

## HID runtime and control lease

The `hid_runtime` component owns HID lifecycle state and is the only project
code that calls TinyUSB HID report APIs. `tud_sof_cb_enable(true)` enables the
public TinyUSB SOF callback; `tud_sof_cb()` invokes a bounded, heap-free,
one-slot-per-interface executor (at most one report submission per SOF) in
TinyUSB task context. Lifecycle callbacks
and the executor therefore share one TinyUSB ordering domain. SOF processing
does not log, write UART, parse JSON, wait on a mutex, or allocate.

On ESP32-S3, the DWC2 bus-reset/configuration-reset sequence does not preserve
the SOF enable state established before enumeration. `hid_runtime` therefore
re-arms the SOF callback from `Runtime::on_mount()` for every configured attach,
including re-attach and host reconfiguration. The post-mount SOF path
continuously refreshes the readiness snapshot even when its HID mailbox is
empty. `keyboard_ready` and `mouse_ready` remain the actual TinyUSB endpoint
readiness (including endpoint-busy state), not inferred capability bits.

The internal mailbox has EMPTY/WRITING/READY/EXECUTING/CANCELED states and
stores both the attach generation and authority epoch with each operation.
The executor checks both tokens, mounted state, non-suspended state, and the
safety barrier immediately before TinyUSB report submission. The bounded
release-all ticket remains the only public safety operation; keyboard and mouse
tickets are the explicit public unsafe report paths.

The USB generation remains separate from the authority epoch. A newly accepted
`usb.attach` advances generation once before the public driver install; its
mount callback does not advance it again. An unexpected unmount while the
driver remains exposed advances generation once at the host-loss boundary; a
later mount uses that fresh generation. Conversely, `usb.detach` retains the
old generation only for lifecycle-owned all-up safety work, records its result,
then advances generation exactly once before public driver uninstall. Queued
work is canceled when either token differs at the final executor check.
Uncertainty is not cleared by teardown; a reattach requires fresh-generation
all-up reconciliation before unsafe work is admitted.

Keyboard and Mouse logical state is separate from host-state uncertainty. A
successful submission is provisional until `tud_hid_report_complete_cb`; a
`tud_hid_report_failed_cb` marks that interface uncertain and requires an
all-up safety report, including when the failed report was itself all-up. The
input-report failure callback also publishes a non-blocking notification; the
UART RX task revokes the control session authority before invoking the runtime
safety callback. Host-to-device output reports are not treated as project HID
state and do not trigger this path.
Keyboard and Mouse safety release is independent, so a successful Keyboard
release never clears a pending Mouse release. Only safety all-up reports may
be retried automatically. Unsafe reports that are not ready, are canceled by
detach, suspend, an authority-epoch change, or fail submission are discarded
and never replayed. During suspend, all held, in-flight, or uncertain state is
preserved as a safety-release requirement; late callbacks from that earlier
epoch are ignored. After resume, the executor prioritizes required all-up work
and blocks every unsafe interface until this safety barrier is clear.

`usb.status` reads an atomic runtime snapshot (`mounted`, `suspended`,
`keyboard_ready`, and `mouse_ready`). Valid states include active configured
(`mounted=true, suspended=false`, with readiness varying), suspended configured
(`mounted=true, suspended=true, keyboard_ready=false, mouse_ready=false`),
and unmounted (all false). `link_active = mounted && !suspended` is an internal
concept; the wire schema has no new `link_active` field. The ready fields are
instantaneous endpoint availability, including endpoint-busy state; they are
not a promise that a host will remain attached or that a queued report will be
delivered.
The existing Configuration 1, Boot Keyboard/Mouse interfaces, endpoints
0x81/0x82, descriptors, and VID/PID are unchanged. The public HID commands are
the safety-only `hid.release_all`, the unsafe `hid.keyboard.report` and
`hid.mouse.report` primitives, `hid.route.status` and `hid.route.set`, and
`hid.route.v2.status` and `hid.route.v2.set`.
The optional BOOT-button diagnostic remains build-time
disabled by default and routes any enabled test report through the runtime.

Control sessions have a mandatory 5000 ms lease measured by the monotonic
`esp_timer_get_time()` clock. The existing UART RX loop services expiry after
each bounded read (normally within 100 ms); no additional timer task exists.
Successful hello starts the lease and advertises `lease_ms:5000` plus
`hid.lease-v1`, `hid.release-all-v1`, `hid.keyboard-report-v1`, and
`hid.mouse-report-v1`. A valid current-session request, exact retry, or operational
command result refreshes it. Malformed, epoch-mismatched, session-mismatched,
stale/conflicting, unknown, and semantically invalid requests do not. Expiry
revokes authority first, clears the normal retry cache, and requests runtime
safety release.

A new hello nonce revokes old authority and starts safety release before
activating the new session. Exact hello retry replays its cached bytes and
refreshes the lease only within its captured authority epoch. A hello while
suspended is permitted for diagnostics and captures the suspended epoch, but
resume invalidates it and requires a fresh hello. Lease expiry and takeover
allow diagnostic commands while safety is pending; keyboard and mouse report
paths remain blocked until the global safety barrier is clear.

## Public safety release

`hid.release_all` is the public safety operation. The separate unsafe
`hid.keyboard.report` accepts the
normal no-params request (`params` omitted or `{}`) and reports independent
Keyboard and Mouse outcomes. A successful result is exactly:

```json
{"keyboard":"already_up","mouse":"submitted"}
```

`already_up` means the runtime knows that interface is all-up, not uncertain,
and has no relevant queued or in-flight operation. `submitted` means the
TinyUSB all-up report was accepted by `tud_hid_n_report`; it does not promise
completion or host/OS processing. Cross-endpoint atomicity is not promised.

If either interface cannot be proven safe within the bounded operation window,
the response is the existing error envelope with exactly
`HID_SAFETY_PENDING` / `all-up safety release is pending` and no result object.
Partial release is therefore pending overall; already submitted interfaces are
not duplicated. Suspended or unmounted known-clean interfaces may return
`already_up`; safety-required suspended interfaces remain pending. A lifecycle
authority change returns `SESSION_MISMATCH` and is never cached or lease
refreshed.

Exact retries (same ID and bytes) replay the cached success or pending error
without restarting the operation. A new request ID reevaluates current state.
The `hid.release-all-v1`, `hid.keyboard-report-v1`, and
`hid.mouse-report-v1` capabilities advertise these commands. The public
safety operation is also available as `hidbotctl release-all`. Host APIs are
`Client.release_all()`, `Client.keyboard_report(modifiers, keys)`, and
`Client.mouse_report(buttons, x, y, wheel, pan)`.

## Relative mouse reports

`hid.mouse.report` accepts exactly five required parameters:

```json
{"v":1,"id":8,"session":"<token>","cmd":"hid.mouse.report","params":{"buttons":0,"x":1,"y":0,"wheel":0,"pan":0}}
```

`buttons` is an absolute persistent bitmap in `0..31`: bit 0 is Left, bit 1
Right, bit 2 Middle, bit 3 Backward, and bit 4 Forward. `x`, `y`, `wheel`,
and `pan` are one-report relative deltas in `-127..127`; they are not stored
as logical state. Boolean, fractional, missing, null, and extra fields are
invalid, and `-128` is outside the descriptor's logical range.

The Mouse report uses interface 1, report ID 0, and the five-byte layout
`[buttons, x, y, wheel, pan]`. `submitted` means only that the task-affine
TinyUSB call accepted those bytes; it does not promise host polling or a
cursor event. `already_set` is allowed only for zero deltas with the same
confirmed buttons and no uncertainty, safety work, ticket, or in-flight
report. A nonzero relative delta always submits for a new request ID.

An exact same-ID/byte retry replays the cached response and never submits a
second relative report. Operational failures are cached for exact retries;
new IDs reevaluate current readiness. Mouse tickets capture attach and
authority epochs, use bounded claim/cancel transitions, and are discarded on
timeout, lifecycle invalidation, or safety barriers, so they cannot create a
ghost movement or button transition later. `report_complete` confirms only
buttons; `report_failed` leaves buttons unconfirmed, requires safety release,
revokes authority, and never retries or inverses relative motion.

`hid.release_all` may submit a Mouse all-up report
`[0,0,0,0,0]` when held or uncertain state requires recovery. It never
synthesizes an inverse movement. Mouse unsafe reports are blocked by the same
global `HID_SAFETY_PENDING` barrier as Keyboard reports; `HID_BUSY`,
`HID_NOT_READY`, and `SESSION_MISMATCH` retain their existing meanings.

## BOOT-button diagnostic

The GPIO0 controlled Mouse report diagnostic is build-time opt-in through
`CONFIG_S3_HIDBOT_BOOT_MOUSE_DIAGNOSTIC`, default disabled. Default firmware
does not configure GPIO0 or send a report because of a BOOT-button press.
GPIO0 is active-low and is a boot strapping pin: holding it low during reset
or power-on can select download boot.

## Validation limits

Host-native tests exercise the actual protocol core and ESP-IDF cJSON source:
strict parsing, framing recovery, session lifecycle, cache behavior, output
bounds, and diagnostic command results. Static checks cover the common
writer's bounded lock/flush/raw-write order and the default HID-safety guard.
They do not prove UART serialization under concurrent hardware tasks. A later
hardware gate must capture bounded synthetic common-writer frames alongside
ordinary logs and verify byte integrity, count, and sequence while ignoring
non-protocol text. That test must not send a HID report.

The authority-epoch barrier intentionally does not claim that every physical
OTG cable removal will generate an immediate firmware lifecycle event. The
current board integration has no verified VBUS-comparator GPIO path, so adding
board-evidenced VBUS monitoring remains a future optional hardware improvement.
