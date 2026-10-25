# RocketNet v1.0.0-preview-3

Changes compared with the `v1.0.0-preview-2` tag. The release commit and the
native Linux, macOS and Windows unit and functional results are recorded in
[the qualification index](QUALIFICATION-v1.0.0-preview-3.md) and in
[TESTING.md](../TESTING.md). This preview replaces the earlier, never
published preview-3 draft; what that draft described and is still true is
folded in below.

RocketNet provides encrypted client/server UDP connections and eight message
delivery modes through a C API. Your application supplies message bytes and
drives the same five-step frame loop: update, receive, simulate, queue
messages, then send packets.

## Migrating from preview-2

The API is still provisional, and there is no compatibility layer before
v1.0. Preview-2 code needs the changes below. Deploy the client and the
server from the same release.

### One message size field, 1 MiB by default

`max_payload_bytes` is removed. `max_message_bytes` covers every reliability
type, and its default is now 1 MiB (it was 262,144 bytes). `packets_in_flight`
defaults to 1024 (it was 256) and must be at least
`reliable_messages_in_flight`; create refuses a smaller value. See
[USAGE.md](../USAGE.md).

### Take every message before the next update

If a delivered message is still in the receive queue at the next update, that
connection ends. The client state is `ROCKETNET_CLIENT_STATE_MESSAGE_NOT_RECEIVED`
and the server's disconnect reason is
`ROCKETNET_DISCONNECT_REASON_MESSAGE_NOT_RECEIVED`; debug builds assert. Call
receive until it returns NULL on every update. A missed drain is a
disconnect, not a silent loss.

### Destroy called early

An early destroy still asserts, but it no longer just returns. It requests
shutdown and steps it a bounded number of times without sleeping, then frees
the object. If a worker has still not exited, destroy logs an error and keeps
the object, which stays valid. In that case, poll
`rocketnet_client_disconnected()` or `rocketnet_server_stopped()` and call
destroy again.

### New calls, statuses and settings

- `rocketnet_client_can_send_message()` and
  `rocketnet_server_can_send_message()` say whether a message would fit now.
  Running out of room is still fatal.
- `rocketnet_client_light_config()` and `rocketnet_server_light_config()` set a
  light profile for 1000 players at 10 Hz.
- A connect token naming port 0 fails connect with
  `ROCKETNET_STATUS_CONNECT_TOKEN_PORT_ZERO`, and the token makers refuse a
  port-zero address.
- `required_memory` now reports the peak over the whole lifecycle.
- The configuration is read once, at create. The link simulator applies to a
  live connection.
- A build can set `ROCKETNET_MAX_CLIENTS` from 512 to 32,768.

### Portable servers in production builds

Production builds (`-DROCKETNET_DEV=OFF`) now allow the portable server
transport. Automatic selection keeps native transports first and tries
portable last: `xdp,gso,io_uring,portable` on Linux, `msgx,portable` on macOS
and `iocp,rio,portable` on Windows. If you relied on production create or
`rocketnet_server_required_memory` refusing a portable pin, that refusal is
gone; pin a native transport by name to require it. Production builds still
refuse the development keys, development tokens and the test packet filter.

## Wire compatibility

The packet encodings, the connect-token encoding and the protocol version are
unchanged, and a preview-2 build is not refused. The packing and acceptance
rules below changed. Mixed preview-2 and preview-3 clients and servers were
not tested, so deploy both from the same release. Each wire-affecting change:

