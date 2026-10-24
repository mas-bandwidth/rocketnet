# Native transports

The public transport API in `rocketnet.h` is independent of the backend.
Native transports implement the internal transport interface in
`sdk/rocketnet_driver.h`; they do not add binding or wire-format rules. For
Linux transport selection, pinning, host requirements (`xdp`, `gso`,
`io_uring`, `portable`) and host verification, see
[docs/LINUX.md](docs/LINUX.md).

`rocketnet_drivers.inc` is the built-in registry. Each row supplies a name,
bound-port discovery property, backend record, platform mask, and server/client
preference ranks. Lower nonzero ranks come first in `auto`; zero means that
endpoint is unsupported. IDs, names, diagnostics and platform availability are
derived from these rows. Existing IDs and preference orders are preserved.

The backend record points to immutable server and/or client operations. The
transport retains the selected operations and opaque instance for its lifetime.
Factories, resources and platform decisions stay in the transport layer. The
transport branches on generic capabilities, never a specific backend identity.

A transport with a preference rank at or above `ROCKETNET_TRANSPORT_PIN_ONLY`
(1000) is pin-only: it stays in the availability list, so an explicit name
always selects it, but `auto` never chooses it. The loopback transport is the
one pin-only row. It is in-process, carries datagrams through memory with no
socket and no system network call. It is a production transport selected only
by name. auto never chooses it. Its process-wide table is `ROCKETNET_LOOPBACK_MAX_ENDPOINTS`
records of 72 bytes each (72,072 bytes) under one mutex. Each live endpoint
allocates its full `ROCKETNET_LOOPBACK_QUEUE_PACKETS` queue at create, so
runtime sends never allocate or grow it. A process using loopback must budget
for every live endpoint's complete queue capacity. The cost is paid only by a
process that selects loopback by name. Lookups scan the table linearly under the mutex.

The names `rocketnet_server_uring_available`, `rocketnet_server_uring_leaked_bytes`
and `rocketnet_server_uring_leaked_transports` are internal transport helpers
(`sdk/rocketnet_server_uring.h`); a game uses `rocketnet_server_create`,
`rocketnet_server_required_memory` and the other server entry points in
`sdk/rocketnet.h`.

## Server capabilities and ownership

Two I/O shapes preserve the existing implementations:

- **Buffered:** the transport supplies its common control, payload and receive
  buffers. `send_batch` and `receive_batch` borrow that storage only for the call.
  This is the portable/msgx shape. The current server backend copies sends into
  bounded TX banks for its transmit worker and receives into bounded RX banks
  on its receive worker; the borrowed interface does not transfer the caller's
  buffers to either worker.
- **Staged:** the transport supplies frame storage and a complete receive batch.
  XDP, GSO and IOCP use this shape, with their own pools and worker arrangements.
  The transport does not allocate, recycle or schedule those backend frames.

The staged callbacks are start, finish, abort, flush and receive. Start returns
a `ROCKETNET_FRAME_STRIDE` root; the wire starts at `ROCKETNET_FRAME_WIRE_OFFSET`.
The caller-visible payload alignment must remain valid. Receive returns at most
`ROCKETNET_SERVER_SOCKET_MAX_PROCESS_PACKETS` datagrams, each within the receive
ceiling, with normalized source addresses. Its data remains valid until the
next receive or destruction, including while workers accept further packets.

The flags express the differences the transport needs:

| Flag | Meaning |
| --- | --- |
| `STAGED` | Use transport-owned frames and receive views instead of borrowed transport buffers. |
| `FLUSH_ON_UPDATE` | Flush staged control traffic at the end of update. Buffered control traffic completes by the end of update; full batches may flush during the update walk. |
| `FINISH_FINAL` | A finished frame cannot be withdrawn. Otherwise abort may withdraw it before flush. Unfinished reservations can always be aborted. |
| `BINDS_PUBLIC_ADDRESS` | The factory binds the advertised address rather than the transport's ordinary bind address. |

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

