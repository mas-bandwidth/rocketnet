# Transport implementation reference

This page is for SDK contributors implementing or reviewing a transport.
Games use the public C API and choose carriers through [Transports](TRANSPORTS.md).
The internal interfaces here are not an application ABI. Platform setup is in
the [deployment guide](DEPLOYING.md) and its linked host guides.

## The registry and the backend interface

The public transport API in `rocketnet.h` is independent of the backend.
Native transports implement the internal transport interface in
`sdk/rocketnet_driver.h`; they do not add binding or wire-format rules. For
Linux transport selection, pinning, host requirements (`xdp`, `gso`,
`io_uring`, `portable`) and host verification, see
[docs/LINUX.md](LINUX.md).

`rocketnet_drivers.inc` is the built-in registry. Each row supplies a name,
bound-port discovery property, backend record, platform mask, and server/client
preference ranks. Lower nonzero ranks come first in `auto`; zero means that
endpoint is unsupported. IDs, names, diagnostics and platform availability are
derived from these rows. Existing IDs and preference orders are preserved.

The backend record points to immutable server and/or client operations. The
transport retains the selected operations and opaque instance for its lifetime.
Factories, resources and platform decisions stay in the transport layer. The
transport branches on generic capabilities, never a specific backend identity.

The `transport` config value is an ordered candidate list
(`rocketnet_transport_ladder_parse`, `sdk/rocketnet_config.cpp`).
A known name absent from this build's availability list for the endpoint on
this platform (`gso` or `io_uring` on macOS, `io_uring` on a client or built without
liburing) is skipped with one info line. A list with at least
one name left after skipping is a strict pin: only those candidates are tried,
in order, and if every one refuses, the ladder is exhausted and connect or
start fails. A list whose every name is skipped falls back to the endpoint's
`auto` order with a warning: one
fleet-wide line such as `ROCKETNET_SERVER_TRANSPORT=gso` deploys unchanged to
mixed machines. A row that is compiled out but kept as a refusing rung (`xdp`
in a build without XDP) is not skipped: its pin refuses at start with
`ROCKETNET_STATUS_TRANSPORT_NOT_BUILT`. An unknown name, an empty element or
`auto` inside a list refuses the create.

A transport with a preference rank at or above `ROCKETNET_TRANSPORT_PIN_ONLY`
(1000) is pin-only: it stays in the availability list, so an explicit name
always selects it, but `auto` never chooses it. The loopback transport is the
one row pin-only on both endpoints. The pin-only check reads the
endpoint's own column, so a transport can rank in one endpoint's `auto` order
and be pin-only on the other. The loopback transport is in-process, carries datagrams through memory with no
socket and no system network call. It is a production transport selected only
by name; `auto` never chooses it. It keeps no library globals: its endpoints
meet in a hub the application creates with `rocketnet_loopback_hub_create`
and names in each config's `loopback_hub`, and create refuses a list naming
loopback with a `NULL` hub (`ROCKETNET_STATUS_LOOPBACK_HUB_MISSING`). Because
the hub is the only shared state, a server and its clients built into
separate shared objects, each with its own copy of the library from one
build, connect through one hub; every use checks the hub's magic, version
and layout id and refuses a hub from another build
(`ROCKETNET_STATUS_LOOPBACK_HUB_MISMATCH`). The hub holds one server and
`max_clients` clients (1 .. `ROCKETNET_MAX_CLIENTS`), and create allocates it
as one block through the caller's allocator: the endpoint records, each
client's `ROCKETNET_LOOPBACK_QUEUE_PACKETS` queue and the server's queue
sized for `max_clients`. Start and connect register an endpoint and allocate
nothing, and runtime sends never allocate or grow a queue. Lookups scan the
hub's records linearly under its mutex. `rocketnet_loopback_hub_destroy`
refuses while any endpoint is registered (`ROCKETNET_STATUS_LOOPBACK_HUB_IN_USE`),
keeping the hub; destroy it after the server and clients, before
`rocketnet_term`. The registration lifecycle is modelled in
`proofs/tla/LoopbackHub.tla` and the queue ring arithmetic in
`proofs/tla/LoopbackHubLayout.tla`.

The names `rocketnet_server_uring_available`, `rocketnet_server_uring_leaked_bytes`
and `rocketnet_server_uring_leaked_transports` are internal transport helpers
(`sdk/rocketnet_server_uring.h`); a game uses `rocketnet_server_create`,
`rocketnet_server_required_memory` and the other server entry points in
`sdk/rocketnet.h`.

## Client transports

A client transport fills `rocketnet_client_transport_io_operations_t`
(`sdk/rocketnet_driver.h`) and runs on the client I/O thread of
[docs/CLIENT-IO.md](CLIENT-IO.md): the I/O thread owns the socket, and the
game thread hands it rings of sealed packets through the N-buffer handoff.

The client's `auto` order ranks the native client first and `portable` last on
every OS; `portable` is the fallback:

| OS | client `auto` order (client rank) |
|---|---|
| Linux | `gso` (10), `portable` (30) |
| macOS | `msgx` (10), `portable` (30) |
| Windows | `iocp` (10), `portable` (30) |
| other | `portable` (30) |

`loopback` is pin-only on the client too. A native client whose create refuses
is logged once (`client transport gso refused. trying the next candidate`) and
the ladder falls to `portable`, logging the grant with the refusals it passed;
a backend allocation failure ends candidate selection with
`ROCKETNET_STATUS_OUT_OF_MEMORY`. These backend factories run during public
`rocketnet_client_connect`, after the token supplies the server address.
A pin is honoured exactly: a pinned client whose backend refuses makes connect
return `ROCKETNET_STATUS_TRANSPORT_LADDER_EXHAUSTED`, without adding another
candidate to the configured list. A token naming both families selects its
IPv6 server on a host with IPv6; when a candidate's IPv6 socket is refused,
at open or at connect (no route to the address is the common case), the same
candidate is opened again, bound IPv4, for the token's IPv4 server before the
ladder falls to the next candidate (`client transport portable refused the
IPv6 server ... trying the IPv4 server ...`), and a client granted that way
races nothing. A refusal for want of memory on either family ends the create
with `ROCKETNET_STATUS_OUT_OF_MEMORY` (TRANSPORT.md t11 rule 3,
`proofs/tla/RNClientLadder.tla`, `tests/test_unit_client_family_fallback.cpp`). `tests/test_unit_client_transport_ranking.cpp` pins the order, the
fallback and the pins. `msgx` and `portable` share one client table, and the pump is
the socket's, chosen at create from the transport name: `msgx` the Darwin
`sendmsg_x`/`recvmsg_x` batch pump, `portable` the per-packet loop. On Linux
the portable client batches with `sendmmsg`/`recvmmsg`. The msgx client, its
pins and its degrade path are in [docs/MACOS.md](MACOS.md), "The msgx
client".

