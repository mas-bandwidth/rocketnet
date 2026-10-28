# RocketNet v1.0.0-preview-3

Changes compared with the `v1.0.0-preview-2` tag. The release commit and the
native Linux, macOS and Windows unit and functional results are recorded in
[the qualification index](QUALIFICATION-v1.0.0-preview-3.md) and, once the tag
is cut, in the
[GitHub release](https://github.com/mas-bandwidth/rocketnet/releases/tag/v1.0.0-preview-3).
See [TESTING.md](../TESTING.md) for the historical tables.

RocketNet provides encrypted client/server UDP connections and eight message
delivery modes through a C API. Your application supplies message bytes and
drives the same five-step frame loop: update, receive, simulate, queue messages,
then send packets.

## Migrating from preview-2

The API is still provisional, and there is no compatibility layer before
v1.0. Preview-2 code needs the changes below. Deploy the client and the server
from the same release: a preview-2 build is not refused, but mixed preview-2
and preview-3 clients and servers were not tested.

### One message size field, 1 MiB by default

`max_payload_bytes` is removed. `max_message_bytes` covers every reliability
type, and its default is now 1 MiB (1,048,576 bytes; it was 262,144).
`packets_in_flight` defaults to 1024 (it was 256) and create refuses a value
below `reliable_messages_in_flight`. See [USAGE.md](../USAGE.md) for the
ranges.

### Take every message before the next update

If a message is still in the receive queue at the next update, that
connection ends. The client state is `ROCKETNET_CLIENT_STATE_MESSAGE_NOT_RECEIVED`
and the server's disconnect reason is
`ROCKETNET_DISCONNECT_REASON_MESSAGE_NOT_RECEIVED`; debug builds assert. Call
receive until it returns NULL on every update. A missed drain is a disconnect,
not a silent loss.

### Destroy called early

An early destroy still asserts, but it no longer just returns. It requests
shutdown and steps it a bounded number of times without sleeping, then frees
the object. If a worker has still not exited, destroy logs an error and keeps
the object, which stays valid. Poll `rocketnet_client_disconnected()` or
`rocketnet_server_stopped()` and call destroy again.

### New calls, statuses and settings

- `rocketnet_client_can_send_message()` and
  `rocketnet_server_can_send_message()` say whether a message would fit now.
  Running out of room is still fatal.
- `rocketnet_client_light_config()` and `rocketnet_server_light_config()` set
  a light profile for 1000 clients at a low send rate.
- A connect token naming port 0 fails connect with
  `ROCKETNET_STATUS_CONNECT_TOKEN_PORT_ZERO`.
- `required_memory` reports the peak over the whole lifecycle.
- Each endpoint's configuration is read once, at create, and the network
  simulator applies to a live connection.
- A build can set the client limit with `-DROCKETNET_MAX_CLIENTS=N`, from 512
  to 32,768 (default 1000).
- `rocketnet_server_client_user_data()` copies the 256 bytes of connect-token
  user data for a connected client into the caller's buffer.
- The server configuration's `handshakes_per_update` caps the connection
  requests a server processes in one update (1 to 1024, default 16). A request
  past the cap is dropped unanswered and counted in
  `handshakes_dropped_over_budget`; the client sends it again, so a mass join
  or a request flood spreads across updates. Create refuses a value outside
  the range, so a configuration not filled by the default config call must
  set it.

### Portable servers in production builds

Production builds (`-DROCKETNET_DEV=OFF`) now allow the portable server
transport. Automatic selection keeps native transports first and tries
portable last: `xdp,gso,io_uring,portable` on Linux, `msgx,portable` on macOS
and `iocp,rio,portable` on Windows. An explicit `"portable"` server pin is
accepted in either build. Production builds still refuse the development keys,
development tokens and the test packet filter. If you relied on a production
create refusing a portable pin, pin a native transport by name to require it.
The portable server carries no performance guarantee.

## Wire compatibility

The packet encodings, the connect-token encoding and the protocol version are
unchanged. The packing, acknowledgement and acceptance rules changed; each
wire-affecting change is listed with its section of the standard:

- **Token creation skew.** A server accepts connect tokens created up to 30
  seconds ahead of its clock, up from 5 seconds. Expiry order is unchanged and
  there is still no maximum token lifetime. A preview-2 server still refuses
  tokens more than 5 seconds ahead.
  [TRANSPORT.md t12](../standard/TRANSPORT.md#t12)
- **Request refusal order and token claims.** The order in which a server
  refuses a connection request is numbered in the standard, and a request
  claim on a token is released when its half-open mapping is removed, so a
  copied first request can hold a token only for the shorter of the handshake
  deadline and the half-open idle deadline. A full request-claim table is
  counted separately.
  [TRANSPORT.md t12](../standard/TRANSPORT.md#t12),
  [t18](../standard/TRANSPORT.md#t18), [t19](../standard/TRANSPORT.md#t19)
- **Connection response padding.** A connection response that opens but whose
  padding is not all zero is refused with its own counter and does not count
  as an integrity failure.
  [TRANSPORT.md t10](../standard/TRANSPORT.md#t10),
  [t18](../standard/TRANSPORT.md#t18)
- **Both address families.** A client given a token that names both IPv4 and
  IPv6 addresses tries both and keeps whichever answers first.
  [TRANSPORT.md t11](../standard/TRANSPORT.md#t11)
- **Kernel replay window.** The AF_XDP kernel program's replay window is the
  userspace replay window.
  [TRANSPORT.md t14](../standard/TRANSPORT.md#t14)
- **Acknowledgement after acceptance.** A reliable payload is acknowledged
  only after its messages are accepted, so receive pressure delays the
  acknowledgement and no data is lost. The empty acknowledgement block is
  recognised on its wire fields at any highest sent sequence.
  [TRANSPORT.md t15.1](../standard/TRANSPORT.md#t15),
  t15.3, t15.4 and t15.6; [MESSAGES.md m1.4](../standard/MESSAGES.md#m1)
- **Unreliable packing.** Small unreliable messages are packed into shared
  transport packets.
  [MESSAGES.md m2](../standard/MESSAGES.md#m2)
- **One message size limit.** The receiver holds single and unreliable records
  to `max_message_bytes`, and the default message limits are 1 MiB.
  [MESSAGES.md m4](../standard/MESSAGES.md#m4), m5.4, m11.2 and m11.4;
  [TRANSPORT.md t2.3](../standard/TRANSPORT.md#t2)
- **Undrained messages.** An update with a message not received ends the
  connection.
  [MESSAGES.md m1.5](../standard/MESSAGES.md#m1),
  [TRANSPORT.md t18](../standard/TRANSPORT.md#t18)
- **Send window.** `packets_in_flight` below `reliable_messages_in_flight` is
  refused.
  [MESSAGES.md m9.1](../standard/MESSAGES.md#m9)
- **Ack receipts.** An ack receipt reports loss only after its fixed receipt
  deadline.
  [MESSAGES.md m12](../standard/MESSAGES.md#m12)

Editorial changes in the same period (terminology and normative wording in
t7, t13, t15, t17, t19, m9, m11 and m13) change no rule.

## Other changes since preview-2

- **Reliability.** The reliable timeout is checked on every update and runs
  from each message's own first send, so waiting for send budget no longer
  counts against it. Sequenced blocks and small records follow one in-turn
  rule.
- **Work on the game thread.** Every client transport uses one buffer handoff:
  a worker sends and receives, and the game thread never waits for it. The
  server statistics worker is removed and statistics are kept current as acks
  arrive. The packet builder and block completion are linear, and each
  received packet is looked up once and decrypted in place.
- **Shutdown and memory.** Disconnecting every client costs a bounded amount
  per slot, with the disconnect bursts spread over the following updates. Slot
  reuse and destroy no longer clear memory in bulk; destroy wipes only key
  material. The application's allocator covers the library's own memory over
  the whole lifecycle; the memory the operating system owns is listed in the
  [public header](../sdk/rocketnet.h).
- **Native clients.** Clients rank `gso` first on Linux, `msgx` on macOS and
  `iocp` on Windows, with `portable` last everywhere.
- **Linux.** io_uring uses kernel send segmentation and receive coalescing.
  AF_XDP serves IPv6 end to end and builds and runs against stock Ubuntu
  22.04 libraries. On Ubuntu 22.04, whose liburing has no multishot receive,
  io_uring is reported as not built rather than offered and refused. A
  hard-killed client or an ICMP error stalls no Linux server transport.
- **macOS and Windows.** The `msgx` server sizes its send banks to the client
  count. `iocp` uses one completion worker. The Windows stop bursts, the
  don't-fragment setting and the receive drop counts are fixed.
- **Security hardening.** The embedded XDP program reads the SDK's limits and
  refuses a mismatch. Connect-token user data and staged configuration
  secrets are wiped. A forged challenge cookie no longer displaces the
  client's kept cookie, and handshake work per update is capped.
- **Client family race.** When a client tries both address families, the
  losing socket is closed on the thread that reads it and freed on the
  caller's thread.
- **Backend examples.** The Go and C# examples under
  [examples/backend](../examples/backend) generate their own signing keypairs
  with `--keygen`, accept server registration with `PUT /servers/{id}`, and
  answer `GET /match/{clientID}` with a token for a registered server, or HTTP
  503 when none is registered. Registration authentication and client
  restrictions are left to the application.
- **Load diagnostics.** The development-only `load_split` harness captures
  per-connection delivery state once per second and at disconnect.
- **Documentation.** An API reference generated from the header, and
  getting-started, transport, platform and keys guides.
- **Source tree.** The whole tree is formatted with clang-format, checked in
  CI; later layout-only passes changed no compiled output.

See [BUILDING.md](../BUILDING.md), [TRANSPORTS.md](../TRANSPORTS.md) and
[USAGE.md](../USAGE.md) for the supported configuration and API contracts.

## Known limits and release qualification

- The API is provisional and can change before the final release. Always
  build against a matching header and library.
- Mixed preview-2 and preview-3 clients and servers were not tested.
- Performance is judged as parity with the integrating application's previous
  network stack at equal load, not as a per-call budget. The old
  100-microsecond per-call check still fails at larger client counts, and
  server encryption still runs on the game thread.
- A default 1,000-client server requires about 20.9 GB, and the light profile
  about 3.2 GB, on Linux ([USAGE.md](../USAGE.md)). Memory is spent to save CPU
  and bandwidth and is taken up front. The plan leaves out what the operating
  system owns: worker stacks, XDP UMEM, the AF_XDP and io_uring rings and
  kernel socket buffers. Provision host memory for both. Destroy has no
  promised time bound.
- Background handoffs can add one or more updates of latency under load.
  Unreliable loss is allowed by design; reliable delivery was exact in every
  recorded 1,000-client LAN cell.
- A Windows server under LAN load is not qualified: the iocp and rio LAN cells
  were not made. 1,000 Windows clients against an AF_XDP server were not
  retested, and the gso server ladder with Windows clients is incomplete.
- AF_XDP is recorded in copy mode only; zero copy and line rate are not
  measured.
- The Linux AF_XDP impaired soak cell passed but lost about twice the
  configured share of server-to-client payloads; the record attributes the
  excess to the test client's socket receive buffer being clamped by the
  guest kernel's `net.core.rmem_max`
  ([record](runs/linux-soak-impaired-xdp-loss.md)). On Linux, set
  `net.core.rmem_max` to at least the receive buffer the client requests.
- The io_uring log does not yet name `RLIMIT_MEMLOCK` when create fails; size
  that limit for every io_uring server under one account.
- macOS packet delivery under a concurrent flood has no root cause yet.
- Sixteen TLA+ cases end at their time bound and are not reported as passes;
  some CBMC proofs and the full big-endian system run are owed. Some
  functional tests still assert wall-clock timing.
- A replayed connection response refreshes a slot's receive time. A security
  review of the whole library is not yet done.

The [qualification index](QUALIFICATION-v1.0.0-preview-3.md) records the
tested head and every gate, including those still owed at tag time. The
[roadmap](../ROADMAP.md) carries the remaining work.
