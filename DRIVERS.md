# Native transport drivers

The public transport API in `rocketnet.h` is independent of the backend. C#
uses that API, and will use the future client/server API above it. Native
drivers implement `rocketnet_driver.h`; they do not add managed bindings or
wire-format rules.

`rocketnet_drivers.inc` is the built-in registry. Each row supplies a name,
bound-port discovery property, backend record, platform mask, and server/client
preference ranks. Lower nonzero ranks come first in `auto`; zero means that
endpoint is unsupported. IDs, names, diagnostics and platform availability are
derived from these rows. Existing IDs and preference orders are preserved.

The backend record points to immutable server and/or client operations. The
transport retains the selected operations and opaque instance for its lifetime.
Factories, resources and platform decisions stay in the driver layer. The
transport branches on generic capabilities, never a specific backend identity.

## Server capabilities and ownership

Two I/O shapes preserve the existing implementations:

- **Buffered:** the transport supplies its common control, payload and receive
  buffers. `send_batch` and `receive_batch` borrow that storage only for the call.
  This is the portable/msgx shape. It does not require a backend-owned pool.
- **Staged:** the driver supplies frame storage and a complete receive batch.
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
| `STAGED` | Use driver-owned frames and receive views instead of borrowed transport buffers. |
| `FLUSH_ON_UPDATE` | Flush staged control traffic at the end of update. Buffered control traffic completes by the end of update; full batches may flush during the update walk. |
| `FINISH_FINAL` | A finished frame cannot be withdrawn. Otherwise abort may withdraw it before flush. Unfinished reservations can always be aborted. |
| `BINDS_PUBLIC_ADDRESS` | The factory binds the advertised address rather than the transport's ordinary bind address. |

A flush means submission, not necessarily completion or reusable storage. IOCP
keeps operation buffers until completion retirement. The backend owns that
decision; it must never hand a still-owned frame to another producer.

Start/finish/abort may run on concurrent fill threads, with one producer per
frame. Join them before update, receive, flush, stop or destroy. Flush is called
once for a staging generation; the backend retains ownership until it can
safely reuse that storage. None
of these rules sets a frame, packet-send or ACK rate.

## Creation and shutdown

The factory borrows resolved configuration for the call and retains only what
its instance needs. It owns partial-construction cleanup. A failed candidate's
tentative bind address cannot constrain the next candidate. A discovering
factory reports its actual port on success. Existing XDP preflight and the
commit barrier remain in its adapter/backend.

Server stop is an idempotent stop/poll callback: false means work remains; true
allows the transport to report stopped. It must not wait for outstanding I/O
inside update. Destroy handles partial setup and a created instance that never
received an update, and drains/joins its owned operations before releasing
their storage. GSO/XDP preserve their existing stop/destruction behavior; IOCP
uses its cancellation and completion-drain implementation.

Clients use create/destroy/send/receive callbacks. The existing transport
receive thread and buffers remain shared infrastructure. Receive can run
concurrently with sends; both calls borrow their buffers synchronously. Destroy
runs after that thread has joined and must release any additional backend-owned
work. Client fast-path I/O is not required by this interface.

## Adding a backend without editing the transport

For an in-tree driver, define a `const rocketnet_driver_backend_t`, add its row
to `rocketnet_drivers.inc`, and add its source to the build. It may support one
endpoint or both. There are no transport switches, members or platform guards
to extend.

An embedding project can also supply its own registry rows and implementation:

```sh
cmake -S . -B build \
  -DROCKETNET_DRIVER_REGISTRY_HEADER=/path/to/my_drivers.inc \
  -DROCKETNET_DRIVER_SOURCES=/path/to/my_driver.cpp
```

The `.inc` has `ROCKETNET_DRIVER(...)` rows in the same format as the built-in
file, and no include guard: it is intentionally expanded more than once. Native
registration is compiled into the SDK; this is not a dynamic plugin ABI.
Backend symbols are not public C exports. Per-driver settings and resource
validation belong in the factory; the public transport configuration is unchanged.

`test/driver_extension` is an executable example: an external row and backend
serve both ends of an encrypted handshake and payload exchange through only
the public transport API. `tools/check_driver_extension.py build` builds and
runs it independently. `test_driver` checks registry ordering, factory refusal,
address isolation, lifecycle and buffered I/O. Existing GSO/IOCP and complete
transport tests cover the real staged paths; hosted tests do not replace XDP
hardware failure tests or per-platform line-rate measurements.

