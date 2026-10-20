# Native transports

The public transport API in `rocketnet.h` is independent of the backend.
Native transports implement `sdk/rocketnet_driver.h`; they do not add binding or
wire-format rules. For Linux transport selection, pinning, host requirements (`xdp`, `gso`, `io_uring`, `portable`) and host verification, see [docs/LINUX.md](docs/LINUX.md).

`sdk/rocketnet_drivers.inc` is the built-in registry. Each row supplies a symbol, a name,
bound-port discovery property, backend record, platform mask, and server/client
preference ranks. Lower nonzero ranks come first in `auto`; zero means that
endpoint is unsupported. IDs, names, diagnostics and platform availability are
derived from these rows.

The backend record points to immutable server and/or client operations. The
transport retains the selected operations and opaque instance for its lifetime.
Factories, resources and platform decisions stay in the backend layer. The
transport branches on generic capabilities, never a specific backend identity.

A transport with a preference rank at or above `ROCKETNET_DRIVER_PIN_ONLY`
(1000) is pin-only: it stays in the availability list, so an explicit name
always selects it, but `auto` never chooses it. The loopback transport is the
one pin-only row. It is in-process, carries datagrams through memory with no
socket and no system network call, and serves tests and examples; it is never
chosen by auto. Its process-wide table is `ROCKETNET_LOOPBACK_MAX_ENDPOINTS`
records of 72 bytes each (72,072 bytes) under one mutex. Each live endpoint
allocates its queue when it starts, `ROCKETNET_LOOPBACK_QUEUE_PACKETS` x
`sizeof(rocketnet_loopback_packet_t)` = 1,531,904 bytes, so a full table of
live endpoints is about 1,533,435,904 bytes. The cost is paid only by a process
that instantiates the loopback transport, which is tests and examples, never a
game on auto. Lookups scan the table linearly under the mutex.

The names `rocketnet_server_uring_available`, `rocketnet_server_uring_leaked_bytes`
and `rocketnet_server_uring_leaked_drivers` are internal helpers of the io_uring
transport (`sdk/rocketnet_server_uring.h`); a game uses `rocketnet_server_create`,
`rocketnet_server_required_memory` and the other server entry points in
`sdk/rocketnet.h`.

## Server capabilities and ownership

A server backend has one of two I/O shapes:

- **Buffered:** the transport supplies its common control, payload and receive
  buffers. `send_batch` and `receive_batch` borrow that storage only for the call.
  This is the shape of portable, msgx and loopback. It does not require a backend-owned pool.
- **Staged:** the backend supplies frame storage and a complete receive batch.
  XDP, GSO, io_uring, IOCP and RIO use this shape, with their own pools and worker arrangements.
  The transport does not allocate, recycle or schedule those backend frames.

The staged callbacks are `start_packet`, `finish_packet`, `abort_packet`,
`flush` and `receive`. `start_packet` returns a `ROCKETNET_FRAME_STRIDE` root;
the wire starts at `ROCKETNET_FRAME_WIRE_OFFSET`. The caller-visible payload
alignment must remain valid. `receive` returns at most
`ROCKETNET_SERVER_SOCKET_MAX_PROCESS_PACKETS` datagrams, each within the receive
ceiling, with normalized source addresses. Its data remains valid until the
next receive or destruction, including while workers accept further packets.

The flags express the differences the transport needs:

| Flag | Meaning |
| --- | --- |
| `STAGED` | Use backend-owned frames and receive views instead of borrowed transport buffers. |
| `FLUSH_ON_UPDATE` | Flush staged control traffic at the end of update. Buffered control traffic completes by the end of update; full batches may flush during the update walk. |
| `FINISH_FINAL` | A finished frame cannot be withdrawn. Otherwise abort may withdraw it before flush. Unfinished reservations can always be aborted. |
| `BINDS_PUBLIC_ADDRESS` | The factory binds the advertised address rather than the transport's ordinary bind address. |

A flush means submission, not necessarily completion or reusable storage. IOCP
keeps operation buffers until completion retirement. The backend owns that
decision; it must never hand a still-owned frame to another producer.