Every transport receives at least twice what it sends in one update:
`receive_capacity >= 2 * send_budget`, checked by
`test_unit_receive_budget_twice_send.cpp` and locked by a
`ROCKETNET_STATIC_ASSERT` beside each transport’s constants. The send budget
is what one update stages or submits; the receive capacity is the bank,
ring, staging or completion budget the send path can fill while it waits.
The server process batch `ROCKETNET_SERVER_SOCKET_MAX_PROCESS_PACKETS`
(`sdk/rocketnet_constants.h`, 16000) bounds every transport drain.

| Transport | Send budget | Receive capacity |
| --- | --- | --- |
| portable | 256–1000, max `ROCKETNET_SERVER_MAX_SEND_PACKETS` (`sdk/rocketnet_constants.h:87`) | 16000 per update (`sdk/rocketnet_constants.h:81`) |
| msgx | portable batch256–1000 (`sdk/rocketnet_constants.h:87`) | 16000 per update (`sdk/rocketnet_constants.h:81`) |
| loopback | portable batch256–1000 (`sdk/rocketnet_constants.h:87`) | 2048 per endpoint (`sdk/rocketnet_driver_loopback.h:26`) |
| gso | 4096 (`sdk/rocketnet_server_gso.h:49`) | 8192 per queue (`sdk/rocketnet_server_gso.h:57`) |
| io_uring | 2048 staged;10240 completion reap (`sdk/rocketnet_server_uring.h:49`,`sdk/rocketnet_server_uring.h:70`) | 4096 receive buffers per queue;16000 bank (`sdk/rocketnet_server_uring.h:55`,`sdk/rocketnet_server_uring.h:69`) |
| xdp | 4096 (`sdk/rocketnet_constants.h:57`) | 16384 staging (`sdk/rocketnet_constants.h:58`) |
| iocp | 2048 (`sdk/rocketnet_server_iocp.h:22`) | 4096 queue (`sdk/rocketnet_server_iocp.h:24`) |
| rio | 2048 per phase (`sdk/rocketnet_server_rio.h:34`) | 4096 posted receives (`sdk/rocketnet_server_rio.h:35`) |

## UDP coalescing

The coalescing rules live once, in `sdk/rocketnet_coalesce.h`: a receive split
at the stride the kernel reports, and a send run of same-destination frames of
one size with at most one shorter tail. Each coalescing transport probes its
offloads at create on the bound socket; a refusal is a tier, not an error, and
the create log line names each feature as yes or no.

- **GSO (Linux):** `UDP_SEGMENT` on send, `UDP_GRO` on receive.
- **IOCP (Windows 11, Server 2022 or later):** USO on send through the
  `UDP_SEND_MSG_SIZE` socket option, URO on receive through
  `UDP_RECV_MAX_COALESCED_SIZE` with `WSARecvMsg` reading the
  `UDP_COALESCED_INFO` stride. `rocketnet_server_iocp_features` reports
  `ROCKETNET_IOCP_FEATURE_USO` and `ROCKETNET_IOCP_FEATURE_URO`; the create flags
  `ROCKETNET_IOCP_DISABLE_USO` and `ROCKETNET_IOCP_DISABLE_URO` skip each probe.
  The Winsock reference documents the segment size as a socket option, not a
  per-send control message, so a run of more than one frame sets the option to
  the run's stride and posts one `WSASendMsg` with one buffer per frame. Every
  frame of a run stays owned until that one completion retires the run. A live
  USO send refused with `WSAEINVAL` or `WSAEOPNOTSUPP` clears the feature and
  resends the run frame by frame, counted in `ROCKETNET_IOCP_USO_FALLBACKS`.

Start/finish/abort may run on concurrent fill threads, with one producer per
frame. Join them before update, receive, flush, stop or destroy. Flush is called
for staged work; the backend retains ownership until it can safely reuse
that storage. XDP retries a flush on each `send_packets` call, including when
no new commit advances the transport counter. With two staging buffers,
`rocketnet_send_handoff_t::publish` queues at most one and keeps the other
for producers until the worker retires the queued generation. The next flush
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

