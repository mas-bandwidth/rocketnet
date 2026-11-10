# RocketNet v1.0.1

Changes compared with the `v1.0.0` tag.

- Tag: `v1.0.1`, on the commit that adds these notes.
- Release: [GitHub release](https://github.com/mas-bandwidth/rocketnet/releases/tag/v1.0.1).

RocketNet provides encrypted client/server UDP connections and eight message
delivery modes through a C API. Your application supplies message bytes and
drives the same five-step frame loop: update, receive, simulate, queue messages,
then send packets.

v1.0.1 is a fix release. It adds no function, configuration field, status
value or environment variable, and it changes no byte on the wire.

## Upgrading from v1.0.0

Rebuild against the v1.0.1 header and library. No source change is required
unless your code relies on one of the behaviours listed under "Behaviour you
can observe".

### Wire compatibility

The packet types, the packet and prefilter layouts, the connect-token encoding,
the message record encodings and the protocol version (`0x4E7C1A93`) are
unchanged. A v1.0.0 endpoint and a v1.0.1 endpoint interoperate in both
directions: a v1.0.1 client connects to a v1.0.0 server, and a v1.0.0 client
connects to a v1.0.1 server.

One receiver rule changed, without a change to any encoding (see "Standard"
below): a late fragment of a reliable-sequenced block that a newer message on
the same channel has already superseded is now dropped and counted as
`MESSAGE_SEQUENCED_STALE`. A v1.0.0 receiver does not apply this rule, so
upgrade both endpoints to have it on both sides.

### Public API

`rocketnet.h` changed in its comments only. No declaration, struct field, enum
value or export was added, removed or renumbered; code built against v1.0.0
compiles against v1.0.1 unchanged. The counter registry
([TRANSPORT.md t18](../standard/TRANSPORT.md#t18)) gains one counter at its
end, `DISCONNECT_BURST_DROPPED`; no existing counter moves.

### Behaviour you can observe

- **Client state callback.** A call that changes the client (connect, destroy,
  update, send_packets, disconnect, the message, receipt and send rate calls,
  network_simulator) made from inside `state_change_callback` is now refused at
  once, leaves the client as it was, and counts one `api_misuses`. In
  particular, `rocketnet_client_destroy` called from the callback returns
  without destroying the client: destroy it after the callback returns.
- **Log callback.** The log callback must not call any rocketnet function
  ([USAGE.md](USAGE.md#threads)). The server now writes its disconnect log
  lines after the slot has been closed.
- **Server create.** `rocketnet_server_create` returns `NULL` with
  `ROCKETNET_STATUS_CRYPTO_INIT_FAILED` when the server keypair cannot be
  generated, or its public key cannot be derived from a supplied
  `server_secret_key`. Nothing is created. This refusal writes no error line.
- **Client token expiry at connect.** With a nonzero
  `token_epoch_unix_seconds`, `rocketnet_client_connect` checks the token's
  expiry against that epoch plus the time of the client's most recent update
  (zero if it has never been updated), and never reads the wall clock. v1.0.0
  checked against the epoch alone, so it could accept a token already expired
  at the caller's time, and the connect attempt ran until it timed out; v1.0.1 returns
  `ROCKETNET_STATUS_CONNECT_TOKEN_EXPIRED` from connect at once. The epoch
  must be the Unix time at which the clock you pass to
  `rocketnet_client_update` reads zero, as the header has always required: a
  game that sets an epoch but passes a clock running ahead of it can now have
  a valid token refused at connect. A connect before the client's first
  update is checked against the epoch alone. An epoch of 0 still reads the
  wall clock once, at connect ([SECURITY.md](SECURITY.md)).
- **`rocketnet_client_index`.** A connected client's index is the slot the
  server assigned. A debug build no longer asserts when that index is at or
  past the client build's `ROCKETNET_MAX_CLIENTS`.
- **`send_message_buffer_bytes`.** Its range is unchanged (`12288 ..
  268435456`), but it no longer has to be about three times
  `max_message_bytes`: a message larger than 1152 bytes is held in send space
  sized from `receive_message_buffer_bytes`. A configuration that v1.0.0
  refused for that reason with `ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE` is now
  created ([LOAD.md](LOAD.md)).
- **Reliable timeout.** A reliable message whose first send the transport
  refused is no longer treated as sent: its reliable timeout starts when it
  first goes on the wire.
- **Counters.**
  - `PREFILTER_TOO_LARGE` also counts a custom transport's out-of-range
    results on the client and the server (a negative or over-capacity count, a
    NULL receive view, an out-of-range send count), in addition to datagrams
    above the receive ceiling.
  - `SEND_RING_FULL` and `SEND_NO_LINK_ADDRESS` on the server's receive path
    count only a response that was refused, and a refused unreliable send
    counts one transport refusal.
  - A disconnect burst dropped because the server's disconnect ring is at its
    cap is counted in `DISCONNECT_BURST_DROPPED` and logged at debug level.
- **Memory.** `rocketnet_server_required_memory` reports more than v1.0.0 for
  the same configuration: the encryption map's index has two cells per mapping, the
  connect-token bindings table keeps an insertion-order ring, and on Linux the
  XDP worker's receive frame arrays are part of the plan. On the Linux x86-64
  development build with io_uring and XDP, the light profile's server at
  `max_clients` 1000 plans 3,111,387,923 bytes and its client at most
  15,195,570 bytes. Query your own build before allocating
  ([MEMORY.md](MEMORY.md)).
- **Sockets.** Every socket and wake handle the library opens is
  non-inheritable (close-on-exec on Linux and macOS, no handle inheritance on
  Windows), so a child process you start no longer holds them. An IPv6
  dual-stack socket also sets IPv4 Don't-Fragment for IPv4-mapped traffic.

## Fixes

### Connections and messages

- A late fragment of a superseded reliable-sequenced block no longer stays
  held; it is dropped as stale and the connection continues.
- A client connect attempt that ends before any cookie arrives closes its
  second address family's socket instead of keeping both open and polling.
- The client's I/O thread retries a send the transport refused for the moment
  after 2 ms, instead of at once.
- The server's encryption map keeps its lookups and removals short when the
  map is full.
- A full connect-token bindings table takes its oldest expired binding
  directly.
- The server checks a staged transport's receive view before reading it, and
  the write pass respects the payload capacity a transport reports for a
  frame.
- Server stop finishes when a disconnect burst cannot be sealed, and a create
  that fails after its I/O worker has started stops waiting for that worker
  after one second.

### Linux

- **gso and xdp receive.** A packet that arrives before the update that claims
  the receive bank is delivered by that update, not one or two updates later.
- **gso send.** One destination that refuses UDP segmentation no longer turns
  segmentation off for the others. A send interrupted by a signal is retried at
  once. After stop, a socket that stays full no longer keeps the transport from
  reaching stopped.
- **gso receive.** The receive walk rotates its starting queue, so a busy early
  queue cannot starve the later ones, and the worker backs off for 1 ms instead
  of spinning when it cannot publish a bank or a read keeps failing.
- **io_uring.** Sends are spread across the socket pool, so a burst larger than
  one socket's send buffer uses its siblings.
- **xdp.** A hard error when waking the kernel's transmit ring is counted in
  the transport's send failures and logged once per run of errors, and stop no
  longer waits forever on that queue. The gateway refresh asks for the known
  gateway's neighbour entry directly instead of reading the kernel's whole
  route and neighbour tables. The worker's receive frames live in its queue
  rather than on its thread stack.
- **portable.** After a full socket, the send worker waits for the socket to
  become writable and retries within 1 ms.

### Windows

- **IOCP server.** Releasing a send operation no longer retires it when the
  free list is briefly busy, so stop completes once every send has finished. A
  packet length out of range is refused in release builds and counted as
  `SEND_FAILED`, as the RIO server does.
- **RIO server.** Stop drains every completion batch in one pass, and a create
  that fails part way returns within a bounded time.
- **IOCP client.** Posted I/O is cancelled before its socket is closed.

### All platforms

- The spin lock that guards the sent-packet history pauses while it spins and
  yields after a bounded number of attempts.
- Development builds only: each client's network simulator has its own seed,
  server stop drains the simulator before the transport stops, and taking a
  pending simulator setting never blocks the send path.

## Standard

[MESSAGES.md m7.3](../standard/MESSAGES.md#m7.3) now states two receiver rules
the library already followed or now follows:

- A reliable-sequenced record any distance ahead of its ordering epoch is held
  and never refused for its distance (rules 4 and 6), as the m7 vectors and the
  unreliable-sequenced rules already said.
- A fragment of a reliable-sequenced block that a newer message superseded is
  dropped and counted as stale when another fragment of the same block was
  already accepted and released (rule 8).

[TRANSPORT.md t8](../standard/TRANSPORT.md#t8) counts the nine assigned packet
types and names the first reserved type, as the registry in
[t3](../standard/TRANSPORT.md#t3) does. The `send rate` row moved from the t3
table to [t17](../standard/TRANSPORT.md#t17) with the same value, and the t18
counter registry and its vector gained `DISCONNECT_BURST_DROPPED`. The
revision history in [RATIONALE.md](../standard/RATIONALE.md#rh) lists these
changes. The document version and the protocol version are unchanged.

## Documentation

- The header and the guides state the contracts above: the state and log
  callback rules ([USAGE.md](USAGE.md)), the server create status
  ([ERRORS-AND-LOGGING.md](ERRORS-AND-LOGGING.md)), the keypair generators'
  statuses ([KEYS.md](KEYS.md)), the `send_message_buffer_bytes` rule
  ([LOAD.md](LOAD.md), [ENVIRONMENT.md](ENVIRONMENT.md)), and the client
  simulator hand-over ([CLIENT-IO.md](CLIENT-IO.md)).
- The header states that `num_xdp_queues` above 32 is refused by the gso and
  io_uring transports, that an overlong `ROCKETNET_CLIENT_TRANSPORT` or
  `ROCKETNET_SERVER_TRANSPORT` value returns
  `ROCKETNET_STATUS_CONFIG_ENV_INVALID`, and that a loopback hub's block sits
  outside both memory plans.

See [BUILDING.md](BUILDING.md), [TRANSPORTS.md](TRANSPORTS.md) and
[USAGE.md](USAGE.md) for the supported configuration and API contracts.