An optional kernel replay capability protects session packets from replay on a
kernel dataplane. A backend with it implements `replay_forget`,
`replay_mark` and `replay_commit` together, all three or none; the transport
calls them on the main thread once per update, after packet processing, and
nothing else writes the backend's replay map. XDP is the only built-in backend
that reports it.

## UDP coalescing

The coalescing rules live once, in `sdk/rocketnet_coalesce.h`: a receive split
at the stride the kernel reports, and a send run of same-destination frames of
one size with at most one shorter tail. Each coalescing transport probes its
offloads when it starts on the bound socket; a refusal is a tier, not an error,
and the start log line names each feature as yes or no.

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

`start_packet`, `finish_packet` and `abort_packet` may run on concurrent fill threads, with one producer per
frame. Join them before update, receive, flush, stop or destroy. Flush is called
once for a staging generation; the backend retains ownership until it can
safely reuse that storage. None of these rules sets a frame, packet-send or ACK
rate.

## Creation and shutdown

The factory borrows resolved configuration for the call and retains only what
its instance needs. It owns partial-construction cleanup. A failed candidate's
tentative bind address cannot constrain the next candidate. A discovering
factory reports its actual port on success. The XDP pre-flight and the commit
barrier live in the XDP backend.

Server stop is an idempotent stop/poll callback: false means work remains; true
allows the transport to report stopped. It must not wait for outstanding I/O
inside update. Destroy handles partial setup and a created instance that never
received an update, and drains and joins its owned operations before releasing
their storage. IOCP cancels its outstanding operations and drains their
completions before it releases them.

Clients use create, destroy, send and receive callbacks, plus four optional
ones: `send_batch`, `receive_batch`, `wait` and `wake`. A client backend with
`wait` and `wake` runs on the transport's client I/O thread, which sends and
receives off the main thread; one without them runs with the transport's
polling receive thread and per-packet sends from the main thread. Every call
borrows its buffers synchronously. Destroy runs after the transport's thread
has joined and must release any additional backend-owned work.

## Adding a backend without editing the transport

For an in-tree transport, define a `const rocketnet_driver_backend_t`, add its row
to `sdk/rocketnet_drivers.inc`, and add its source to the build. It may support one
endpoint or both. There are no transport switches, members or platform guards
to extend.

An embedding project can also supply its own registry rows and implementation:

```sh
cmake -S . -B build \
  -DROCKETNET_DRIVER_REGISTRY_HEADER=/path/to/my_transports.inc \
  -DROCKETNET_DRIVER_SOURCES=/path/to/my_transport.cpp
```

The `.inc` has `ROCKETNET_DRIVER(...)` rows in the same format as the built-in
file, and no include guard: it is intentionally expanded more than once. Native
registration is compiled into the SDK; this is not a dynamic plugin ABI.
Backend symbols are not public C exports. Per-transport settings and resource
validation belong in the factory; they do not extend the public transport configuration.

`test/driver_extension` is an executable example: an external row and backend
serve both ends of an encrypted handshake and payload exchange through only
the public transport API. `tools/check_driver_extension.py build` builds and
runs it independently. `test_driver` checks registry ordering, factory refusal,
address isolation, lifecycle and buffered I/O. The GSO and IOCP tests and the
full transport tests cover the real staged paths; hosted tests do not replace XDP
hardware failure tests or per-platform line-rate measurements.

## The RIO server transport

`rio` (`sdk/rocketnet_server_rio_windows.cpp`, `sdk/rocketnet_server_rio.h`)
is a Windows staged server transport with one registered memory region, polled
send and receive completion queues, deferred sends committed once per flush,
and two send phases so a phase stays owned until its sends complete. It uses no
worker threads. Its server rank is 45, after IOCP's 40, so automatic selection
never chooses it ahead of IOCP; pin `rio` to select it directly. Its
`ROCKETNET_RIO_COUNTER_*` counters report sent and received packets, full send
slots, failed sends, waits for a send phase, failed receives, dropped-size and
dropped-overflow receives, failed receive reposts, and RIO commit calls. Its
send phases, receive banks and destruction are modelled in
`tla/RNRioDriver.tla`.

## The io_uring server transport