On Windows the `iocp` client (`sdk/rocketnet_client_iocp_windows.cpp`,
the Windows `auto` choice; pin `ROCKETNET_CLIENT_TRANSPORT=iocp`) associates its one connected
socket with a completion port of its own, never the iocp server's, and posts
`ROCKETNET_CLIENT_IOCP_RECEIVES` (64) overlapped `WSARecvFrom` receives at
create into buffers allocated at create. The I/O thread's wait is
`GetQueuedCompletionStatusEx` with the timeout and its wake one
`PostQueuedCompletionStatus` with a wake key, so the portable client's
event wait is replaced by the port; receive_batch copies each completed
receive and re-posts it after the copy; sends are synchronous `WSASendTo`
(overlapped send is rn-win-client-overlapped-send). Destroy closes the
socket and dequeues until every posted receive has completed before it frees
(`proofs/tla/RNClientIocp.tla`, FreedOnlyWhenDrained); a drain that does not finish
within `ROCKETNET_CLIENT_IOCP_DRAIN_WAITS` dequeues of one second leaks the
transport rather than free memory the kernel may still write.

## Server capabilities and ownership

Two I/O shapes preserve the existing implementations:

- **Buffered:** the transport supplies its common control, payload and receive
  buffers. `send_batch` and `receive_batch` borrow that storage only for the call.
  This is the portable/msgx shape. The current server backend copies sends into
  bounded TX banks for its transmit worker and receives into bounded RX banks
  on its receive worker; the borrowed interface does not transfer the caller's
  buffers to either worker.
- **Staged:** the transport supplies frame storage and a complete receive batch.
  XDP, GSO, io_uring, IOCP and RIO use this shape, with their own pools and
  worker arrangements.
  The transport does not allocate, recycle or schedule those backend frames.

The staged callbacks are start, finish, abort, flush and receive. Start returns
a `ROCKETNET_FRAME_STRIDE` root; the wire starts at `ROCKETNET_FRAME_WIRE_OFFSET`.
The caller-visible payload alignment must remain valid. Receive returns at most
`rocketnet_server_receive_packets_per_update( max_clients )` datagrams, each within the receive
ceiling, with normalized source addresses. Its data remains valid until the
next receive or destruction, including while workers accept further packets.

The flags express the differences the transport needs:

| Flag | Meaning |
| --- | --- |
| `STAGED` | Use transport-owned frames and receive views instead of borrowed transport buffers. |
| `FLUSH_ON_UPDATE` | Flush staged control traffic at the end of update. Buffered control traffic completes by the end of update; full batches may flush during the update walk. |
| `FINISH_FINAL` | A finished frame cannot be withdrawn. Otherwise abort may withdraw it before flush. Unfinished reservations can always be aborted. |
| `BINDS_PUBLIC_ADDRESS` | The factory binds the advertised address rather than the transport's ordinary bind address. |
| `REFLUSHES` | Retry backend flush on `send_packets` even if no new transport commit was recorded. |

A flush means submission, not necessarily completion or reusable storage. IOCP
keeps operation buffers until completion retirement. The backend owns that
decision; it must never hand a still-owned frame to another producer.

An optional kernel replay capability protects session packets from replay on a
kernel dataplane. A transport with it implements `replay_forget`,
`replay_mark` and `replay_commit` together, all three or none; the transport
calls them on the main thread once per update, after packet processing, and
nothing else writes the transport's replay map. XDP is the only built-in backend
that reports it.

## Per-update send and receive budgets

The backend constants pair send staging with receive storage, checked by
static assertions and the configured-limit cases in `test_unit_max_clients.cpp`.
The table describes storage and processing units for a server with the default
`max_clients` of 1000; it is not a guaranteed packet rate or a bound on every
syscall or worker pass. The capacities that scale with clients are sized at
create from the server's configured `max_clients`; see
[Memory and capacity](MEMORY.md#maximum-clients).

The server process batch `rocketnet_server_receive_packets_per_update`
(`sdk/rocketnet_constants.h`, 16000 at 1000 clients) bounds packets exposed for
protocol processing. Datagrams discarded before that batch is returned are
bounded per receive call by the read budget below.

| Transport | Send budget | Receive capacity |
| --- | --- | --- |
| portable | 256–1000, max `rocketnet_server_send_batch_packets` (1000, `sdk/rocketnet_constants.h`) | 16000 per update (`rocketnet_server_receive_packets_per_update`) |
| msgx | portable batch, 256–1000 | 16000 per update |
| loopback | portable batch, 256–1000 | 2048 per client endpoint; server queue scales with hub capacity (16384 at 1000 clients), with 16000 processed per update (`sdk/rocketnet_driver_loopback.h`) |
| gso | 4096 in the current shared producer bank (`ROCKETNET_GSO_SEND_QUEUE_SIZE`) | 8192 per queue buffer (`ROCKETNET_GSO_RECV_QUEUE_SIZE`) |
| io_uring | 2048 shared send slots per phase, two phases; 10240 completion reaps per send call (`ROCKETNET_URING_SEND_QUEUE_SIZE`, `ROCKETNET_URING_SEND_PHASES`, `ROCKETNET_URING_SEND_BUDGET`) | 4096 receive buffers of 66 KiB per queue; 16000 in each shared receive bank (`ROCKETNET_URING_RECV_BUFFERS`, `rocketnet_server_uring_bank_packets`) |
| xdp | 4096 per queue (`ROCKETNET_XDP_SEND_QUEUE_SIZE`) | 16384 staging per queue (`ROCKETNET_XDP_RECV_QUEUE_SIZE`) |
| iocp | 2048 (`rocketnet_server_iocp_send_queue_size`) | 4096 queue (`rocketnet_server_iocp_receive_queue_size`) |
| rio | 2048 per phase (`rocketnet_server_rio_send_queue_size`) | 4096 posted receives (`rocketnet_server_rio_receives`) |

<a id="current-staging-and-drain-limitations"></a>

### Staging and drain bounds

GSO's three send banks are shared across all sockets. Only its current
producer bank can be filled; the transmit worker consumes published banks.
io_uring likewise has two shared 2048-slot send phases, and an incomplete send
continues to occupy its phase. `num_xdp_queues` adds receive queues and changes socket
rotation; it does not multiply the 4096-slot GSO or 2048-slot io_uring send
capacity (`sdk/rocketnet_server_gso_linux.cpp`,
`sdk/rocketnet_server_uring_linux.cpp`).

Every built-in server transport reports the frames it takes before its next
flush through the optional `send_room` operation (`sdk/rocketnet_driver.h`):
the free slots of the gso producer bank, the io_uring array, the IOCP
producer bank (at most its free send operations), the open RIO fill phase,
the fullest XDP queue, and the portable producer bank. A frame the transport
still holds from an earlier flush is not room.
`rocketnet_server_transport_send_capacity_available` returns it for the
staged transports, so the message layer's rotation stops at a full staging
and its cursor keeps the next unserved connection. The disconnect drain seals
no more than the room each update, a kicked client's burst is sealed at once
only while the room holds all of it, and stop's bursts leave across stop steps
the same way (`proofs/tla/ServerStopSpread.tla`). Stop tells the I/O to stop
only once the optional `send_idle` operation reports every send it was handed
finished (each built-in server transport reports it), so a stopping worker
never retires a bank of disconnect packets unsent. A custom transport without
`send_room` is treated as unbounded; a start it refuses ends that update's
drain, and the bursts wait for the next.

The platform receive helpers spend one read of a fixed budget for each
datagram, kept or discarded: a zero-length datagram, a refused source, a
weather errno (POSIX) or a reset status (Windows) spends a read as a kept
datagram does, and the call returns when the budget is spent. A batch call's
budget is its `max_packets`; a single-packet call's is `ROCKETNET_PLATFORM_RECEIVE_READS`
(64), on the POSIX helpers, the Darwin msgx pump and its per-packet arm, the
generic batch pump and the Windows per-packet loop (`sdk/rocketnet_platform.cpp`).
The Linux GSO server worker's drain (`rocketnet_server_gso_drain`,
`sdk/rocketnet_server_gso_linux.cpp`) counts each message and interrupted call
against `ROCKETNET_GSO_RECV_QUEUE_SIZE` (8192). It checks that threshold before
each receive batch; the final batch can take the count past it by at most
`ROCKETNET_GSO_RECV_BATCH - 1` (63).
The Linux GSO client's plain fallback uses the platform helpers; its GRO path
counts skipped datagrams against `max_packets` and weather errnos against
their own retry budget (`sdk/rocketnet_client_gso_linux.cpp`). These limits
prevent a stream of empty or rejected datagrams from keeping a drain running
indefinitely before publication or a quit test; they do not establish a
worst-case worker-time or shutdown-time bound. The portable receive worker's
model reads one datagram per step against its capacity
(`proofs/tla/RNPortableRx.tla`, `ReadBudget`).

