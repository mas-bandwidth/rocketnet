# RocketNet v1.0.0-preview-2

Draft for the next preview. Source changes below are present at
`eaf420df45614fe1b0d8f1152b76b815d33cfdd2`, compared with the
`v1.0.0-preview-1` tag. Final release testing and the release commit are pending;
this document does not announce a published release. See [TESTING.md](../TESTING.md)
for qualification and [the release procedure](RELEASING.md) for the cut.

RocketNet provides encrypted client/server UDP connections and eight message
delivery modes through a C API. Your application supplies message bytes and
drives the same five-step frame loop: update, receive, simulate, queue messages,
then send packets.

## Migrating from preview-1

### Request shutdown, check completion, then destroy

Client disconnect and server stop request worker shutdown without waiting.
Keep the endpoint, allocator and callback contexts alive, and poll
`rocketnet_client_disconnected()` or `rocketnet_server_stopped()` before
destroying it. A client state of `DISCONNECTED` alone does not prove its
worker has released storage. Caller-stepped clients progress through update.

Destroy asserts the completion precondition, then synchronously joins and
frees resources. It has no promised wall-clock bound. A stuck worker leaves
completion false; it does not permit early destruction. An endpoint that has
never started or connected is already complete.

Wait for server completion before restarting. A client retry from a terminal
failure can return `ROCKETNET_STATUS_CLIENT_NOT_DISCONNECTED` while the old
attempt shuts down; poll completion and retry with the token. See
[USAGE.md](../USAGE.md) and the [public header](../sdk/rocketnet.h).

### Bound receipt iteration by the output capacity

`rocketnet_client_receipts()` and `rocketnet_server_receipts()` now return
the number of available outcomes, which can exceed the number copied into
your output buffer. Iterate over at most `min(returned_count, capacity)`
entries. Passing a NULL output with zero capacity queries the available count.
If the count exceeds capacity, read again into a larger buffer before the
next update; reads do not consume outcomes. The signatures and receipt layout
are unchanged, but preview-1 callers must adopt this return-value contract.
See [receipt reads after preview-1](ABI-EVOLUTION.md#receipt-reads-after-preview-1).

### Recheck client quality thresholds

Client `jitter_ms` now uses retained first-ACK RTT samples: the mean excess
over their minimum. Client `packet_loss_percent` now estimates outbound
unacknowledged session packets from the eligible retained send window, excluding
the newest half-window. Delayed ACKs affect that estimate, and the window advances
with sent sequences rather than elapsed time. These replace the previous client
receive-jitter and receive-loss measures. Recalibrate dashboards and thresholds
that used those fields; see [the statistics example](examples/stats.md).

### Select a production build explicitly

`ROCKETNET_DEVELOPMENT` is replaced by `ROCKETNET_DEV`, which defaults to 1.
Release optimization does not select a production build. Configure CMake with
`-DROCKETNET_DEV=OFF` and link the production target before deployment.

Production builds exclude development helpers and refuse published development
keys, tokens sealed to the development server key, and the test packet filter.
The old `ROCKETNET_STATUS_DEV_KEYS_IN_RELEASE` name becomes
`ROCKETNET_STATUS_DEV_KEYS_IN_PRODUCTION`. Set your own packet filter and
backend key, and issue connect tokens from your backend. Go and C# backend
examples are under [examples/backend](../examples/backend).

The portable server transport is development-only; production servers require
an available native transport. Portable clients remain available. Explicit
transport pins refuse unavailable configurations rather than silently falling
back. Follow [Before production](../AGENTS.md#before-production).

## Wire compatibility

Packet and connect-token encodings and the protocol version are unchanged.
Preview-1 and preview-2 SDK peers remain encoding-compatible, with tighter
receive and handshake acceptance in preview-2:

- RocketNet packets larger than 1452 bytes are dropped. Preview-1 SDK senders
  already stayed within 1452 bytes, but custom peers relying on its wider
  receive acceptance must conform to the narrower ceiling. See
  [transport constants](../standard/TRANSPORT.md#t3) and
  [receive processing](../standard/TRANSPORT.md#t10).
- Tokens with zero or negative timeouts are refused. Clients ignore payloads
  and fragments received before the first keep-alive establishes the connection.
  Early unreliable data reordered ahead of that keep-alive can be lost;
  reliable data is resent after connection. See the
  [client handshake](../standard/TRANSPORT.md#t11).
- Servers refuse tokens created more than five seconds ahead of their clock,
  regardless of token lifetime. See the
  [server handshake](../standard/TRANSPORT.md#t12).

## Other changes since preview-1

- Portable and Windows RIO server receive paths use bounded worker handoffs;
  portable server sends also use a worker. Receive-bank capacity follows the
  configured client count. An extra server-to-client update from the background
  handoff is expected. Client-to-server traffic also uses a bounded handoff;
  there is no fixed public update-count guarantee in either direction.
  This does not change the application's frame order.
  These changes do not establish the frame-time target.
- The client I/O idle wait is a guard that can be interrupted by work, rather
  than a fixed send cadence.
- Server pre-authentication processing checks packet shape and cheap rejection
  conditions before expensive work; token claims follow signature verification.
  Tokens created more than five seconds ahead of the server clock are refused.
  Client activity and RTT decisions
  rely on the authenticated challenge rather than a clear-salt cookie.
- Linux builds can omit XDP with `ROCKETNET_XDP=OFF`; missing XDP build
  prerequisites also leave it unavailable. Selection reports
  `ROCKETNET_STATUS_TRANSPORT_NOT_BUILT`. XDP fill-ring capacity is doubled
  relative to the frame pool.
- Strict compiler warnings are enabled by default. The split-load harness now
  acknowledges connection establishment before starting its shared traffic
  window and uses one deadline for the start exchange.

See [BUILDING.md](../BUILDING.md), [TRANSPORTS.md](../TRANSPORTS.md) and
[USAGE.md](../USAGE.md) for the supported configuration and API contracts.

## Known limits and release qualification

- The API is provisional and can change before the final release. Rebuild
  against matching headers and libraries when moving from preview-1.
- Server API calls still exceed the 100 microsecond frame budget at larger
  client counts. Worker handoffs are not proof of a performance pass.
- Large server configurations need substantial memory. Query required memory
  before create and leave room for transport, operating-system and application
  resources. The 1000-client example soak can spike in memory during shutdown
  and run out of memory on Linux. The secrets-only destroy change is deferred
  to preview-3; completion checks do not eliminate this shutdown cost.
  Final-head platform sizing and lifecycle checks remain release gates.
- Windows server LAN-load qualification is pending replacement test hardware
  and a fresh run. Earlier loss measurements on the 1 GbE test NIC do not
  qualify the updated software or the replacement NIC.
- The current AF_XDP test environment uses copy mode. This is not evidence for
  zero-copy operation or line-rate throughput on another host.
- Final-head platform, sanitizer, soak and model results have not yet been
  recorded for preview-2. Earlier passes and failures retain their original
  commit and scope; none is a blanket preview-2 pass.

The [testing tables](../TESTING.md) and [roadmap](../ROADMAP.md) retain work
still owed. Fixes under review and future merges are not included in this
pinned source summary.

## Deferred to preview-3

The uniform client buffer handoff, secrets-only destroy, and macOS CI
restructuring are outside preview-2. The existing worker handoffs described
above do not include that later uniform-client change. None of these deferred
changes is a preview-2 fix or qualification result.