Dispatch measurements and their limits are recorded in
[bench/transport_fill.md](bench/transport_fill.md).

## The XDP receive filter

`server_xdp/server_xdp.c` accepts both address families. IPv4 is parsed as a
20-byte header with UDP behind it. IPv6 is parsed as the fixed 40-byte header
with UDP directly behind it. Both families then run one shared function: the
destination port and address against the config map, then the userspace
refusal ladder in its order (the prefilter's too short, too large, wrong epoch
and wrong value; then a reserved type, a server-to-client type, and the type's
wire size by `ROCKETNET_WIRE_SERVER_PACKET_VALID`), and the redirect into the
XSKMAP for the receive queue. The counters are the same for both families. The IPv6 destination is `public_address_ipv6` in
`rocketnet_server_xdp_config_t`; all zero accepts any destination, as a zero
`public_address` does for IPv4.

An IPv6 frame whose next header is an extension header (hop-by-hop 0, routing
43, fragment 44, destination options 60, AH 51 or ESP 50) passes to the kernel
stack through one named branch and increments
`ROCKETNET_SERVER_XDP_COUNTER_PASSED_IPV6_EXTENSION`. The program never walks
an extension chain and never drops one, so IPv6 fragments pass. Every other
non-UDP next header, ICMPv6 (neighbor discovery) above all, passes uncounted,
as non-UDP IPv4 does.

Every verdict is counted. `sdk/rocketnet_wire.h` defines the counters
(`ROCKETNET_SERVER_XDP_COUNTER_*`) and `ROCKETNET_SERVER_XDP_COUNTER_TABLE`,
one row per counter with its name and class. The program looks up the per-CPU
stats entry once per frame and raises `RECEIVED` on entry; every exit then
raises exactly one more counter, so

    RECEIVED == REDIRECTED + every DROPPED_* + every prefilter and type counter + every PASSED_*

holds by construction. The exits and their counters:

| exit | verdict | counter |
|---|---|---|
| frame shorter than an Ethernet header | pass | `PASSED_SHORT_ETHERNET` |
| ethertype neither IPv4 nor IPv6 | pass | `PASSED_NOT_IP` |
| IPv4 header truncated | drop | `DROPPED_IPV4_SHORT_HEADER` |
| IPv4 header with options, or protocol not UDP | pass | `PASSED_IPV4_NOT_PLAIN_UDP` |
| IPv6 header truncated | drop | `DROPPED_IPV6_SHORT_HEADER` |
| IPv6 extension header | pass | `PASSED_IPV6_EXTENSION` |
| IPv6 next header neither UDP nor an extension | pass | `PASSED_IPV6_NOT_UDP` |
| UDP header truncated | pass | `PASSED_UDP_SHORT_HEADER` |
| filter not configured | pass | `PASSED_NOT_CONFIGURED` |
| another destination port or address | pass | `PASSED_OTHER_DESTINATION` |
| packet shorter than the 6-byte prefilter header | drop | `PREFILTER_TOO_SHORT` |
| packet above the receive ceiling | drop | `PREFILTER_TOO_LARGE` |
| filter epoch not the configured one | drop | `PREFILTER_WRONG_EPOCH` |
| filter value not the configured one | drop | `PREFILTER_WRONG_VALUE` |
| type above 9 | drop | `RESERVED_TYPE` |
| denied, challenge or cookie | drop | `WRONG_DIRECTION` |
| server-inbound type outside its wire size | drop | `<TYPE>_WRONG_SIZE` |
| redirect into the queue's socket | redirect | `REDIRECTED` |
| no socket for the queue | drop | `DROPPED_REDIRECT_NO_SOCKET` |

