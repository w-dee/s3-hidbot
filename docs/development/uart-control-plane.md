# UART Control Plane v1

## Scope and transport invariant

The control plane uses the onboard USB-UART path. Native USB-OTG remains the
separate TinyUSB HID Device path. A host must never assume that opening a serial
device proves that the HID USB path is attached, or vice versa.

U1 establishes transport only. It has no JSON parser, commands, HID action,
session, lease, event, or host Python client. In particular, receiving UART
bytes in U1 cannot send a Keyboard or Mouse report.

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
- An overlong prefixed line is discarded through LF. U1 reports a framing
  event only; a later protocol layer will translate it to `LINE_TOO_LONG`.

`TRANSPORT_SYNC` is exactly four consecutive raw NUL bytes:

```text
00 00 00 00
```

It clears only partial framing and overlong-discard state. It does not alter
USB, HID, MCU, UART configuration, session, request cache, or lease state.
One to three NUL bytes are ordinary non-sync input. Five or more NUL bytes are
one synchronization interval; extra NUL bytes are ignored until a non-NUL
byte starts fresh framing.

## Machine-readable output and logs

Future protocol responses and events must use the common machine writer. It
has a fixed maximum of 1024 bytes including prefix, JSON, and LF. The writer
holds the stdout FILE lock, flushes stdout, then writes the frame through the
configured console UART driver before unlocking. This prevents byte-level
interleaving with normal stdout/VFS writers participating in the same locking
discipline.

Machine frames must not use ESP_LOG or printf, and project code must not add a
separate direct UART writer. ESP-IDF ROM boot output, panic output, and any
writer that bypasses stdout locking are outside this guarantee. Host parsers
must therefore accept only complete prefixed frames and ignore other lines.

## Deferred v1 protocol decisions

`protocol.hello` will establish a new control session. A different
`client_nonce` takes over from the old session. Takeover clears old logical HID
state and attempts safety all-up when HID is ready. If it is not ready, a
safety release is pending only for the same USB attach and is attempted before
new-session HID action. Unmount discards that pending release.

Hello retry state and normal command retry state are separate. An exact repeat
of the same hello request for the same `client_nonce` replays its serialized
response; the session is not recreated. The same nonce with different bytes is
rejected fail closed.

Within a session, normal requests follow these rules:

```text
id > last_id                         new request
id == last_id + exact same bytes     cached response replay
id == last_id + different bytes      REQUEST_ID_CONFLICT
id < last_id                          REQUEST_ID_STALE
```

ID gaps are permitted. IDs never wrap; the host establishes a new session
before the signed 32-bit maximum. Retry replay never repeats a HID action.

The outbound frame bound is 1024 bytes. Side-effecting commands need not
serialize a response before HID submission, but every response shape they can
produce must be statically bounded to fit the fixed output buffer without an
allocation or overflow failure path.

The future control lease is refreshed for a valid current-session request with
a valid envelope, known command, and semantically valid parameters, even if
HID execution returns USB-not-mounted, HID-not-ready, or HID-send-failed. It
is not refreshed by malformed, invalid, unknown, or session-mismatched input.
Lease expiry revokes the session; later requests fail as `SESSION_MISMATCH`.

`hid.release_all` will report per-interface results and does not promise
atomicity across the separate Keyboard and Mouse endpoints. Duplicate retries
replay that result only; a host wanting another attempt uses a new request ID.

## BOOT-button diagnostic

The GPIO0 controlled Mouse report diagnostic is build-time opt-in through
`CONFIG_S3_HIDBOT_BOOT_MOUSE_DIAGNOSTIC`, default disabled. Default firmware
does not configure GPIO0 or send a report because of a BOOT-button press.
GPIO0 is active-low and is a boot strapping pin: holding it low during reset
or power-on can select download boot.

## Validation limits

Host-native tests cover framing, line boundaries, and transport sync. Static
checks cover the common writer's bounded lock/flush/raw-write order. They do
not prove ESP-IDF UART serialization under concurrent hardware tasks. A later
hardware gate must use a diagnostic test build that starts several bounded
ESP_LOG/stdout producer tasks and one common-writer producer. The common writer
emits uniquely numbered synthetic frames of varied sizes through its normal
1024-byte limit. A host capture must verify exact byte integrity, line count,
and monotonic synthetic-frame sequence while ignoring ordinary logs. That test
does not send a HID report and must run before any UART HID command validation.