`io_uring` (`sdk/rocketnet_server_uring_linux.cpp`, `sdk/rocketnet_server_uring.h`)
is a staged Linux server transport with one io_uring per server, used only on the
main thread, and no worker threads. An `SO_REUSEPORT` socket group, bound the
way the GSO transport binds its group, shards receives across queues. Each queue
has a provided buffer ring and one multishot `RECVMSG`, so the kernel fills
receive buffers with no syscall per datagram; `send_packets` submits one
batched `SENDMSG` per staged slot, in chunks no larger than the free submission
queue space, and returns once every send has completed. A queue count past
`ROCKETNET_URING_MAX_QUEUES` refuses the server start; zero selects
`ROCKETNET_URING_DEFAULT_QUEUES`.

The transport needs liburing at build time (`ROCKETNET_IO_URING`, on by default
and used when liburing is found). A build without it keeps the registry row with
no platform: the row is absent from the availability lists and a pin of
`io_uring` is skipped by the ladder. At run time `rocketnet_server_uring_available()`
probes once that the kernel accepts the ring, the opcodes and the provided
buffer ring. Its server rank is 25, after `gso` (20) and before `portable` (30)
in the Linux `auto` order; it has no client side.

Its counters (`ROCKETNET_SERVER_URING_COUNTER_*`) are packets sent and received,
submit syscalls, send drops, receive drops for size, bank overflow and
unconvertible source, receives ended for want of buffers, and multishot
re-arms. A destroy that gives up waiting for outstanding completions leaks the
kernel-visible memory rather than freeing it and counts it in
`rocketnet_server_uring_leaked_bytes()` and `rocketnet_server_uring_leaked_drivers()`.
The send staging, the two receive banks and destroy are modelled in
`tla/RNUringDriver.tla`.

## The XDP receive filter

`server_xdp/server_xdp.c` accepts both address families. IPv4 is parsed as
a 20-byte header with UDP behind it. IPv6 is parsed as the fixed 40-byte header with
UDP directly behind it. Both families then run one shared function: the
destination port and address against the config map, the wire size bounds, the
filter epoch and value, `ROCKETNET_WIRE_SERVER_PACKET_VALID`, and the redirect
into the XSKMAP for the receive queue. The counters are the same for both
families. The IPv6 destination is `public_address_ipv6` in
`rocketnet_server_xdp_config_t`; all zero accepts any destination, as a zero
`public_address` does for IPv4.

An IPv6 frame whose next header is an extension header (hop-by-hop 0, routing
43, fragment 44, destination options 60, AH 51 or ESP 50) passes to the kernel
stack through one named branch and increments
`ROCKETNET_SERVER_XDP_COUNTER_PASSED_IPV6_EXTENSION`. The program never walks
an extension chain and never drops one, so IPv6 fragments pass. Every other
non-UDP next header, ICMPv6 (neighbor discovery) above all, passes uncounted,
as non-UDP IPv4 does.

The AF_XDP transport serves IPv4 and IPv6 end to end. A server serves the
family of its public address, IPv4 or IPv6, and the ladder offers `xdp` for
either. The receive loop dispatches each frame on its ethertype through
`rocketnet_xdp_frame_receive` (`sdk/rocketnet_xdp_frame.h`): IPv4 at its
offsets, IPv6 at the 40-byte fixed header, and the from-address carries the
family it read. A frame it does not take is dropped and counted by reason:
another ethertype (`not_ip`), an IPv6 extension header (`ipv6_extension`,
never parsed at the UDP offsets), the family the server does not serve
(`other_family`), or a frame the parse refuses or too short for any server
packet (`malformed`). The flush builds each reply with the server's own
address as the source, and an IPv6 reply carries the UDP checksum computed in
software. Create and the pre-flight find the interface carrying the address
over `AF_INET` or `AF_INET6` and refuse an IPv6 link-local address (fe80::/10)
by name. The IPv6 gateway MAC comes from `ip -6 route show default dev <if>`
and `ip -6 neigh show dev <if>`, the twin of the IPv4 discovery;
`ROCKETNET_GATEWAY_ETHERNET_ADDRESS` overrides either.