The prefilter and type counters carry the names of the userspace counters
for the same refusal (STANDARD.md, the prefilter's four reasons), so the
kernel filter and the userspace ladder report in the same shape.

`test_xdp_filter` judges the program with the kernel. It loads the object
through the verifier and runs crafted frames with `BPF_PROG_TEST_RUN`, which
returns the program's verdict and transmits nothing. It swaps the XSKMAP for a
DEVMAP before load so a redirect can succeed without an AF_XDP socket, and it
reads the stats map back after each frame: each frame raises `RECEIVED` and
exactly one other counter. It sweeps every type at the boundary sizes against
the userspace ladder's counter names, and with `--base` it runs the same sweep
through an object built from an earlier `server_xdp.c` and requires the same
verdict for every frame. It needs root, or CAP_BPF and CAP_NET_ADMIN; `-v`
prints the verifier's last log line:

```sh
sudo ./bin/test_xdp_filter -v --base base_server_xdp.o
```

## The XDP driver's address family

The xdp driver serves IPv4 or IPv6, one family per server. A server owns one
public address, and the family of that address is the family it serves; dual
stack on one AF_XDP socket set is not served, so a deployment that wants both
families runs one server per family.

- Pre-flight and create accept an IPv4 or an IPv6 public address. The
  interface is the UP interface carrying it (`getifaddrs`, `AF_INET` or
  `AF_INET6`). An IPv6 link-local address (`fe80::/10`) is refused by name: a
  server's public address is global.
- The gateway MAC comes from `ip neigh` for the IPv4 default gateway, and for
  IPv6 from the next hop of `ip -6 route show default dev <if>` looked up in
  `ip -6 neigh show dev <if>`. `ROCKETNET_GATEWAY_ETHERNET_ADDRESS` overrides
  both.
- The config map carries the server's own address in its family's slot. The
  other family's slot holds that family's loopback address (`127.0.0.1` or
  `::1`), not zero, because zero accepts any destination. No NIC delivers a
  loopback destination legitimately, so the program redirects only the
  server's family and passes the other.
- Receive strips headers through the frame codec's parse and fills the
  from-address in the family it read. Send builds each frame through the codec
  from the server's own address. A destination of the other family is refused
  and counted, never sent malformed.
- `rocketnet_server_socket_driver_counters` counts what userspace handed up
  and sent, and every frame the program redirected that userspace refused:
  `receive_refused_parse`, `receive_refused_family`, `send_refused_family`,
  `send_refused_build`. A redirect the driver cannot use is counted and named.

The functional cases `xdp_max_size_both_ways_ipv4` and
`xdp_max_size_both_ways_ipv6` prove each family on the wire. They build a veth
pair at MTU 1500 between two network namespaces, pin the server to the xdp
driver, run a portable client, and send a maximum-size payload 20 times each
way. Then they read the stats map, the driver counters and the transport
counters, and print one `tcpdump -vv` line per direction from a capture on the
client end. They change the machine, so they run only as root with
`ROCKETNET_TEST_XDP_VETH=1` on an XDP-provisioned machine, and print a named
skip otherwise. The two cases share their namespaces, so they run one at a
time: a case name given to `./bin/functional` (bare, or after `--run`) runs
that one case and nothing else, and an unknown name exits 2 by name.

```sh
sudo ROCKETNET_TEST_XDP_VETH=1 ROCKETNET_TEST_XDP_PCAP_DIR=/path/to/keep ./bin/functional --run xdp_max_size_both_ways_ipv4
sudo ROCKETNET_TEST_XDP_VETH=1 ROCKETNET_TEST_XDP_PCAP_DIR=/path/to/keep ./bin/functional --run xdp_max_size_both_ways_ipv6
```

The library pre-flight recognises a veth by its kernel driver name (`veth`,
read with `ETHTOOL_GDRVINFO`) and waives R5 and R7 for it: a veth has no NAPI
busy-poll pair to tune and reports no combined channel count. Every other
driver is held to both unchanged; `xdp_preflight_veth_waiver` checks the
verdicts. `scripts/rocketnet-provision veth-s 1 verify` does not waive them,
so on a veth its R5 and R7 lines print as unmet; the cases keep that output
for the record and gate on the library pre-flight.

## Embedded XDP source maintenance

The XDP frame codec (`sdk/rocketnet_xdp_frame.h`) synthesises Ethernet + IP +
UDP for both address families: IPv4 byte-identical to the historical builder
(ihl 5, DF set, ttl 64, header checksum, UDP checksum 0) and IPv6 with the
mandatory UDP checksum (RFC 8200 section 8.1) computed in software over the
pseudo-header. Hardware TX checksum offload (AF_XDP TX metadata, kernel 6.8
or later) is a follow-up, not yet used.

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