## UDP coalescing

The coalescing rules live once, in `sdk/rocketnet_coalesce.h`: a receive split
at the stride the kernel reports, and a send run of same-destination frames of
one size with at most one shorter tail. Each coalescing transport probes its
offloads at create on the bound socket; a refusal is a tier, not an error, and
the create log line names each feature as yes or no.

- **GSO (Linux):** `UDP_SEGMENT` on send, `UDP_GRO` on receive. The server
  (`sdk/rocketnet_server_gso_linux.cpp`) does both. The client
  (`sdk/rocketnet_client_gso_linux.cpp`) sends every datagram to its one
  connected server, so each send batch is one `sendmmsg` call and each run
  leaves as one message with a `UDP_SEGMENT` cmsg at the run's stride. It
  probes `UDP_SEGMENT` at create as the server does; a refused probe is the
  plain tier, one datagram per message. A live `EINVAL`, `EIO` or `EOPNOTSUPP`
  on a segmented message moves the client to the plain tier for good and
  resends from that message's first datagram, so no datagram is lost. On
  receive the client turns `UDP_GRO` on at create (a refused probe is the
  plain tier, the platform's `recvmmsg`); each `recvmmsg` then takes one
  message into a 64 KiB coalesce buffer allocated at create, and
  `rocketnet_coalesce_split` cuts it at the `UDP_GRO` stride into the I/O
  thread's receive slots, in arrival order. Segments past the receive
  buffer's room are dropped and counted; a truncated control area or a
  short `UDP_GRO` cmsg is one counted fault and the message one datagram.
- **IOCP (Windows 11, Server 2022 or later):** USO on send through the
  `UDP_SEND_MSG_SIZE` socket option, URO on receive through
  `UDP_RECV_MAX_COALESCED_SIZE` with `WSARecvMsg` reading the
  `UDP_COALESCED_INFO` stride. `rocketnet_server_iocp_features` reports
  `ROCKETNET_IOCP_FEATURE_USO` and `ROCKETNET_IOCP_FEATURE_URO`. In a DEV build,
  the test-only create flags `ROCKETNET_IOCP_DISABLE_USO` and
  `ROCKETNET_IOCP_DISABLE_URO` skip each probe; production always probes both.
  The Winsock reference documents the segment size as a socket option, not a
  per-send control message, so a run of more than one frame sets the option to
  the run's stride and posts one `WSASendMsg` with one buffer per frame. Every
  frame of a run stays owned until that one completion retires the run. A live
  USO send refused with `WSAEINVAL` or `WSAEOPNOTSUPP` clears the feature and
  resends the run frame by frame, counted in `ROCKETNET_IOCP_USO_FALLBACKS`.
  `ROCKETNET_SERVER_NUM_XDP_QUEUES` (a Linux AF_XDP setting) is ignored here
  and by RIO: IOCP runs one completion worker plus one send worker and RIO one
  I/O worker, so a client's datagrams are not reordered by several workers
  dequeuing one completion port.

Start/finish/abort may run on concurrent fill threads, with one producer per
frame. Join them before update, receive, flush, stop or destroy. Flush is called
for staged work; the backend retains ownership until it can safely reuse
that storage. XDP retries a flush on each `send_packets` call, including when
no new commit advances the transport counter. With three staging buffers (`ROCKETNET_XDP_NUM_BUFFERS`),
`rocketnet_xdp_buffer_exchange_t::publish` queues at most two and keeps the remaining
buffer for producers until the worker retires the queued generations. The next flush
publishes the retained work. `RNDriverSendStaging.Publish`, `WorkerRetire`
and `RetryFlushPublishes` model this rule. Packet-slot capacity is still
`ROCKETNET_XDP_SEND_QUEUE_SIZE` per buffer; retaining producer storage does
not permit an unbounded backlog. None
of these rules sets a frame, packet-send or ACK rate.

## Creation and shutdown

The factory borrows resolved configuration for the call and retains only what
its instance needs. It owns partial-construction cleanup. A failed candidate's
tentative bind address cannot constrain the next candidate. A discovering
factory reports its actual port on success. Existing XDP preflight and the
commit barrier remain in its adapter/backend.

An explicit port that another socket already holds is refused on every
transport. The `gso` and `io_uring` server transports bind their first socket
before they set `SO_REUSEPORT` on it, whether the port is explicit or
discovered, so a port held by another socket (another server, or any socket
with `SO_REUSEPORT` set) fails that bind with `EADDRINUSE`, as the `portable`
bind does: the create logs `failed to bind socket` and returns NULL, a refusal
the ladder treats like any other. The server's own later sockets set
`SO_REUSEPORT` before they bind and join its group on the same port. The
functional cases `test_gso_server_refuses_explicit_port_in_use` and
`test_uring_server_refuses_explicit_port_in_use`
(`tests/functional/posix_socket_errors.cpp`) hold this.

A factory reports why it failed through `config->status`
(`rocketnet_server_transport_io_config_t.status` and
`rocketnet_client_transport_io_config_t.status`, `sdk/rocketnet_driver.h`),
one form for every transport. A create that is refused an allocation writes
`ROCKETNET_STATUS_OUT_OF_MEMORY` there, at the allocation that failed, through
`rocketnet_create_status` (`sdk/rocketnet_internal.h`), which never replaces
an `OUT_OF_MEMORY` already written. The platform socket, wake and thread
creates take the status for the allocations they make, and the xdp umem
(`sdk/rocketnet_server_socket_linux.cpp`), allocated outside
`rocketnet_malloc`, writes it the same way. Every built-in transport (portable,
msgx, gso, xdp, io_uring, iocp, rio, loopback) reports it so; a native
extension's create does the same. Any other NULL leaves the status OK and is a
refusal. The transport ladder falls to the next
candidate on a refusal, and on OUT_OF_MEMORY it stops:
`rocketnet_server_transport_create_ex` (`sdk/rocketnet_server_transport.cpp`)
and `rocketnet_client_transport_create_ex`
(`sdk/rocketnet_client_transport.cpp`) destroy what they built and return
NULL with `ROCKETNET_STATUS_OUT_OF_MEMORY`.

Server shutdown uses separate idempotent `stop` and `stopped` callbacks.
Stop requests shutdown and returns; stopped checks completion and returns false
while workers or kernel operations still own resources. Neither waits for a
worker, joins it, or frees storage. Destroy asserts completion, then synchronously
joins and releases resources. The client and server transport wrappers use the
same stop/stopped/destroy order. Keep backend and callback contexts alive through
destroy. A stuck operation leaves completion false; timeout is never permission
to reclaim or detach memory that an operation still owns.

Clients use create/destroy/send/receive callbacks. A transport that supplies
wait and wake callbacks uses the bounded client I/O core: one background thread
receives and sends batched datagrams through owned handoff rings while the frame
thread advances protocol state. A backend without those callbacks uses the base
path: one polling receive worker exchanges receive buffers under a short mutex,
and sends run synchronously on the frame thread. Destroy runs only after stop
completion, joins that one client thread, and releases backend-owned work.

## ICMP errors

A UDP send to a remote endpoint that died hard comes back as ICMP port or host
unreachable. A BSD stack reports such an error to the next receive or send on a
connected socket as ECONNREFUSED, EHOSTUNREACH or ENETUNREACH. A client's
portable or msgx socket is connected to its one server
(`rocketnet_platform_socket_connect`, so the kernel drops datagrams from any
other source), and the kernel routes these errors to it; server sockets are
never connected. The receive arms still treat ECONNREFUSED, EHOSTUNREACH,
ENETUNREACH, EHOSTDOWN, ENETDOWN, ENOBUFS and EINTR as weather
(`rocketnet_platform_receive_weather` in `sdk/rocketnet_platform.cpp`): the
portable arm (`rocketnet_platform_socket_receive_packet`) and the msgx arm
(`rocketnet_platform_socket_receive_packets`) skip the error and keep
draining. These errors are nonfatal and do not pause the msgx pump. Each call
has one read budget (`max_packets` for a batch, 64 for the single-packet call),
and every read spends one unit of it: a datagram kept, a zero-length datagram
or refused source skipped, a weather error skipped. A spent budget ends the
current drain so the next call can continue, so neither a repeating errno nor
a stream of chaff holds a drain. EAGAIN ends a drain because the queue is empty. Any other
receive errno pauses the msgx pump with one warning per pause
(`rocketnet_msgx_pause`). The send side
already classes the same errnos as weather. A backend's receive keeps the same
contract: one client's death never stalls or degrades another client's traffic.
The functional cases in tests/functional/posix_hard_killed_client.cpp hold this per
transport cell, and an injected-errno seam covers each receive arm. The macOS
cells are the portable and msgx servers with the portable and msgx clients;
the seam cases are macOS only. The Linux cells are the portable, gso and
io_uring servers (io_uring where `rocketnet_server_uring_available()` is true)
with the portable client, IPv4 and IPv6: `test_posix_hard_killed_client_*`,
`test_posix_send_to_closed_port_*` and `test_posix_client_server_gone_*`.
Linux reports ICMP port unreachable to an unconnected UDP socket only with
`IP_RECVERR`, which no server socket sets, so the kernel drops the answers to
the server's sends to a killed client or a closed port: the Linux server cells
prove those sends never stall or degrade the other clients, the dead slot
times out and frees, and a fifth client connects. The client socket is
connected, and Linux reports ICMP errors to it without `IP_RECVERR`:
`test_posix_client_server_gone_*` proves the portable client's receive takes
each ECONNREFUSED as weather. The three hard-killed kinds also have an xdp
server cell with the portable client, IPv4 and IPv6
(`test_posix_hard_killed_client_xdp_ipv4_client_portable`,
`test_posix_hard_killed_client_xdp_ipv6_client_portable`,
`test_posix_send_to_closed_port_xdp_ipv4_client_portable`,
`test_posix_send_to_closed_port_xdp_ipv6_client_portable`,
`test_posix_client_server_gone_xdp_ipv4_client_portable`,
`test_posix_client_server_gone_xdp_ipv6_client_portable`). That cell runs only
as root inside a virtme-ng guest.

## Server network threads and their priority

The library sets the server thread priority on each network thread a server
transport creates, from the creating thread right after the create
(`rocketnet_platform_server_thread_priority`, `sdk/rocketnet_platform.cpp`):
`SCHED_RR` at `sched_get_priority_min( SCHED_RR )` on Linux and at
32 on macOS, one above an ordinary thread on both (macOS's SCHED_RR range is
15 to 47 and its default 31),
`THREAD_PRIORITY_TIME_CRITICAL` on Windows.

| Transport | Network threads it creates | What the library sets |
|---|---|---|
| GSO (Linux) | one receive worker per queue plus one transmit worker | `SCHED_RR`, minimum priority, after each create |
| XDP (Linux) | one thread per queue (`sdk/rocketnet_server_socket_linux.cpp`) | on itself at thread start: pinned to cpu `num_os_cpus + queue` with `pthread_setaffinity_np`, then `SCHED_RR` at the minimum priority on `pthread_self()` |
| IOCP (Windows) | one completion worker plus one transmit worker; ignores `num_xdp_queues` | `THREAD_PRIORITY_TIME_CRITICAL`, after each create |
| RIO (Windows) | one I/O worker | `THREAD_PRIORITY_TIME_CRITICAL`, after create |
| portable/msgx socket | one receive worker and one transmit worker | nothing |
| io_uring | none | nothing |

`rocketnet_thread_priority( false )` turns the priority calls off for
threads created after it; the XDP thread's pin to its queue's cpu always runs.
A refused call is counted in the stats structs' `thread_priority_refusals` and
logged once per call kind (`docs/ERRORS-AND-LOGGING.md` 3.4); the transport starts
either way. The server task workers of a game and its main thread are the
game's to set; the library sets nothing on them.

## Adding a backend without editing the transport

For an in-tree transport, define a `const rocketnet_transport_backend_t`, add its row
to `rocketnet_drivers.inc`, and add its source to the build. It may support one
endpoint or both. There are no transport switches, members or platform guards
to extend.

An embedding project can also supply its own registry rows and implementation:

```sh
cmake -S . -B build \
  -DROCKETNET_TRANSPORT_REGISTRY_HEADER=/path/to/my_transports.inc \
  -DROCKETNET_TRANSPORT_SOURCES=/path/to/my_transport.cpp
```

The `.inc` has `ROCKETNET_TRANSPORT(...)` rows in the same format as the built-in
file, and no include guard: it is intentionally expanded more than once. Native
registration is compiled into the SDK; this is not a dynamic plugin ABI.
Backend symbols are not public C exports. Per-transport settings and resource
validation belong in the factory; the public transport configuration is unchanged.

`tests/driver_extension` is an executable example: an external row and backend
serve both ends of an encrypted handshake and payload exchange through only
the public transport API. `tools/check_driver_extension.py build` builds and
runs it independently. `test_transport` checks registry ordering, factory refusal,
address isolation, lifecycle and buffered I/O. Existing GSO/IOCP and complete
transport tests cover the real staged paths; hosted tests do not replace XDP
hardware failure tests or per-platform line-rate measurements.

Dispatch measurements and their limits are recorded in
[benchmarks/transport_fill.md](../benchmarks/transport_fill.md).

## The RIO server transport

`rio` (`sdk/rocketnet_server_rio_windows.cpp`, `sdk/rocketnet_server_rio.h`)
is a Windows staged server transport with one registered memory region and one
I/O worker that polls the send and receive completion queues. The frame thread
hands deferred sends to one of three phases; a phase stays worker-owned until
its sends complete, and four receive banks cross the same bounded ownership seam.
Its server rank is 45, after IOCP's 40, so automatic selection
never chooses it ahead of IOCP; pin `rio` to select it directly. Its
`ROCKETNET_RIO_COUNTER_*` counters report sent and received packets, full send
slots, failed sends, waits for a send phase, failed receives, dropped-size and
dropped-overflow receives, failed receive reposts, and RIO commit calls. Its
three send phases and four receive banks are implemented in
`rocketnet_rio_worker_pass` and `rocketnet_server_rio_receive_packets` in
`sdk/rocketnet_server_rio_windows.cpp`; their ownership is modelled in
`proofs/tla/RNRioWorker.tla`.

## Windows connection reset

When a UDP send reaches a port no socket holds, the remote host's stack answers with
an ICMP port unreachable. Winsock, unlike other stacks, reports that answer to
the sending socket's next receive as `WSAECONNRESET` (or `WSAENETRESET` for a
network unreachable), and an overlapped or registered receive completes with
the matching unreachable status. A client that dies without closing its socket
makes the server see one such report for every send to it, so a receive loop
that stops at the first error, or a completion that is not posted again, stops
serving every other client. Every socket rocketnet creates on Windows (the
portable server and client sockets, `iocp` with and without USO and URO, and
`rio`, IPv4 and IPv6) turns off `SIO_UDP_CONNRESET` and `SIO_UDP_NETRESET` at
create through `rocketnet_platform_socket_disable_udp_resets`, which checks each
`WSAIoctl` and logs a refusal once without failing the create. Every receive
path still skips the reset and unreachable statuses and keeps draining: the
portable receive reads on, an `iocp` re-post refused with one waits on the
retry list for the worker's next wakeup, a failed `iocp` completion is
reposted, and a `rio` slice whose re-post fails waits on a retry list for the
next `receive_packets`. `rocketnet_platform_receive_resets` counts the reset
statuses the portable receive skips and the `iocp` and `rio` completions and
refused posts carry. The functional
cases in `tests/functional/windows_connreset.cpp` prove it for each server transport
and family: `windows_connreset_hard_killed_client_*` ends one of four clients
with `TerminateProcess` while the server keeps sending to it, and the other
three see no receive gap past one update interval plus 50 ms, the dead client
times out by the normal timeout and a fifth client connects;
`windows_connreset_send_to_closed_port_*` sends 1000 datagrams to the closed
port with exact receive counts on the live clients; and
`windows_client_connreset_server_gone_*` connects a client whose requests first
went to a closed port.