A factory reports why it failed through `config->status`
(`rocketnet_server_transport_io_config_t.status` and
`rocketnet_client_transport_io_config_t.status`, sdk/rocketnet_driver.h:52 and :115),
one form for every transport. `rocketnet_server_transport_io_open` and
`rocketnet_client_transport_io_open` write `ROCKETNET_STATUS_OUT_OF_MEMORY` there
when the create returned NULL after an allocation on the calling thread was
refused (sdk/rocketnet_driver.cpp:126, :148 and :182, reading the per-thread
count kept at sdk/rocketnet.cpp:369); any other NULL leaves it OK and is a
refusal. The count rises on every `rocketnet_malloc` refusal
(sdk/rocketnet.cpp:461 and :475) and on the two page buffers a transport
allocates outside it, the xdp umem (sdk/rocketnet_server_socket_linux.cpp:1720)
and the rio region (sdk/rocketnet_server_rio_windows.cpp:508), through
`rocketnet_allocation_refused` (sdk/rocketnet.cpp:378). So every built-in
transport (portable, msgx, gso, xdp, io_uring, iocp, rio, loopback) reports it
through the open. The transport ladder falls to the next
candidate on a refusal, and on OUT_OF_MEMORY it stops:
`rocketnet_server_transport_create_ex` (sdk/rocketnet_server_transport.cpp:1299)
and `rocketnet_client_transport_create_ex`
(sdk/rocketnet_client_transport.cpp:882) destroy what they built and return
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

A UDP send to a peer that died hard comes back as ICMP port or host
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
draining. The error is never fatal, never ends a drain, and never
pauses the msgx pump. EAGAIN ends a drain, because the queue is empty. Any other
receive errno pauses the msgx pump with one warning per pause
(`rocketnet_msgx_pause`). The send side
already classes the same errnos as weather. A backend's receive keeps the same
contract: one peer's death never stalls or degrades another peer's traffic.
The functional cases in functional/posix_hard_killed_peer.cpp hold this per
transport cell, and an injected-errno seam covers each receive arm.

## Server network threads and their priority

The library sets the server thread priority on each network thread a server
transport creates, from the creating thread right after the create
(`rocketnet_platform_server_thread_priority`, `sdk/rocketnet_platform.cpp:1434`, `:3142`
and `:3573`):
`SCHED_RR` at `sched_get_priority_min( SCHED_RR )` on Linux and macOS,
`THREAD_PRIORITY_TIME_CRITICAL` on Windows.

| Transport | Network threads it creates | What the library sets |
|---|---|---|
| GSO (Linux) | one receive worker per queue plus one transmit worker | `SCHED_RR`, minimum priority, after each create |
| XDP (Linux) | one thread per queue (`sdk/rocketnet_server_socket_linux.cpp:2568-2577`) | on itself at thread start: pinned to cpu `num_os_cpus + queue` with `pthread_setaffinity_np`, then `SCHED_RR` at the minimum priority on `pthread_self()` |
| IOCP (Windows) | one worker per completion thread plus one transmit worker | `THREAD_PRIORITY_TIME_CRITICAL`, after each create |
| RIO (Windows) | one I/O worker | `THREAD_PRIORITY_TIME_CRITICAL`, after create |
| portable/msgx socket | one receive worker and one transmit worker | nothing |
| io_uring | none | nothing |

Every server also runs one background stats worker. It uses a socket-free wake
and does not receive the network-thread priority override described above.

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
[bench/transport_fill.md](bench/transport_fill.md).

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
`tla/RNRioWorker.tla`.

## Windows connection reset

