# UART Control Plane v1

## Scope and transport invariant

The control plane uses the onboard USB-UART path. Native USB-OTG remains the
separate TinyUSB HID Device path. A host must never assume that opening a serial
device proves that the HID USB path is attached, or vice versa.

U2 establishes a bounded JSON control core for handshake and diagnostics only.
It implements `protocol.hello`, `system.ping`, `system.info`, and `usb.status`.
It has no UART HID command, control lease, asynchronous event, production host
client, USB reconnect, GPIO action, or reset command. Receiving a U2 UART
request cannot send a Keyboard or Mouse report.

The configured ESP-IDF console UART is used without application hardcoding of
its UART number, pins, or baud rate. The UART RX control task is the sole
project-owned consumer of its RX bytes. It does not use stdin, scanf, or a
line-oriented VFS read.

## Framing and synchronization

The future JSON control wire format is:

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

Requests must be bounded JSON objects with no duplicate object key. U2 uses
the ESP-IDF cJSON parser followed by project-owned recursive duplicate-key and
depth/member/string-limit checks. Every envelope allows only its listed fields;
unknown fields fail closed.

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
  The U2 diagnostic commands accept only omitted or empty-object params.

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

Protocol responses and future events must use the common machine writer. It
has a fixed maximum of 1024 bytes including prefix, JSON, and LF. The writer
holds the stdout FILE lock, flushes stdout, then writes the frame through the
configured console UART driver before unlocking. This prevents byte-level
interleaving with normal stdout/VFS writers participating in the same locking
discipline.

The protocol response buffer is fixed at 1024 bytes. All U2 formatters use
bounded `vsnprintf` serialization and fail closed to a bounded
`INTERNAL_ERROR` when a formatter cannot serialize. The hello format has a
compile-time maximum calculation based on bounded metadata, four 32-hex values
(top-level/result session, boot ID, and client nonce), fixed capabilities,
maximum ID, prefix, and LF; host-native tests assert every U2 success and
error response is within the bound.

Machine frames must not use ESP_LOG or printf, and project code must not add a
separate direct UART writer. ESP-IDF ROM boot output, panic output, and any
writer that bypasses stdout locking are outside this guarantee. Host parsers
must therefore accept only complete prefixed frames and ignore other lines.

## Boot epoch and control session

At each MCU boot U2 generates a 16-byte `boot_id` using ESP-IDF
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
protocol.hello-v1, system.ping-v1, system.info-v1, usb.status-v1
```

For a successful hello, top-level `session` equals `result.session`. Both are
the generated control-session token. The echoed nonce identifies this hello
attempt; `boot_id` identifies the MCU boot epoch.

The complete successful-hello shape is:

```json
{"type":"response","v":1,"id":1,"session":"<new-session>","ok":true,"result":{"project":"s3-hidbot","protocol_version":1,"client_nonce":"<request-client-nonce>","boot_id":"<boot-id>","session":"<new-session>","capabilities":["protocol.hello-v1","system.ping-v1","system.info-v1","usb.status-v1"]}}
```

The angle-bracket values above are documentation placeholders only; wire
values are fixed-length lowercase hexadecimal tokens.

Adding a future capability does not itself require changing protocol version
`v`; hosts must use the advertised capability list. U2 does not advertise HID,
release-all, event, or lease capabilities. All normal requests require the
exact current session; USB mount does not automatically create one.

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
caches. The hello cache holds the client nonce, exact normalized JSON bytes,
serialized response, and resulting session:

```text
same nonce + same bytes       replay the cached response only
same nonce + different bytes  CLIENT_NONCE_CONFLICT
new nonce                     establish a new session
```

Exact hello retry neither regenerates a session nor repeats takeover work.

The normal-request cache holds an ID, exact normalized JSON bytes, and complete
serialized result. With the active session, requests follow these rules:

```text
id > last_id                         new request
id == last_id, exact same bytes      cached response replay
id == last_id, different bytes       REQUEST_ID_CONFLICT
id < last_id                          REQUEST_ID_STALE
```

ID gaps are permitted. IDs never wrap; after `2147483647`, the host establishes
a new session. A cache hit replays only the result and never repeats a future
HID action. Pre-command parse, schema, or session errors, stale IDs, and ID
conflicts do not replace the completed-request cache.

## USB unmount boundary

Native USB HID detach is a control-session safety boundary. It revokes the
active session and clears both normal and hello retry caches. An old cached
hello response therefore cannot revive its revoked session after unmount; the
same hello sent again establishes a fresh session. U2 has no held HID state to
restore.

When HID state is introduced, a hello takeover must first clear old logical
state, perform any possible safety release, revoke the old session, and then
activate the new session. U2 does not yet implement that release behavior.

## U2 diagnostic commands

- `system.ping` returns `{ "pong": true }`.
- `system.info` returns bounded static project, target, ESP-IDF version, and
  protocol version. It must not include a user name, host path, serial
  identifier, or build-host information.
- `usb.status` returns current TinyUSB `mounted`, `suspended`,
  `keyboard_ready`, and `mouse_ready` booleans. It queries status only and
  never submits a HID report.

Lifecycle transitions do not produce asynchronous U2 machine events.

## Future U3 host correlation contract

For a normal request, a future host accepts a response only when its type,
protocol version, expected ID, expected current session, and response schema
all match. A wrong-session or malformed frame cannot satisfy the request; the
host should discard it within bounded limits and continue waiting.

For hello, the host additionally requires the expected ID, `ok:true`, exact
`result.client_nonce`, valid top-level and result session tokens that are equal,
valid `boot_id`, expected project/protocol version, and a valid capabilities
list. A wrong-nonce/stale frame must never establish a session. U3 will define
the concrete discard limit and timeout recovery (`TRANSPORT_SYNC` followed by a
fresh hello).

U2.1 is a pre-freeze v1 wire hardening. Protocol `v` remains `1` and the
capability list is unchanged; no new capability is advertised. The nonce
identifies a hello attempt, the boot ID identifies an MCU boot epoch, and the
request ID identifies a request within the current control session.

## Deferred control safety

U2 has no control lease. A later slice will make a bounded mandatory lease part
of the command contract; session validity must not be documented as permanent.
That future lease may be refreshed only by a valid current-session request with
a valid envelope, known command, and semantically valid params. Malformed,
invalid, unknown, and session-mismatched input must not refresh it.

`hid.release_all` and report commands are deferred. A future release-all must
report independent Keyboard and Mouse interface outcomes and cannot promise
cross-endpoint atomicity. Duplicate retries must replay the cached result, not
repeat a side effect.

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