## The io_uring server transport

`io_uring` (`sdk/rocketnet_server_uring_linux.cpp`, `sdk/rocketnet_server_uring.h`)
is a staged Linux server transport with one io_uring per transport, used only on the
main thread, with no I/O worker threads. An `SO_REUSEPORT` socket group, bound the
way the GSO transport binds its group, shards receives across queues. Each queue
has a provided buffer ring and one multishot `RECVMSG`, so the kernel fills
receive buffers with no syscall per datagram; `send_packets` submits one
batched `SENDMSG` per send run, in chunks no larger than the free submission
queue space.

Send segmentation is the GSO transport's tier, probed at create: every socket
of the group must accept `UDP_SEGMENT` (set to 0), all or nothing, and the
create info line names the result (`UDP_SEGMENT yes`, or `no (one SENDMSG per packet)`). With it, a run of consecutive staged slots to one destination whose
wire bytes are equal, the last allowed to be shorter, found by
`rocketnet_coalesce_send_run` and bounded by `ROCKETNET_GSO_MAX_SEGMENTS` and
`ROCKETNET_GSO_MAX_SUPER_BYTES`, goes out as one `SENDMSG` whose msghdr
(`rocketnet_uring_gso_message`, `sdk/rocketnet_uring_codec.h`) holds one iovec
per slot frame and a `UDP_SEGMENT` cmsg with the stride. The run's first slot
owns the msghdr, the name and the control bytes, and the run's slots their
iovecs, all allocated with the transport; the run's one completion retires
every slot of the run (`GSO_SENDS` counts segmented sends). Without it, or
created with `ROCKETNET_SERVER_URING_DISABLE_SEGMENT` (through
`rocketnet_server_uring_create_flags`, for tests), a run is one slot. A live
refusal, a segmented send's completion of `-EINVAL`, `-EIO` or `-EOPNOTSUPP`,
turns segmentation off for the transport with one warning (`GSO_FALLBACKS`) and
counts that run's packets as `SEND_DROPPED`: unlike the GSO transport's
`sendmmsg`, the run cannot be resent after its completion has handed the slots
back, so the degradation is to plain sends from the next flush, not lossless.
`rocketnet_server_uring_features` reports `ROCKETNET_SERVER_URING_FEATURE_SEGMENT`
while it is in use.

