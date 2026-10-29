# RocketNet v1.0.0-preview-2

Changes compared with the `v1.0.0-preview-1` tag. This corrected preview-2
recut includes the move of root-level test sources into `tests/`, the Windows
resource-path repair, Darwin functional-suite flood isolation and the native
byte-order io_uring unit fixture repair. It also repairs the ordinary server
send boundary so GSO and io_uring retry deferred work without requiring a
new message. It supersedes the earlier preview-2
publications at `c1a63ac9` and `2f88961a` under the release owner's explicit
recut authorization. The final commit and native Linux, macOS and Windows
unit/functional results are recorded in the [GitHub release](https://github.com/mas-bandwidth/rocketnet/releases/tag/v1.0.0-preview-2).
See [the qualification index](QUALIFICATION-v1.0.0-preview-2.md) and
[TESTING.md](TESTING.md) for the recorded evidence and its original scope.

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

Destroy asserts the completion precondition when assertions are enabled, then
synchronously joins and frees resources. It has no promised wall-clock bound.
A stuck worker leaves completion false. An endpoint that has never started or
connected is already complete.

Calling destroy too early is API misuse in every build: it logs one error
naming the skipped completion function, records the misuse, requests shutdown
itself and updates a bounded number of times without sleeping, then
synchronously joins, wipes and frees. A worker that has not exited by then is
logged a second time and its object is kept, not freed under it. The
debug assert still stops on the misuse. Preview-1 callers must still adopt the
completion checks above; the leak is gone, but the error and misuse count say
the contract was broken.

Wait for server completion before restarting. A client retry from a terminal
failure can return `ROCKETNET_STATUS_CLIENT_NOT_DISCONNECTED` while the old
attempt shuts down; poll completion and retry with the token. See
[USAGE.md](USAGE.md) and the [public header](../sdk/rocketnet.h).

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
back. Follow [Before production](AGENTS.md#before-production).

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
  configured client count. Moving bulk send and receive work into the background
  trades added latency under load for scale; unnecessary extra delay remains
  a regression. Client-to-server traffic also uses a bounded handoff;
  there is no fixed public update-count guarantee in either direction.
  This does not change the application's frame order.
  These changes do not establish the frame-time target.
- The legacy client receive handoff processes available batches in arrival
  order up to the existing per-update packet budget, removing a persistent
  extra-frame backlog during quiet operation. Batches beyond the budget stay
  queued for later updates.
- The client I/O idle wait is a guard that can be interrupted by work, rather
  than a fixed send cadence.
- Later server send calls retry retained GSO batches and pending io_uring
  work even when no new message was queued. The native flush remains
  nonblocking; background scheduling and budget delays under load remain
  part of the scalability tradeoff. Functional send-error checks now wait
  for final counters through that ordinary boundary, keeping the existing
  assertions and deadlines.
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

See [BUILDING.md](BUILDING.md), [TRANSPORTS.md](TRANSPORTS.md) and
[USAGE.md](USAGE.md) for the supported configuration and API contracts.

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
  This sizing risk is separate from the interrupted AF_XDP soak below; no cause
  is assigned to that interruption.
- Windows server LAN-load qualification is deferred until replacement test
  hardware is available and a fresh run. Earlier loss measurements on the
  1 GbE test NIC do not qualify the updated software or the replacement NIC.
- The current AF_XDP test environment uses copy mode. This is not evidence for
  zero-copy operation or line-rate throughput on another host. At `415e796d`, whose
  SDK and load-harness inputs are identical at runtime source `a76d641a`, 60-second
  Linux AF_XDP copy-mode correctness rungs passed at 1, 10, 100, 500 and 1000
  clients, with XDP detached after each rung. Their server-update p99 upper
  bounds were 39, 58, 497, 1003 and 2282 microseconds respectively, so the
  timing verdict failed from 100 clients onward.
- The same input-equivalent ladder passed non-timing correctness at every rung
  for portable, GSO and io_uring Linux servers with a Linux portable client.
  At 1000 clients their server-update p99 upper bounds were 935, 1146 and 1846
  microseconds respectively. On exact `ba09993b`, a Windows portable client
  also passed every portable-server rung, including 1000 clients and 60,000
  deliveries each way; its server-update p99 upper bound there was 1223
  microseconds. The AF_XDP 1000-client Windows-client rung failed after eight
  server connections hit reliable timeouts; lower rungs passed non-timing
  correctness. These ladders are not throughput qualification.
- The release TLA inputs are identical to the qualified `b4c0a109` tree.
  All 287 required cases across 44 groups passed with pinned tool checksums
  and unchanged input hashes. Eighteen separate bench cases remain explicit
  bounded-timeout debt and are not reported as passes.
- The exact `ba09993b` channel sweep passed all 60,000 runs. Fresh release-head
  Linux examples and the frame-budget check passed; the latter reported a
  16.49-microsecond p99 median against the 100-microsecond gate. The fresh
  instruction comparison could not be made because the available toolchain
  differed from its pinned baseline, so the older comparable receipt retains
  its original scope.
- The published manual qualification retains the tested head on every receipt.
  Some platform, sanitizer, soak, fuzz and LAN rows remain preview-3 work. The
  full unit and functional leak suite passed at `415e796d`; its SDK, build
  configuration and sanitizer support inputs are unchanged at the release
  source, while later functional-oracle changes retain their own validation.
  Input-equivalent threaded TSan and sanitized-soak jobs passed at `c9db35ff`.
  The c9 full soak completed its portable, GSO, GSO-plain and io_uring
  1000-client cells with non-timing PASS, then was killed during the AF_XDP
  cell; the full soak is incomplete, with no OOM cause claimed. The macOS
  flood-isolation repair and big-endian unit-fixture repair are included in
  this recut. Separate big-endian full-system emulation completion failures
  remain unclassified; completion deadlines are unchanged. Mac fuzz, Windows
  manual Debug/toolchain/examples/soak/frame-budget coverage, and the remaining macOS and Windows LAN rows are deferred. io_uring suite-concurrency and runner memlock provisioning work is also deferred. Earlier passes and
  failures retain their original commit and scope.

The [testing tables](TESTING.md), [qualification index](QUALIFICATION-v1.0.0-preview-2.md), and [roadmap](ROADMAP.md) record the tested heads and preview-3 work.

## Deferred to preview-3

The uniform client buffer handoff and secrets-only destroy are deferred to
preview-3. Darwin functional-suite flood isolation is included in this recut;
its existing workload and assertions are unchanged. The existing
worker handoffs described above do not include that later uniform-client
change. None of these deferred changes is a preview-2 fix or qualification
result.