Dual stack: one server owns one public address, so one family. The config map
holds the server's address in its family's slot and that family's loopback
address in the other slot (zero would accept any destination), so the program
redirects only the server's family and passes the other to the kernel. One
interface takes one XDP program, so a machine that serves both families on
one interface runs `xdp` for one family and another transport (`gso`,
`io_uring` or `portable`) for the other.

The kernel filter's counters surface on the transport counter surface as
`ROCKETNET_COUNTER_XDP_KERNEL_*`, assigned from the kernel's cumulative
per-CPU totals each time the transport counter surface is read. Only a
backend with a kernel prefilter fills them; one with none leaves them
zero.

The replay map is read-only to the kernel: it drops a session packet 16 or
more behind the high-water the userspace window sets, and never writes the
map. The transport marks each connected client after every update, and the
userspace window stays 4096.

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
memory runs out and the server is killed):

```sh
vng --run /path/to/vmlinuz --memory 1536M --cpus 2 --append "irqaffinity=0 isolcpus=1 nohz_full=1" --rwdir "$PWD" -- sudo ./bin/test_xdp_filter -v
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
UDP for both address families: IPv4 with ihl 5, DF set, ttl 64, the header
checksum and a UDP checksum of 0, and IPv6 with the
mandatory UDP checksum (RFC 8200 section 8.1) computed in software over the
pseudo-header. Hardware TX checksum offload (AF_XDP TX metadata, kernel 6.8
or later) is not used.

The filter's decisions also run as ordinary host C in the unit suite, with no
kernel, no root and no BPF syscall. The `rocketnet_xdp_filter_host` target
compiles `test/xdp_host/server_xdp_host.c`, which includes
`server_xdp/server_xdp.c` as shipped, with the BPF build's defines
(`-D__linux__ -D__BPF__ -ffreestanding`) and `test/xdp_host/` first on that
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
[standard/TRANSPORT.md](standard/TRANSPORT.md) size table, the IPv6 extension and fragment passes, and the
redirect decision per receive queue. It also covers the `config == NULL`
branch, which the kernel's one-entry array never reaches. Only the kernel
tier (`test_xdp_filter`) shows what the verifier accepts, the real helpers'
behaviour, and the real per-CPU and XSKMAP maps. The host target is built for
GCC or Clang on a little-endian, non-Windows host. Elsewhere, including the
MSVC unit suite on Windows, the cases print `SKIPPED`. When the filter adopts
a new kernel header, helper or map, the shim follows in the same change, with
the helper's kernel source cited. `xdp_host_every_counter_has_a_case` fails
when a `ROCKETNET_SERVER_XDP_COUNTER_*` has no row that moves it. To run the
host tier against another filter source, configure with
`-DROCKETNET_XDP_FILTER_HOST_SOURCE=<path>`; the default is the shipped file.

`cmake/XdpSources.cmake` is the source list for staging, archiving, CMake rebuild
dependencies and verification. Add a new runtime BPF source/header there; archive
entries use basenames, which must be unique. The checker also exercises adding
one future input to this list while unrelated files remain in staging.

Windows uses the committed `sdk/rocketnet_server_xdp.h` without requiring xxd.
Whenever a listed input changes, refresh that fallback on a development host
with xxd, then run the same decoded-content check used by package tests and
Windows CI:

```sh
cmake --build build --target rocketnet_xdp_header
if ! python3 tools/check_xdp_archive.py --committed-only; then
    cp build/generated/rocketnet_server_xdp.h sdk/rocketnet_server_xdp.h
fi
python3 tools/check_xdp_archive.py
```

`--committed-only` verifies the fallback without requiring xxd. The generated
header must contain the exact generated archive bytes; the committed fallback's
archive must contain the exact current source bytes, independently of archive
timestamps and ownership metadata. The conditional copy avoids changing the
fallback solely because archive metadata differs. CI compiles a fresh extraction
of the archive, so a removed input cannot be supplied by old staging files.
Neither check loads a program or attaches to a network interface.

The Linux connection-type label is an IPv4 main-table heuristic. It does not
model policy routing or multipath entries without a direct output-interface
attribute, and may report the wired fallback for those configurations. This
label does not choose the transport route or limit IPv6 endpoint support.