Receive coalescing is the GSO transport's tier too, probed at create: every
socket of the group must accept `UDP_GRO` (set to 1), all or nothing, the
sockets that accepted rolled back otherwise, and the create info line names
the result (`UDP_GRO yes`, or `no (one datagram per receive buffer)`). With
it each queue's multishot msghdr reserves a control area for the `UDP_GRO`
cmsg (`ROCKETNET_URING_GRO_CONTROL_BYTES`), and one provided buffer may hold
a coalesced receive of several datagrams: `rocketnet_uring_recvmsg_parse`
returns the stride, `rocketnet_coalesce_split` cuts the buffer, and every
segment is size-checked and copied into the fill bank before the buffer goes
back to the ring (`proofs/tla/RNUringDriver.tla`, `ReturnAfterLastCopy`). A
receive is copied only while the fill bank has room for a whole coalesced
receive; otherwise it is held with its buffer, in arrival order, until a swap
brings room, and the completions behind it, sends among them, are reaped in
the same call (`proofs/tla/RNUringReapLayout.tla`). Destroy's drain copies
regardless and counts what the bank cannot take as `RECEIVE_DROPPED_OVERFLOW`. `GRO_BATCHES` counts buffers that held more than
one segment, and `GRO_CMSG_FAULTS` a truncated or malformed `UDP_GRO` cmsg,
taken as one datagram (the GSO transport's fault). Created with
`ROCKETNET_SERVER_URING_DISABLE_GRO`, no control area is reserved and a
buffer is one datagram; `rocketnet_server_uring_features` reports
`ROCKETNET_SERVER_URING_FEATURE_GRO` while it is on. A coalesced receive is
up to 64 KiB, so a provided buffer is 66 KiB (`ROCKETNET_URING_RECV_BUFFER_BYTES`):
4096 buffers are 264 MiB of address space per queue, allocated at create
(8 MiB of 2 KiB buffers before `UDP_GRO`), and the buffers are not written at
create, so a page is resident only once the kernel writes it: small datagrams
keep about one page of each buffer resident. Each `send_packets` and `receive_packets` call reaps at most a
fixed completion budget (`ROCKETNET_URING_SEND_BUDGET`, and the receive
bank's size, `rocketnet_server_uring_bank_size`) and `send_packets` waits at most once, so an
update returns under sustained traffic; completions past a budget stay queued
for the next call, and a send without its completion keeps its slot until a
later call reaps it. A queue count past
`ROCKETNET_URING_MAX_QUEUES` refuses the create; zero selects
`ROCKETNET_URING_DEFAULT_QUEUES`.

The transport needs liburing at build time (`ROCKETNET_IO_URING`, on by default
and used when liburing is found). A build without it keeps the registry row with
no platform: the row is absent from the availability lists and a pin of
`io_uring` is skipped by the ladder. At run time `rocketnet_server_uring_available()`
probes once that the kernel accepts the ring, the opcodes and the provided
buffer ring. Its server rank is 25, after `gso` (20) and before `portable` (30 in development, 50 in production)
in the Linux `auto` order; it has no client side.

Its counters (`ROCKETNET_SERVER_URING_COUNTER_*`) are packets sent and received,
submit syscalls, send drops, receive drops for size, bank overflow and
unconvertible source, receives ended for want of buffers, multishot
re-arms, calls that left completions for the next call (`REAP_DEFERRED`), and
send calls that returned with sends still owned by the kernel
(`SEND_DEFERRED`), receive completions that named no provided buffer
(`RECEIVE_DROPPED_NO_BUFFER`), segmented sends (`GSO_SENDS`) and live
segmentation refusals (`GSO_FALLBACKS`). Stop requests cancellation, and stopped remains
false until outstanding completions have relinquished their storage. Destroy
requires stopped and no longer uses a timed wait followed by leaked storage as
normal cleanup. Existing leak counters remain diagnostic compatibility surfaces;
completion, not a timeout, authorizes reclamation.
The send staging (a send run as one send whose completion frees every slot of
the run), the two receive banks, the per-call completion budget and destroy are
modelled in `proofs/tla/RNUringDriver.tla`.

## The XDP receive filter

`sdk/xdp/server_xdp.c` accepts both address families. IPv4 is parsed as
before (a 20-byte header, UDP). IPv6 is parsed as the fixed 40-byte header with
UDP directly behind it. Both families then run one shared function: the
destination port and address against the config map, the wire size bounds, the
filter epoch and value, `ROCKETNET_WIRE_SERVER_PACKET_VALID`, and the redirect
into the XSKMAP for the receive queue. A packet that passes every check but
whose receive queue has no socket in the XSKMAP is dropped and increments
`ROCKETNET_SERVER_XDP_COUNTER_DROPPED_NO_SOCKET` (surfaced as
`XDP_KERNEL_DROPPED_NO_SOCKET`). The counters are the same for both
families. The IPv6 destination is `public_address_ipv6` in
`rocketnet_server_xdp_config_t`; all zero accepts any destination, as a zero
`public_address` does for IPv4.

RocketNet assumes a 1500-byte MTU path and never sends a datagram the network
must fragment, so an IP fragment addressed to the server is hostile or
misconfigured. The first fragment of a datagram to the server's port (the one
that carries the UDP header: IPv4 with more-fragments set and offset 0,
protocol UDP, the UDP header after any options; IPv6 whose next header is the fragment header (44) at offset 0
with more-fragments set and UDP behind it), whose UDP header is in the frame
and names the configured port and address, is dropped and increments
`ROCKETNET_SERVER_XDP_COUNTER_DROPPED_FIRST_FRAGMENT` (surfaced as
`XDP_KERNEL_DROPPED_FIRST_FRAGMENT`). Every other fragment passes to the kernel
stack for reassembly and increments
`ROCKETNET_SERVER_XDP_COUNTER_PASSED_FRAGMENT` (surfaced as
`XDP_KERNEL_PASSED_FRAGMENT`): non-first fragments to any port, and first
fragments to another port or address, to an unconfigured program, or whose
UDP header is not inside the frame. Why this is enough: without its first
fragment the kernel can never reassemble the datagram, so the rest wait in
the reassembly queue and are dropped cheaply at its reassembly timeout, and
other services' fragmented traffic is untouched. Both checks run before the
size, epoch, filter and replay checks for both families, so a fragment is never
counted as any other drop. A non-first fragment carries no UDP header, so
nothing in it names a port, and the program cannot tell one for the server
from one for another service.

RocketNet never sends IPv4 options or IPv6 extension headers, and in XDP mode
no kernel socket listens on the server's port, so a datagram for the server
behind either has nowhere to go but the kernel's error path. An IPv4 datagram
with options (ihl 6 to 15) whose UDP header, after the options, names the
configured port and address is dropped and increments
`ROCKETNET_SERVER_XDP_COUNTER_DROPPED_IP_OPTIONS_OR_EXTENSION` (surfaced as
`XDP_KERNEL_DROPPED_IP_OPTIONS_OR_EXTENSION`); with options to any other port
or address it passes uncounted. An IPv6 frame whose next header is another
extension header (hop-by-hop 0, routing 43, destination options 60, AH 51 or
ESP 50) has its chain walked for at most four headers (hop-by-hop, routing,
destination options and AH by their length fields, a first fragment header
by its 8 bytes). When the walk reaches a UDP header in the frame that names
the configured port and address, the frame is dropped and increments the
same counter, before the size, epoch, filter and replay checks. Every other
extension-header frame passes to the kernel stack and increments
`ROCKETNET_SERVER_XDP_COUNTER_PASSED_IPV6_EXTENSION`: to another port or
address, behind ESP (never opened), behind a non-first fragment header, a
chain longer than the walk, or cut by the frame end. Every other non-UDP next
header, ICMPv6 (neighbor discovery) above all, passes uncounted, as non-UDP
IPv4 does.

The AF_XDP transport serves IPv4 and IPv6 end to end. A server serves one
family, or both when `rocketnet_server_create` is given an IPv4 and an IPv6
address on the same port (`ServedFamilies`). The ladder offers `xdp` for one family, and for
both when the two addresses are on one interface. The receive loop dispatches
each frame on its ethertype through `rocketnet_xdp_frame_receive`
(`sdk/rocketnet_xdp_frame.h`): IPv4 at its offsets, IPv6 at the 40-byte fixed
header, and the from-address carries the family it read. A frame it does not
take is dropped and counted by reason: another ethertype (`not_ip`), an IPv6
extension header (`ipv6_extension`, never parsed at the UDP offsets), the
family the server does not serve (`other_family`), or a frame the parse
refuses or too short for any server packet (`malformed`); a receive
descriptor outside the receive region or its own frame (`descriptor`, a
kernel or driver defect) is dropped before a byte of it is read. Each queue's
worker publishes the sum after every receive pass, and the transport's
counters read it, all queues summed, as `XDP_RECEIVE_FRAME_DROPPED`. The flush builds
each reply with the server address of the destination's family as the source,
and an IPv6 reply carries the UDP checksum computed in software. Create and
the pre-flight find the interface carrying each address over `AF_INET` or
`AF_INET6` and refuse an IPv6 link-local address (fe80::/10) by name. Each
family's gateway MAC comes from that family's default route and neighbour
table, read at create through rtnetlink (`sdk/rocketnet_gateway_netlink.h`: an
`RTM_GETROUTE` dump for the default route's gateway on the interface, then an
`RTM_GETNEIGH` dump for that gateway's link-layer address; no process is
started). `ROCKETNET_GATEWAY_ETHERNET_ADDRESS` overrides both.

A NULL `ipv4_address` or `ipv6_address` leaves that family unserved. An
address not of its family, on another port, or that does not parse is one
log line and `INVALID_ADDRESS` (`sdk/rocketnet_server_transport.cpp`).
The two addresses share the IPv6 wildcard on the one port, with `IPV6_V6ONLY`
off. `portable`
(`sdk/rocketnet_platform.cpp`), `msgx` through that same socket, `gso`
(`sdk/rocketnet_server_gso_linux.cpp`), `io_uring`
(`sdk/rocketnet_server_uring_linux.cpp`), `iocp`
(`sdk/rocketnet_server_iocp_windows.cpp`) and `rio`
(`sdk/rocketnet_server_rio_windows.cpp`) serve both from that bind. Port 0
writes the granted port onto both addresses
(`sdk/rocketnet_server_transport.cpp`). The protocol advertises the named
IPv4 and the named IPv6 (`sdk/rocketnet_server_transport.cpp`).
`TokenAddressAdvertised` accepts a request when the token's address of the
arrival family is one of those (`TokenAddressAdvertised`, `sdk/rocketnet_server_protocol.cpp`). A
v4-mapped source is canonicalised to IPv4 first. The token's other-family
address is not consulted.

The given addresses are advertised addresses, not bind restrictions. With one
address, the server binds that family's wildcard (`0.0.0.0` or `[::]`) on the
given port, even when the address names a specific IP
(`sdk/rocketnet_server_transport.cpp`), and every transport except
`xdp` opens its sockets on that wildcard: `xdp` alone carries
`ROCKETNET_TRANSPORT_BINDS_PUBLIC_ADDRESS` and attaches to the interface that
carries the given address. The `server transport started on` log line names
the wildcard for a socket transport and the given address for `xdp`
(`sdk/rocketnet_server_transport.cpp`). Connect tokens must name the
given address, the one clients reach; a socket transport still receives the
server's port on every local address of the family.

`portable` on OpenBSD cannot clear `IPV6_V6ONLY` and refuses both families
with `TRANSPORT_FAMILIES_UNSUPPORTED` (`sdk/rocketnet_driver_builtin.cpp`).
The ladder falls through. Start returns that status when every candidate that
opened refused for that reason (`sdk/rocketnet_server_transport.cpp`).

`xdp` serves both from one program when both addresses are on one interface.
The config map carries both destinations and no loopback placeholder
(`sdk/rocketnet_server_socket_linux.cpp`). Each family keeps its own
gateway MAC (`sdk/rocketnet_server_socket_linux.cpp`,
`sdk/rocketnet_xdp_frame.h`). The receive loop accepts either family
(`sdk/rocketnet_server_socket_linux.cpp`, `sdk/rocketnet_xdp_frame.h`).
Each reply's source is the server address of the destination's family
(`sdk/rocketnet_server_socket_linux.cpp`). Addresses on two interfaces
refuse with `TRANSPORT_FAMILIES_UNSUPPORTED`
(`sdk/rocketnet_server_socket_linux.cpp`).

The kernel filter's counters surface on the transport counter surface as
`ROCKETNET_COUNTER_XDP_KERNEL_*`, assigned from the kernel's cumulative
per-CPU totals each time the transport counter surface is read. Only a
transport with a kernel prefilter fills them; a transport with none leaves them
zero.

The replay map is read-only to the kernel: it drops a session packet a whole
replay window (`ROCKETNET_HISTORY_PACKETS`, 4096 by default) or more behind
the high-water the userspace window sets, and never writes the map. The
kernel window is the userspace window, so a reordered packet that every other
transport accepts is accepted on XDP too. The transport marks each connected
client after every update.

`test_xdp_filter` judges the program with the kernel. It loads the object
through the verifier and runs crafted frames with `BPF_PROG_TEST_RUN`, which
returns the program's verdict and transmits nothing. It swaps the XSKMAP for a
DEVMAP before load so a redirect can succeed without an AF_XDP socket, and it
reads the stats map back after each frame. It needs root, or CAP_BPF and
CAP_NET_ADMIN; `-v` prints the verifier's last log line. One case,
`xdp_on_link_reply_reaches_the_client`, attaches the program to a veth pair and
runs the example server and client across it, so it skips outside a
virtme-ng guest; the guest boots with the boot tier the xdp pre-flight
verifies, and with room for the server's UMEM and queues (the default guest
memory runs out and the server is killed). The case starts the server with
`--max-clients 4`, so its message object is sized for the one client it
connects rather than the example's default 1000 (`tests/test_xdp_filter.cpp`, the
`--max-clients` option of `tests/client_server/server.c`), and it gives the server and client a pipe it holds open as
stdin, because the server stops when its stdin reaches end of file
(`check_stdin_closed` in `tests/client_server/server.c`) and a guest run without a terminal starts there:

