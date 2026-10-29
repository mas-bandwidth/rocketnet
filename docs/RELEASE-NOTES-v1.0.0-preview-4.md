# RocketNet v1.0.0-preview-4

Changes compared with the `v1.0.0-preview-3` tag. The tested head and the
recorded results are in [the qualification index](QUALIFICATION-v1.0.0-preview-4.md)
and, once the tag is cut, in the
[GitHub release](https://github.com/mas-bandwidth/rocketnet/releases/tag/v1.0.0-preview-4).

RocketNet provides encrypted client/server UDP connections and eight message
delivery modes through a C API. Your application supplies message bytes and
drives the same five-step frame loop: update, receive, simulate, queue messages,
then send packets.

## Migrating from preview-3

Build against the preview-4 header and library. Two changes can need code.

### Loopback needs a hub you create

The loopback transport no longer keeps a process-wide table. A caller-owned
hub replaces it, so a server and a client built into separate shared libraries,
each with its own copy of RocketNet, can connect to each other.

- `rocketnet_loopback_hub_create()` makes a hub for one server and 1 to
  `ROCKETNET_MAX_CLIENTS` clients, fixed at create.
  `rocketnet_loopback_hub_destroy()` frees it.
- `rocketnet_server_config_t` and `rocketnet_client_config_t` have a new last
  field, `loopback_hub`. It defaults to NULL. Set it on every endpoint that
  uses loopback.
- A create that names loopback with a NULL hub fails with
  `ROCKETNET_STATUS_LOOPBACK_HUB_MISSING` (-32). A hub made by a different
  RocketNet build fails with `ROCKETNET_STATUS_LOOPBACK_HUB_MISMATCH` (-33).
  Destroying a hub while an endpoint is still registered is refused with
  `ROCKETNET_STATUS_LOOPBACK_HUB_IN_USE` (-34), and the hub is kept.
- The hub's memory is one block allocated at create through your allocator.
  Start and connect allocate nothing for it, and traffic never allocates.
- A create from the earlier, shorter configuration layout cannot name a hub,
  so its loopback creates are refused. Use the current configuration size.
  [ABI-EVOLUTION.md](ABI-EVOLUTION.md) records this exception.

### Disconnect reasons and the client state callback

- `rocketnet_server_disconnect_reason()` no longer reads NONE when the
  transport ended a slot. `ROCKETNET_DISCONNECT_REASON_TIMED_OUT` (-7) means
  nothing arrived from the client within its connection timeout.
  `ROCKETNET_DISCONNECT_REASON_CLIENT_DISCONNECTED` (-8) means the client
  disconnected. A disconnect the server starts, with
  `rocketnet_server_disconnect_client()` or
  `rocketnet_server_disconnect_all_clients()`, now reads
  `ROCKETNET_DISCONNECT_REASON_SERVER_DISCONNECTED` (it read NONE). A stopped
  server still reads NONE for every slot, and an end at the integrity limit
  reads `PROTOCOL_VIOLATION`. Code that tested for NONE to mean "the transport
  ended it" needs the new values.
- The client state callback reports the client's public state, every change
  exactly once. It no longer reports the transport's state. A server kick
  reports CONNECTED, then `SERVER_DISCONNECTED`. Connect-time token refusals
  and the move from DISCONNECTED to REQUESTING_COOKIE are reported too.

## Wire compatibility

None. The packet encodings, the connect-token encoding, the protocol version
and the standard ([TRANSPORT.md](../standard/TRANSPORT.md),
[MESSAGES.md](../standard/MESSAGES.md)) are unchanged since preview-3.

## Other changes since preview-3

- **Configuration.** `num_channels` defaults to the smaller of 32 and
  `ROCKETNET_MAX_CHANNELS`, and its range is 1 to `ROCKETNET_MAX_CHANNELS`.
  The cross-field configuration rules are written in the header and in
  [USAGE.md](USAGE.md). The light configurations carry messages of at most
  1415 bytes, each in at most two parts.
- **Misuse is counted and logged.** A message left in the receive queue at
  update, invalid start-message arguments, and a non-finite or backwards update
  time are counted as API misuse and logged in every build.
- **Receipts and statuses.** Receipts stay readable after an explicit
  disconnect until the next update. `THREAD_CREATE_FAILED` comes from connect,
  and a server start that finds no usable transport reports
  `TRANSPORT_LADDER_EXHAUSTED`.
- **Documentation.** The header, the guides and the TLA+ models were checked
  against the code, and every place where they differed is corrected. The
  shutdown helpers in the examples take the caller's last update time.
- **Message layer.** Every send queue costs a fixed amount per operation, so
  queueing does not slow as the queues fill. The token tables' claims ring
  keeps every live claim when it compacts after wrapping.
- **macOS.** Network threads run at the highest real-time round-robin
  priority, above every ordinary thread. Linux is unchanged.
- **Smaller, simpler code.** Dead code, unused helpers and duplicated
  per-platform copies were removed across the core, platform, protocol,
  token-table, session and message layers. The POSIX platforms share one
  implementation. Behaviour is unchanged by that work.
- **Fuzzing.** A macOS libFuzzer run with AddressSanitizer and
  UndefinedBehaviorSanitizer covered every macOS fuzz target for 900 seconds
  each and found no crash ([record](runs/macos-fuzz.md)).

See [BUILDING.md](BUILDING.md), [TRANSPORTS.md](TRANSPORTS.md) and
[USAGE.md](USAGE.md) for the supported configuration and API contracts.

## Known limits

- A default 1,000-client server requires about 20.9 GB, and the light profile
  about 3.2 GB, on Linux ([USAGE.md](USAGE.md)). Memory is spent to save CPU
  and bandwidth and is taken up front. The plan leaves out what the operating
  system owns: worker stacks, XDP UMEM, the AF_XDP and io_uring rings and
  kernel socket buffers. Provision host memory for both.
- The io_uring log does not yet name `RLIMIT_MEMLOCK` when create fails; size
  that limit for every io_uring server under one account.

The [qualification index](QUALIFICATION-v1.0.0-preview-4.md) records the
tested head and every gate, including those still owed at tag time. The
[testing guide](TESTING.md) explains the current evidence sources and limits.
