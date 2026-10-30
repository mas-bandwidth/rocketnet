# RocketNet v1.0.0-preview-5

Changes compared with the `v1.0.0-preview-3` tag, including everything in
`v1.0.0-preview-4`.

- Tag: `v1.0.0-preview-5`, on the commit that adds these notes.
- Release: [GitHub release](https://github.com/mas-bandwidth/rocketnet/releases/tag/v1.0.0-preview-5).

RocketNet provides encrypted client/server UDP connections and eight message
delivery modes through a C API. Your application supplies message bytes and
drives the same five-step frame loop: update, receive, simulate, queue messages,
then send packets.

## Security fixes

The first fixed version of each item below is `v1.0.0-preview-5`. No advisory
identifier has been assigned; see [SECURITY.md](SECURITY.md) for the disclosure
terms.

- **Reliable-sequenced block release.** Cancelling an incomplete
  reliable-sequenced block released receive descriptors by message number
  alone. It now releases only descriptors the block owns. A live descriptor
  that belongs to another message ends that connection with
  `PROTOCOL_VIOLATION` and is counted. The held-message index is bounded: an
  entry binds the message number of its descriptor, the drain checks that the
  descriptor is live and belongs to the channel before reading it, an insert
  past the window is refused, and the erase search is bounded. Together these
  prevent a stale delivery and a server update that never returns. Only the
  offending connection is ended; valid traffic takes the same paths as before.
- **Session send sequence.** Every session send now takes its sequence in one
  atomic step, including keep-alive, standalone ack and disconnect packets. No
  two packets under one directional session key can carry the same sequence,
  which is the AEAD nonce, whatever order sends are driven in. Inbound ack
  blocks are reconstructed against the highest sequence actually sent. The
  per-request and per-attempt ephemeral key generation is checked, and a failure
  refuses the request and wipes the keys.
- **Example backends.** The Go and C# reference backends listened on all
  interfaces. They now listen on `127.0.0.1` by default. Set
  `ROCKETNET_BACKEND_LISTEN_ADDRESS` to listen elsewhere; a non-loopback
  address logs a warning. The examples do not authenticate registration or
  token requests, so a production backend must add authenticated registration
  and player authorization.
- **Hardening.** Environment reads use `secure_getenv` on glibc. The resend queue checks every link index it follows
  against the window; a corrupt link is reported as a protocol violation instead
  of being followed.

## Migrating from preview-3

Build against the preview-5 header and library. Start every configuration from
`rocketnet_server_default_config` and `rocketnet_client_default_config`.

### Loopback needs a hub you create

The loopback transport no longer keeps a process-wide table. A caller-owned
hub replaces it, so a server and a client built into separate shared libraries,
each with its own copy of RocketNet, can connect to each other.

- `rocketnet_loopback_hub_create()` makes a hub for one server and 1 to
  `ROCKETNET_MAX_CLIENTS` clients, fixed at create.
  `rocketnet_loopback_hub_destroy()` frees it.
- Both configurations have a `loopback_hub` field. It defaults to NULL. Set it
  on every endpoint that uses loopback.
- A create that names loopback with a NULL hub fails with
  `ROCKETNET_STATUS_LOOPBACK_HUB_MISSING` (-32). A hub made by a different
  RocketNet build fails with `ROCKETNET_STATUS_LOOPBACK_HUB_MISMATCH` (-33).
  Destroying a hub while an endpoint is still registered is refused with
  `ROCKETNET_STATUS_LOOPBACK_HUB_IN_USE` (-34), and the hub is kept.
- The hub's memory is one block allocated at create through your allocator.
  Start and connect allocate nothing for it, and traffic never allocates.

### Disconnect reasons and the client state callback

- `rocketnet_server_disconnect_reason()` no longer reads NONE when the
  transport ended a slot. `ROCKETNET_DISCONNECT_REASON_TIMED_OUT` (-7) means
  nothing arrived from the client within its connection timeout.
  `ROCKETNET_DISCONNECT_REASON_CLIENT_DISCONNECTED` (-8) means the client
  disconnected. A disconnect the server starts, with
  `rocketnet_server_disconnect_client()` or
  `rocketnet_server_disconnect_all_clients()`, reads
  `ROCKETNET_DISCONNECT_REASON_SERVER_DISCONNECTED` (it read NONE). A stopped
  server still reads NONE for every slot, and an end at the integrity limit
  reads `PROTOCOL_VIOLATION`. Code that tested for NONE to mean "the transport
  ended it" needs the new values.
- The client state callback reports the client's public state, every change
  exactly once. A server kick reports CONNECTED, then `SERVER_DISCONNECTED`.
  Connect-time token refusals and the move from DISCONNECTED to
  REQUESTING_COOKIE are reported too.

### Configuration

- New last fields on both configurations: `server_secret_key` and
  `token_epoch_unix_seconds` on the server, and `token_epoch_unix_seconds` on
  the client. Structs grow only at their end
  ([ABI-EVOLUTION.md](ABI-EVOLUTION.md)); new size constants name the new
  revisions.
- `server_secret_key` (32 bytes, `ROCKETNET_SERVER_SECRET_KEY_BYTES`) set to all
  zeros, the default, makes create generate a keypair as before. Any other value
  is used as the server's X25519 secret key, and `rocketnet_server_public_key`
  returns the matching public key. The server wipes its copy at destroy; wipe
  your own copy after create. A production build refuses the published
  development server key at start with
  `ROCKETNET_STATUS_DEV_KEYS_IN_PRODUCTION`.
- `token_epoch_unix_seconds` sets the token clock to that epoch plus your update
  time. Zero reads the wall clock once: the client at connect, the server at
  its first update after start. The server used to read the wall clock on every
  update, so a paused update clock now pauses token expiry. The
  `ROCKETNET_*_GAME_CLOCK_EPOCH` environment variables are renamed
  `ROCKETNET_*_TOKEN_EPOCH_UNIX_SECONDS`; update any setting that used the old
  names.

### Connect tokens

Production builds can mint connect tokens in process through the public header:
`rocketnet_generate_backend_keypair`, `rocketnet_generate_server_keypair`,
`rocketnet_connect_token_default_config` and `rocketnet_create_connect_token`.
The call reads no clock (the time is `unix_timestamp` in the token
configuration) and allocates nothing. A production build refuses the published
development backend or server key. Mint tokens with ephemeral keys in process
only, and never compile a real backend secret into a shipped client.
`rocketnet_create_dev_connect_token` is a thin wrapper over it with the same
behaviour. A connect token string is exactly 1024 characters, as the code
always required; the header said "at most" and now says exactly. See
[KEYS.md](KEYS.md).

## New in the public API

- `rocketnet_server_client_connection_id()` returns a nonzero id while a client
  is connected in a slot and 0 when the slot is free. Ids are distinct and never
  repeat across a stop and start. Compare a slot's id after each update to see a
  disconnect and reconnect within one update.
- `rocketnet_max_unfragmented_message_bytes( reliability, channel )` returns the
  largest message that fits one record for a mode and channel, or 0 for a bad
  mode or channel. It is legal in production and before init. It returns the
  limits in [MESSAGES.md m2.1](../standard/MESSAGES.md), including 1398 bytes
  for reliable-sequenced on channels 256 and above, a row the standard did not
  list before.
- The development keys `ROCKETNET_DEV_BACKEND_SECRET_KEY`,
  `ROCKETNET_DEV_SERVER_PUBLIC_KEY` and `ROCKETNET_DEV_SERVER_SECRET_KEY` join
  `ROCKETNET_DEV_BACKEND_PUBLIC_KEY`, in development builds only.
- `rocketnet_server_stop` flushes a disconnect to every connected client before
  it stops the transport. This is now stated in the header and
  [USAGE.md](USAGE.md).
- A CMake consumer that adds RocketNet with `add_subdirectory` sees only
  `rocketnet.h` through the library target. No internal header is reachable
  from it. [BUILDING.md](BUILDING.md) documents this as the supported entry.
- The connect-token string format is part of the standard
  ([TRANSPORT.md t6.5](../standard/TRANSPORT.md#t6.5)), with published vectors
  in `standard/vectors/connect_token_v1.json`.
- `api_misuses` in the client and server statistics, and
  `ROCKETNET_DISCONNECT_REASON_MESSAGE_NOT_RECEIVED` for an unreleased message,
  are shown in [AGENTS.md](AGENTS.md).

## Wire compatibility

The packet encodings, the connect-token encoding and the protocol version are
unchanged. The standard ([TRANSPORT.md](../standard/TRANSPORT.md),
[MESSAGES.md](../standard/MESSAGES.md)) was corrected in this release, and some
corrections change what a receiver does:

- **Reliable-sequenced records.** A reliable-sequenced record any distance
  ahead of the read index is held and never refused, because a sender's ordered
  backlog legitimately puts it ahead. Previously a record a window or more ahead
  was refused as a violation, which ended the connection of a sender that was
  behaving correctly. Rules 7 and 9 of
  [MESSAGES.md m6.1](../standard/MESSAGES.md#m6.1) changed, and two conformance
  vectors with them.
- **Fragment anchors.** A fragment that arrives at connection start can no
  longer claim a block anchor before message zero in a reliable-ordered or
  reliable-sequenced channel. Valid 24-bit wrapping is unchanged
  ([MESSAGES.md](../standard/MESSAGES.md)).
- **Clarifications that describe the existing format.** Conflicting unreliable
  size rules were resolved, fragment encoding and ring offsets were stated
  unambiguously, the payload authentication-tag offset was corrected, timing
  units were made uniform, and the claim of Ed25519 signature uniqueness was
  replaced by token identity from the signed connect data. The server generates
  a fresh keypair for every mapping. The standard gains a reading guide with a
  handshake walkthrough and a signature vector. RN-1 no longer claims that an
  implementation built from it alone interoperates with every other.

## Other changes

- **Configuration.** `num_channels` defaults to the smaller of 32 and
  `ROCKETNET_MAX_CHANNELS`, and its range is 1 to `ROCKETNET_MAX_CHANNELS`.
  The cross-field configuration rules are written in the header and in
  [USAGE.md](USAGE.md).
- **Receipts and statuses.** Receipts stay readable after an explicit
  disconnect until the next update. `THREAD_CREATE_FAILED` comes from connect,
  and a server start that finds no usable transport reports
  `TRANSPORT_LADDER_EXHAUSTED`.
- **Message layer.** Every send queue costs a fixed amount per operation, so
  queueing does not slow as the queues fill.
- **macOS.** Network threads run at the highest real-time round-robin
  priority, above every ordinary thread. Linux is unchanged.
- **Linux GSO server.** A send refused for one destination (for example
  `ENETUNREACH`, `EHOSTUNREACH`, `EPERM` or `EACCES`) dropped every later message
  in the flush. It now drops only the refused message and continues. A full socket (`EAGAIN`, `EWOULDBLOCK`, `ENOBUFS`,
  `ENOMEM`) or `EINTR` still ends the flush. Dropped messages are counted in
  `ROCKETNET_SERVER_GSO_COUNTER_SEND_DROPPED` and never resent
  ([LINUX.md](LINUX.md)).
- **Linux io_uring server.** The receive reap no longer overfills the receive
  bank. A completion that does not fit stays queued for the next call instead of
  being dropped, and is counted in `ROCKETNET_SERVER_URING_COUNTER_REAP_DEFERRED`.
  `RECEIVE_DROPPED_OVERFLOW` now counts only drops during destroy's drain.
  `REAP_DEFERRED` is now counted only when a reap ran: a send call with nothing
  to send or reap no longer counts the receive completions waiting between
  updates ([LINUX.md](LINUX.md)).
- **Loopback.** A client receive skips an oversized packet until it finds one
  that fits, so a bad packet no longer holds up valid replies behind it.
- **Windows.** The IOCP client re-posts its wake after a refused post, and the
  sleep fallback no longer sets a timer period of its own.
- **Linux GSO client.** The receive drain skips at most one batch of
  zero-length or unconvertible datagrams per call.
- **Counters.** A span outside the receive ring is counted as
  `MESSAGE_BAD_RING` on all three reliable modes, as
  [MESSAGES.md](../standard/MESSAGES.md) check 8 says. Reliable-sequenced
  block spans and some fragment spans were counted as `MESSAGE_BAD_BLOCK`. Only
  the counter changes; the connection is handled as before.
- **Send store.** Two allocator corrections. A search for a run of free chunks
  could refuse a request although a large enough run straddled the scan point.
  Production builds cannot reach this: only messages up to 1152 bytes use the
  store, and they take one chunk. A single chunk's generation tag also wrapped
  to 0 after 1023 reuses; it now wraps to 1, because 0 is never a live tag.
- **Endpoint.** A large sequenced record dropped over budget is no longer
  charged to the per-update packet allowance.
- **XDP.** A CPU list entry beyond the range of `int` is refused before it can
  overflow.
- **Examples.** The receipt loops in `ack_receipts.c` and `reliability.c` stop
  at the 32 entries their arrays hold.

## Documentation

- The header and the guides were checked against the code, and every place
  where they differed is corrected.
- The environment-variable tables in [ENVIRONMENT.md](ENVIRONMENT.md) and
  [USAGE.md](USAGE.md) now match the public structs. They add
  `disconnect_packets_per_update` and `handshakes_per_update` on the server and
  `ROCKETNET_DEV_SESSION_SEQUENCE_START` (development builds only). They also
  state that `token_epoch_unix_seconds` has an environment form. No code changed.
- The guides, tests, benchmarks, XDP sources and formal models are reorganised
  under `docs`, `tests`, `benchmarks`, `sdk/xdp` and `proofs`. Executable names,
  the public API and runtime behaviour are unchanged by the move. Start from
  [README.md](../README.md).
- The standard has a reading guide, and the corrections above carry a review
  record in [audits](audits/).

## Internal

Dead code, unused helpers and duplicated per-platform copies were removed, the
POSIX platforms share one implementation, and formal models were added. No API
or wire change.

## Provisioning

A default 1,000-client server requires about 20.9 GB, and the light profile
about 3.2 GB, on Linux ([MEMORY.md](MEMORY.md)). Memory is spent to save CPU and
bandwidth and is taken up front. The plan leaves out what the operating system
owns: worker stacks, XDP UMEM, the AF_XDP and io_uring rings and kernel socket
buffers. Provision host memory for both.

See [BUILDING.md](BUILDING.md), [TRANSPORTS.md](TRANSPORTS.md) and
[USAGE.md](USAGE.md) for the supported configuration and API contracts, and
[TESTING.md](TESTING.md) for the test suites.