- **Small unreliable messages share packets.** Unreliable records are packed
  directly into transport frames. See
  [m2 Payload forms and sender size rules](../standard/MESSAGES.md#m2) and the
  [payload packet](../standard/TRANSPORT.md#t8.6).
- **Acknowledgement after acceptance.** A reliable payload is acknowledged only
  after its messages are accepted, so receive pressure delays the
  acknowledgement and no data is lost. See
  [t15 Acknowledgement](../standard/TRANSPORT.md#t15) and
  [m9.1 Acknowledgement mapping](../standard/MESSAGES.md#m9.1).
- **The empty ack block** is recognised on its wire fields at any highest sent
  sequence. See [t15.1 Acknowledgement block](../standard/TRANSPORT.md#t15.1).
- **Receipt loss after the deadline.** An unreliable-with-receipt message is
  reported lost only after its fixed receipt deadline. See
  [m2 Payload forms and sender size rules](../standard/MESSAGES.md#m2).
- **One message size limit.** The receiver holds single and unreliable records
  to `max_message_bytes`, and both ends default it to 1 MiB; ends whose
  compared configuration differs still end the connection with
  `CONFIG_MISMATCH`. See [m4 Receiver checks](../standard/MESSAGES.md#m4) and
  [t2.3 Configuration surface](../standard/TRANSPORT.md#t2.3).
- **Configuration resolved once.** Each endpoint resolves its configuration
  completely at create. See
  [t2.3 Configuration surface](../standard/TRANSPORT.md#t2.3).
- **Packets in flight.** `packets_in_flight` below
  `reliable_messages_in_flight` is refused, and every send takes a fresh
  sequence. See [m9 Acknowledgement mapping, resend and send budget](../standard/MESSAGES.md#m9).
- **A message not received ends the connection**, counted as
  `MESSAGE_NOT_RECEIVED`. See
  [m1.6 Receiving every delivered message](../standard/MESSAGES.md#m1.6) and
  [t18 Error handling and counters](../standard/TRANSPORT.md#t18).
- **Token creation skew.** A server accepts connect tokens created up to 30
  seconds ahead of its clock, up from 5 seconds. A preview-2 server still
  refuses tokens more than 5 seconds ahead. See
  [t12 Handshake: server](../standard/TRANSPORT.md#t12) and its signed vector
  `t12.token-created-in-future`.
- **Token claims.** A claim is released when its half-open entry times out,
  when processing the request refuses it without keeping a mapping, and once a
  response binds the token. An observer who copies a client's first request
  can hold its token only until that half-open entry ends: the shorter of the
  handshake deadline (5 s by default) and the half-open idle timeout (10 s).
  The request refusal rungs are pinned in order, and a full claim table is
  counted as `CONNECTION_REQUEST_CLAIM_TABLE_FULL`. See
  [t12 Handshake: server](../standard/TRANSPORT.md#t12),
  [t17 Timing and defaults](../standard/TRANSPORT.md#t17) and
  [t18 Error handling and counters](../standard/TRANSPORT.md#t18).
- **Kernel replay window.** The AF_XDP kernel replay window is now the
  userspace window (4096, up from 16), and the embedded XDP program refuses a
  mismatch with the SDK's limits. See
  [t14 Replay protection](../standard/TRANSPORT.md#t14).
- **Connect over both families.** A token that names both IPv4 and IPv6 tries
  both and keeps whichever answers first. See
  [t11 Handshake: client](../standard/TRANSPORT.md#t11).
- **Port zero.** A connect token naming port 0 is refused at connect. See
  [t6 Connect token](../standard/TRANSPORT.md#t6).

## Other changes since preview-2

- **Reliability.** The reliable timeout is checked on every update and runs
  from each message's own first send, so waiting for send budget no longer
  counts. Sequenced blocks and small records follow one in-turn rule.
- **Work on the game thread.** Every client transport uses one buffer
  handoff: a worker sends and receives, and the game thread never waits for
  it. The server statistics worker is deleted; statistics are kept as acks
  arrive and are current on every update. The packet builder and block
  completion are linear, not quadratic. Each received packet is looked up once
  and decrypted in place.
- **Shutdown and memory.** Disconnecting every client costs a bounded amount
  per slot, and the bursts go out over the following updates. Slot reuse and
  destroy no longer clear memory in bulk. Destroy wipes only key material, so
  shutdown no longer spikes memory. The game's allocator covers the library's
  own memory over the whole lifecycle; the memory the operating system owns is
  listed under the known issues.
- **Native clients.** The client ranks gso first on Linux, msgx on macOS and
  iocp on Windows, with portable last on every OS.
- **Linux.** io_uring uses kernel send segmentation and receive coalescing.
  AF_XDP serves IPv6 end to end and builds against stock Ubuntu 22.04
  libraries. A hard-killed client or an ICMP error stalls no Linux server
  transport.
- **macOS and Windows.** The msgx server sizes its send banks to the client
  count. iocp uses one completion worker. The Windows stop bursts, the
  don't-fragment setting and the receive drop counts are fixed.
- **Secrets.** Connect-token user data and staged configuration secrets are
  wiped.
- **Backend examples.** The Go and C# backend examples under
  [examples/backend](../examples/backend) generate their own signing keypairs
  with `--keygen`, accept server registration with `PUT /servers/{id}`, and
  answer `GET /match/{clientID}` with a token for a registered server, or
  HTTP 503 when none is registered. Registration authentication and client
  restrictions are left to the application.
- **Documentation.** An API reference is generated from the header, with
  getting-started, transport, platform and keys guides.

See [BUILDING.md](../BUILDING.md), [TRANSPORTS.md](../TRANSPORTS.md) and
[USAGE.md](../USAGE.md) for the supported configuration and API contracts.

## Known issues

Everything not fixed in this preview is listed here, with what it means for a
game.

### Migration

- **Mixed versions.** A preview-2 build is not refused, but mixed preview-2
  and preview-3 clients and servers were not tested. Deploy both from the
  same release.
- **Destroy while a worker still runs.** With assertions disabled, if a worker
  has not exited, destroy keeps the object, and its memory stays allocated
  until the game polls for completion and calls destroy again. This is the
  intended behaviour.

### Platforms and transports

- **Windows server under LAN load.** The Windows iocp and rio servers start
  on the 10 GbE test NIC, but no client reached them because the test host has
  no firewall rule for the test port. A Windows server under LAN load is not
  qualified.
- **Windows clients against Linux servers.** 1000 Windows clients against an
  AF_XDP server were not retested, and the gso server ladder with Windows
  clients is incomplete. 1000 Linux clients against AF_XDP passed over the LAN.
- **Transport cells not yet repeated.** The Linux AF_XDP cell and the Windows
  iocp client cell have not had their 50-run repeat.
- **macOS packet delivery under a concurrent flood** has no root cause yet.
  The full macOS functional suite passed 5 of 5 runs, and the macOS cells
  passed 50 runs.
- **io_uring locked memory.** The library log does not yet name
  `RLIMIT_MEMLOCK` when create fails. Size the limit for every io_uring server
  under one account.
- **AF_XDP in copy mode only.** The test NIC's driver refuses zero copy, so
  zero copy and line rate are not measured.

### Performance and memory

- **Performance is judged by parity.** The 100 microsecond per-call server
  update target is replaced by parity with the previous networking stack at
  equal load in the integrating application, which can run the network step
  beside the next frame. The old per-call check still fails at higher client
  counts. Parity at the release commit is not yet measured. Server encryption
  still runs on the game thread.
- **Memory at 1000 slots.** `required_memory` is about 20.9 GB at 1000 slots
  for the default profile and about 3.2 GB for the light profile on Linux
  (preview-2: 13.3 GB). Server memory is spent to save CPU and bandwidth, and
  it is taken up front. The plan leaves out what the operating system owns:
  each worker's stack (1 MiB by default on Windows), XDP UMEM of 256 MiB per
  queue, the AF_XDP and io_uring rings, and kernel socket buffers. Provision
  host memory for the plan plus these.
- **Destroy time.** Destroy has no promised time bound.
- **Latency and unreliable loss under load.** Background handoffs can add one
  or more updates of latency under load. At 1000 clients over the LAN,
  io_uring lost 0.46% of unreliable messages, msgx lost 25,662, AF_XDP lost
  1,457 and gso lost none; reliable delivery was exact. Unreliable loss is
  allowed by design, and its causes are measured only in part.

### Testing and qualification

- **Linux AF_XDP soak loss.** The AF_XDP soak cell passed with every payload
  verified, but lost about twice the configured share of server-to-client
  payloads. About half is the configured impairment and about half, by
  estimate, is overflow of the test client's own socket receive buffer; the
  AF_XDP rings, the virtual link and the kernel's packet queues dropped
  nothing. The client socket buffer size under that load is still open.
- **Sanitizer findings.** Under Linux AddressSanitizer, a portable server with
  a portable client stalled once in a 1000-cycle connect test on a heavily
  loaded host; the cause is untraced and it did not recur in 40 later runs.
  The loopback live message matrix ran past its limit under AddressSanitizer.
  The macOS AddressSanitizer and UndefinedBehaviorSanitizer nightly reported
  four findings whose fixes are not yet confirmed.
- **Platform rows not yet passes.** macOS fuzzing, the Windows toolchain,
  examples and frame-budget runs, and the instruction-count check are not
  recorded as passes on the preview-3 inputs. The Windows unit suite can fail
  intermittently when the test host runs out of commit memory: three
  full-capacity server cases commit 37 to 50 GB each.
- **Proofs and models.** 16 TLA+ cases end at their bounded time limit
  (preview-2 had 18). The CBMC proofs of the full-size packet readers, the
  ack-block reader, and the sequence buffers and replay window are not done.
- **Big-endian and timing tests.** One full big-endian system run is owed.
  Some functional tests still assert wall-clock timing.

### Still open from preview-1

- **Security.** A replayed connection response refreshes a slot's receive
  time, and a forged cookie can rewind a client's handshake (bounded). The
  library has been audited area by area, but a security review of the whole
  library is not yet done.
- **Memory.** Client connect and server start still allocate, but only through
  the game's allocator and within the plan.
- **Tests.** On macOS, a portable client connecting over IPv4 to a dual-stack
  server failed once, with no proven cause. Some functional tests measure
  operating-system behaviour.
- **API still provisional.** Names and structure in `rocketnet.h` may still
  change. Always build against a matching header and library.

The [qualification index](QUALIFICATION-v1.0.0-preview-3.md), the
[testing tables](../TESTING.md) and the [roadmap](../ROADMAP.md) record the
tested heads and the remaining work.