```sh
vng --run ~/vm-kernels/vmlinuz-$(uname -r) --memory 1536M --cpus 3 --append "irqaffinity=0 isolcpus=1-2 nohz_full=1-2" --rwdir <job> -- sudo ./bin/test_xdp_filter -v
```

A reply frame's destination MAC is the source MAC of the newest packet from
that client address that the transport accepted for a connection on this
interface, and the gateway's MAC for an address with none learned. With no
default route the server still starts, a direct response to an unlearned
address goes to the source MAC of the frame it answers, and that MAC is kept
as the address's answered entry. A later reply to that address with no frame
in hand (one the development link simulator held for its latency, or its
duplicate) goes to the answered entry, and so does a reply still held for a
released client, whose learned MAC a forget turns into its answered entry. A
learned or gateway MAC always wins over an answered entry, which never
displaces a learned one. Any other reply that resolves to no MAC, among them
every reply to an address no frame came from, is refused at start and counted
as `SEND_NO_LINK_ADDRESS`, never sent and never counted as sent
(`proofs/tla/RNXdpReplay.tla`: `NeverSeenNeverSentTo`, `HeldReplyFindsItsMac`).

## Embedded XDP source maintenance

The XDP frame codec (`sdk/rocketnet_xdp_frame.h`) synthesises Ethernet + IP +
UDP for both address families: IPv4 byte-identical to the canonical builder
(ihl 5, DF set, ttl 64, header checksum, UDP checksum 0) and IPv6 with the
mandatory UDP checksum (RFC 8200 section 8.1) computed in software over the
pseudo-header. Hardware TX checksum offload (AF_XDP TX metadata, kernel 6.8
or later) is not used.