When a UDP send reaches a port no socket holds, the peer's stack answers with
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
cases in `functional/windows_connreset.cpp` prove it for each server transport
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
main thread, with no I/O worker threads. The server still has its separate
background stats worker. An `SO_REUSEPORT` socket group, bound the
way the GSO transport binds its group, shards receives across queues. Each queue
has a provided buffer ring and one multishot `RECVMSG`, so the kernel fills
receive buffers with no syscall per datagram; `send_packets` submits one
batched `SENDMSG` per staged slot, in chunks no larger than the free submission
queue space. Each `send_packets` and `receive_packets` call reaps at most a
fixed completion budget (`ROCKETNET_URING_SEND_BUDGET`,
`ROCKETNET_URING_RECEIVE_BUDGET`) and `send_packets` waits at most once, so an
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
(`SEND_DEFERRED`), and receive completions that named no provided buffer
(`RECEIVE_DROPPED_NO_BUFFER`). Stop requests cancellation, and stopped remains
false until outstanding completions have relinquished their storage. Destroy
requires stopped and no longer uses a timed wait followed by leaked storage as
normal cleanup. Existing leak counters remain diagnostic compatibility surfaces;
completion, not a timeout, authorizes reclamation.
The send staging, the two receive banks, the per-call completion budget and
destroy are modelled in `tla/RNUringDriver.tla`.

## The XDP receive filter

`server_xdp/server_xdp.c` accepts both address families. IPv4 is parsed as
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
refuses or too short for any server packet (`malformed`). The flush builds
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
log line and `INVALID_ADDRESS` (`sdk/rocketnet_server_transport.cpp:1059-1079`).
The two addresses share the IPv6 wildcard on the one port, with `IPV6_V6ONLY`
off (`sdk/rocketnet_server_transport.cpp:1086`). `portable`
(`sdk/rocketnet_platform.cpp:585`), `msgx` through that same socket, `gso`
(`sdk/rocketnet_server_gso_linux.cpp:334`), `io_uring`
(`sdk/rocketnet_server_uring_linux.cpp:230`), `iocp`
(`sdk/rocketnet_server_iocp_windows.cpp:513`) and `rio`
(`sdk/rocketnet_server_rio_windows.cpp:489`) serve both from that bind. Port 0
writes the granted port onto both addresses
(`sdk/rocketnet_server_transport.cpp:1406`). The protocol advertises the named
IPv4 and the named IPv6 (`sdk/rocketnet_server_transport.cpp:1460`).
`TokenAddressAdvertised` accepts a request when the token's address of the
arrival family is one of those (`sdk/rocketnet_server_protocol.cpp:177`). A
v4-mapped source is canonicalised to IPv4 first. The token's other-family
address is not consulted.

`portable` on OpenBSD cannot clear `IPV6_V6ONLY` and refuses both families
with `TRANSPORT_FAMILIES_UNSUPPORTED` (`sdk/rocketnet_driver_builtin.cpp:20`).
The ladder falls through. Start returns that status when every candidate that
opened refused for that reason (`sdk/rocketnet_server_transport.cpp:1375`).

`xdp` serves both from one program when both addresses are on one interface.
The config map carries both destinations and no loopback placeholder
(`sdk/rocketnet_server_socket_linux.cpp:1848`). Each family keeps its own
gateway MAC (`sdk/rocketnet_server_socket_linux.cpp:1524`,
`sdk/rocketnet_xdp_frame.h:524`). The receive loop accepts either family
(`sdk/rocketnet_server_socket_linux.cpp:2858`, `sdk/rocketnet_xdp_frame.h:359`).
Each reply's source is the server address of the destination's family
(`sdk/rocketnet_server_socket_linux.cpp:2975`). Addresses on two interfaces
refuse with `TRANSPORT_FAMILIES_UNSUPPORTED`
(`sdk/rocketnet_server_socket_linux.cpp:1479`).

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
`xdp_on_link_reply_reaches_the_peer`, attaches the program to a veth pair and
runs the example server and client across it, so it skips outside a
virtme-ng guest; the guest boots with the boot tier the xdp pre-flight
verifies, and with room for the server's UMEM and queues (the default guest
memory runs out and the server is killed). The case starts the server with
`--max-clients 4`, so its message object is sized for the one client it
connects rather than the example's default 1000 (test_xdp_filter.cpp:1280,
server.c:872), and it gives the server and client a pipe it holds open as
stdin, because the server stops when its stdin reaches end of file
(server.c:150-165) and a guest run without a terminal starts there:

```sh
vng --run ~/vm-kernels/vmlinuz-$(uname -r) --memory 1536M --cpus 3 --append "irqaffinity=0 isolcpus=1-2 nohz_full=1-2" --rwdir <job> -- sudo ./bin/test_xdp_filter -v
```

A reply frame's destination MAC is the source MAC of the newest packet from
that peer address that the transport accepted for a connection on this
interface, and the gateway's MAC for an address with none learned. With no
default route the server still starts, a direct response to an unlearned
address goes to the source MAC of the frame it answers, and any other reply
that resolves to no MAC is refused at start and counted as
`SEND_NO_LINK_ADDRESS`, never sent and never counted as sent.

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
`server_xdp/server_xdp.c` unchanged, with the BPF build's defines
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
the helper's kernel source cited. `test_xdp_transport_host_every_counter_has_a_case` fails
when a `ROCKETNET_SERVER_XDP_COUNTER_*` has no row that moves it. To reproduce
the compatible filter, configure with
`-DROCKETNET_XDP_FILTER_HOST_SOURCE=<path>`; the default is the shipped file.

The XDP program is compiled at build time and loaded from memory. On Linux,
`cmake/EmbedXdp.cmake` compiles `server_xdp/server_xdp.c` to `server_xdp.o`
with a clang that has the BPF target (the flags of `server_xdp/Makefile`, plus
flags that keep checkout paths out of the object) and
embeds the object's bytes in `build/generated/rocketnet_server_xdp.h` as
`rocketnet_server_xdp_o`. `rocketnet_server_socket_create` opens those bytes
with `bpf_object__open_mem` and hands the object to libxdp with
`xdp_program__from_bpf_obj`: the server host needs no compiler, `make`, kernel
headers, temporary directory or child process. `test_xdp_filter` loads the same
`server_xdp.o`. The build is tolerant: XDP is built only with a clang that has
the BPF target, libbpf and libxdp, and `-DROCKETNET_XDP=ON` (the default).
Short of any of them the configure says why (`XDP off: no clang with the BPF
target was found, so XDP is compiled out of this build`, `XDP off: libbpf or
libxdp was not found, so XDP is compiled out of this build` or `XDP off:
ROCKETNET_XDP=OFF; the xdp transport is unavailable; BPF/libxdp are not
required`) and the whole XDP transport is
compiled out (`ROCKETNET_XDP=0`): no embedded object, no XDP or netlink
gateway code in the library, and no libbpf or libxdp link
(`tools/check_xdp_compiled_out.py --absent` checks the built library). A
transport list naming `xdp` refuses it at create with
`ROCKETNET_STATUS_TRANSPORT_NOT_BUILT`, and `auto` starts at the next rung.
macOS and Windows do not build the object.

`cmake/XdpSources.cmake` lists the object's inputs: CMake recompiles the object
when one changes, and `tools/check_xdp_archive.py` reads the same list. On Linux
the checker compiles the object twice from copies of the sources at different
paths, mtimes and umasks and requires identical bytes, requires the generated
header to embed exactly the object, shows that a changed source changes the
object, and fails when a build tree's embedded object differs from a fresh
compile of the current sources (stale):

```sh
python3 tools/check_xdp_archive.py            # checks ./build when it exists
python3 tools/check_xdp_archive.py --build <build-dir>
```

Elsewhere, and with `--committed-only`, it checks only that every listed input
exists, which needs no compiler.
Neither check loads a program or attaches to a network interface.

The Linux connection-type label is an IPv4 main-table heuristic. It does not
model policy routing or multipath entries without a direct output-interface
attribute, and may report the wired fallback for those configurations. This
label does not choose the transport route or limit IPv6 endpoint support.

## Bounded server payload sends

Unstaged server transports reserve at least one payload slot per configured
client, with a minimum of 256 slots. A lower test capacity exercises contention.
The message send pass starts after the last served connection and offers one
frame per connection per round. Full capacity leaves queued reliable work due
for the next update. In-place unreliable writes use only surplus capacity above
a complete round of connected peers; otherwise they join the queue so application call order cannot
starve another connection or that connection's reliable work. Control packets
use the transport's separate send path.

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