The filter's decisions also run as ordinary host C in the unit suite, with no
kernel, no root and no BPF syscall. The `rocketnet_xdp_filter_host` target
compiles `tests/xdp_host/server_xdp_host.c`, which includes
`sdk/xdp/server_xdp.c` unchanged, with the BPF build's defines
(`-D__linux__ -D__BPF__ -ffreestanding`) and `tests/xdp_host/` first on that
one target's include path. That directory is a shim: minimal `linux/*.h` and
`bpf/bpf_helpers.h` with only the types, constants and map macros the filter
uses. `struct xdp_md` carries `data` and `data_end` as `unsigned long` so they
hold host pointers. The maps are host storage, and the two helpers are host
functions. `bpf_redirect_map` copies the kernel's semantics, cited beside it
(`__bpf_xdp_redirect_map` in `include/linux/filter.h`, reached through
`xsk_map_redirect`): `XDP_REDIRECT` for a populated slot below `max_entries`,
otherwise the flags' action bits, so `BPF_F_INGRESS` gives `XDP_DROP`.
`test_unit_xdp_filter_host.cpp` drives it with frames written byte by byte,
one table row per frame (bytes, receive queue, wanted verdict, wanted
counter). Each row checks its verdict, its counter, and that every other
counter stayed at zero. Each frame is copied into a heap allocation of
exactly its length, so the sanitizer legs catch a read past `data_end`.

The host tier covers both address families, the address and port match, the
size bounds, the epoch and filter value, every type at every size against the
STANDARD.md size table, the IPv6 extension and fragment passes, and the
redirect decision per receive queue. It also covers the `config == NULL`
branch, which the kernel's one-entry array never reaches. Only the kernel
tier (`test_xdp_filter`) shows what the verifier accepts, the real helpers'
behaviour, and the real per-CPU and XSKMAP maps. The host target is built for
GCC or Clang on a little-endian, non-Windows host, and its cases are in the
unit suite on Linux only. When the filter adopts
a new kernel header, helper or map, the shim follows in the same change, with
the helper's kernel source cited. the counter-row check `test_xdp_transport_host_every_counter_has_a_case`
runs in the static source-audit binary `./bin/source_audit` (which CI runs on
a Linux leg) and fails when a `ROCKETNET_SERVER_XDP_COUNTER_*` has no row
that moves it. To reproduce
the compatible filter, configure with
`-DROCKETNET_XDP_FILTER_HOST_SOURCE=<path>`; the default is the shipped file.

The XDP program is compiled at build time and loaded from memory. On Linux,
`cmake/EmbedXdp.cmake` compiles `sdk/xdp/server_xdp.c` to `server_xdp.o`
with a clang that has the BPF target (the flags of `sdk/xdp/Makefile`, plus
flags that keep checkout paths out of the object) and
embeds the object's bytes in `build/generated/rocketnet_server_xdp.h` as
`rocketnet_server_xdp_o`. `rocketnet_server_socket_create` opens those bytes
with `bpf_object__open_mem` and hands the object to libxdp with
`xdp_program__from_bpf_obj`: the server host needs no compiler, `make`, kernel
headers, temporary directory or child process. `test_xdp_filter` loads the same
`server_xdp.o`. The build is tolerant: XDP needs a clang with the BPF target, libbpf, an
AF_XDP provider (libxdp or the legacy libbpf APIs), and
`-DROCKETNET_XDP=ON` (the default). If a prerequisite is missing or XDP is
disabled, configure names the reason and the whole XDP transport is
compiled out (`ROCKETNET_XDP=0`): no embedded object, no XDP or netlink
gateway code in the library, and no libbpf or libxdp link
(`tools/check_xdp_compiled_out.py --absent` checks the built library). A
transport list naming only `xdp` refuses server start with
`ROCKETNET_STATUS_TRANSPORT_NOT_BUILT`, and `auto` starts at the next rung.
macOS and Windows do not build the object.

`cmake/XdpSources.cmake` lists the object's inputs: CMake recompiles the object
when one changes, and `tools/check_xdp_archive.py` reads the same list. On Linux
the checker compiles the object twice from copies of the sources at different
paths, mtimes and umasks and requires identical bytes, requires the generated
header to embed exactly the object, shows that a changed source changes the
object, and fails when a build tree's embedded object differs from a fresh
compile of the current sources with the tree's own BPF clang, multiarch
triple, `ROCKETNET_HISTORY_PACKETS` and `ROCKETNET_MAX_CLIENTS` (stale):

```sh
python3 tools/check_xdp_archive.py            # checks ./build when it exists
python3 tools/check_xdp_archive.py --build <build-dir>
```

Elsewhere, and with `--committed-only`, it checks only that every listed input
exists, which needs no compiler.
Neither check loads a program or attaches to a network interface.

## Bounded server payload sends

Unstaged server transports reserve at least one payload slot per configured
client, with a minimum of 256 slots. A lower test capacity exercises contention.
For these unstaged transports, the message send pass starts after the last
served connection and offers one frame per connection per round. Full capacity leaves queued reliable work due
for the next update. In-place unreliable writes use only surplus capacity above
a complete round of connected clients; otherwise they join the queue so application call order cannot
starve another connection or that connection's reliable work. Control packets
use the transport's separate send path. A staged transport reports its own
send room as the capacity ([above](#staging-and-drain-bounds)),
so the same rotation applies to it; a custom staged transport without
`send_room` reports none and is not covered.

## Receive lengths and truncated datagrams

Buffered transports write at most the supplied capacity or per-packet stride,
but return each datagram's original length, up to 65535 bytes. A larger byte
count does not grant access to bytes beyond that buffer. Client batch
compaction copies only the stored prefix; the prefilter counts oversized
reports without reading that prefix. The buffered server counts oversized
reports before the protocol reads a packet, so the protocol never sees a
claimed length beyond its available storage. Negative lengths,
lengths beyond 65535, and returned packet counts outside the requested range
remain transport faults. A zero byte count in a batch is a received empty
datagram; zero from a single-packet receive means drained.
