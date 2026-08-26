# rocketnet — working notes

## What this is

RocketNet is a merger of flow's good transport bits and zero copy API with a
netcode-like connection protocol and security: a packet level transport
library worthy of a **1000 player (or above)** game. The space game's
transport goodness, productized for Dean Hall (RocketWerkz), and once done
re-integrated into the space game as its foundation. The requirements are
locked in [REQUIREMENTS.md](REQUIREMENTS.md); scope questions argue against
that file.

**Messages are opaque byte buffers.** rocketnet does not own serialization. That is
the central design decision and it shapes everything: at 1000 players, trading
a little bandwidth for simpler code and less CPU is the right side of the
trade, so bit-packing moved *above* rocketnet (the space game fills buffers with a
schema compiler) and rocketnet ships bytes. There is not one bit-packing routine on
the send or receive path.

## The layering

    rocketnet_hydrogen        vendored libhydrogen (~24% of the source)
    leaf utilities       hash, base64, address, trackers
    rocketnet_platform        threads, mutexes, time, sockets — the only system headers
    wire format          constants, packets, tokens, packet filter
    the two sockets      client, and server (XDP or portable)

`rocketnet.{h,cpp}` sits beside rather than above: process init, assert/log/allocator
hooks, RNG.

**The strongest structural property, worth protecting:** the two server
implementations satisfy one header with **zero `#ifdef` at the call site** —
the game's `network_server.cpp` is byte-identical whether it builds against
XDP or portable. Any third implementation drops into that same seam.

## What rocketnet owns, and what it does not

**Owns:** socket lifecycle and tuning, threads/time, `rocketnet_address_t`, the
18-byte `{type, pittle, chonkle}` prefix, packet buffering and batching, the
whole XDP dataplane including Ethernet/IP/UDP synthesis, crypto primitives.

**Leaves to the caller:** serialization and bit packing (absolutely — this is
the design), reliability, acks, sequencing, fragmentation, payload encryption,
handshake, keepalives, DNS, and on the direct path all connection semantics.
`rocketnet_server_socket_t` has no client table; `ROCKETNET_MAX_CLIENTS` is only an
array-sizing constant.

## Current state: direct mode only, by ruling

Direct mode is the library: a client connects to a server address and
exchanges raw packets. The network next era machinery is gone; flow's repo
preserves all of it. What comes next is REQUIREMENTS.md's order of
operations: the minimal secure connect — netcode-style connect tokens on
public key crypto, no secret shared between backend and servers, hydrogen if
possible — then platform transports behind an easy-to-extend seam, then
reliability, flows and broadcast at the end.

Practical consequence today: a direct client is `CONNECTED` at create time
before a byte moves, and the server has no notion of a client at all. That is
the security hole issue #6 exists to fix: a netcode-style two-RTT handshake,
per-session keys, sealed packets, and a server-side client table are the next
construction.

## The scaling story, in one paragraph

The win is not mainly syscall elimination, though that is real (~0.12–0.24
core-seconds/second reclaimed at 60 kpps each way). The larger win is that RSS
fans RX across N queues into N pinned `SCHED_RR` threads with **zero shared
state between queues** — no cross-queue locks, no shared frame pool — while TX
shards by `packet_id % num_queues` so any game thread can produce into any
queue with a single `fetch_add`. What kills a 1000-player single-socket UDP
server is the per-socket receive-queue lock and the skb allocator; both are
gone. The buffer handoff is correct: the XDP thread holds `receive_mutex`
across its whole fill loop, so the game's counter flip lands only between
batches and the game then drains the *other* buffer lock-free — one mutex
acquisition per tick per queue.

"Zero copy" in the comments means game↔rocketnet. There is exactly one `memcpy`
each way in the XDP path, which is the right trade: it keeps umem frame
lifetime out of the game entirely, for ~85 MB/s of copying at target load.

## Known issues, ranked

Still open, highest first:

1. **`magic` is all zeros at every call site**, so chonkle is unkeyed: the
   filter proves a packet has rocketnet's *shape*, not that it came from someone
   holding a key.

   Note the kernel-side filter **is live in this repo's XDP program.** It is
   commented out in space game's vendored copy, where the AF_XDP program is not
   running at all — that project is in development, not production, and cut the
   XDP path down. Worth knowing before anyone "re-enables" something that is
   already on.
2. **Startup side effects, output and exit status discarded**: several `popen`
   calls, and library init disables SMT machine-wide.

3. **The umem fill-ring refill path is unverified on Linux**: the
   `num_consumed` accounting that drives release, refill and the
   pending-return spill has never been run for real, because the file is
   `#if ROCKETNET_XDP` and does not compile on macOS.

Refuted on inspection, recorded so nobody re-hunts them:

- The 18-byte runt packet does NOT give an attacker a coin-flip on dispatch.
  Every construction attempt is blocked by an existing check: the filter's
  18-byte minimum, then `packet_bytes < ROCKETNET_HEADER_BYTES` in
  `process_direct_packet`. A 1-byte packet reads only `packet_data[0]`, which
  that packet wrote.
- The fill ring SETUP is sound: 2048 slots reserved, 2048 valid addresses from
  an initialized 32768-entry pool. The defect was in the per-poll refill, above.

5. **Lower:** `destroy()` walks uninitialized pointers on `create()` error
   paths; the per-queue socket array is never zeroed and works only because a
   ~271 MiB `malloc` lands in `mmap` (a glibc detail, not a guarantee); the XDP
   port literal is pre-swapped but named as host order, so it is correct by
   accident and will silently blackhole the day someone "updates" it.

## Planned: a non-root server implementation

For hosts where XDP is not available — AWS GameLift and similar, no
`CAP_NET_ADMIN` — a third server implementation alongside the portable and XDP
ones. The abstraction already supports it: one header, zero `#ifdef` at the
call site.

The right primitive is `recvmmsg`/`sendmmsg` with `SO_REUSEPORT` sharding: one
socket per worker thread, kernel hashes flows across them, which preserves the
per-queue-no-shared-state property that makes the XDP path scale and amortizes
syscalls 32–64 packets at a time. It recovers most of the syscall win. It does
not recover skb-allocation avoidance — worth knowing when sizing instances,
because it is a solid middle tier rather than parity.

## Crypto

Ruled 2026-08-26: **sodium** (see REQUIREMENTS.md). Hydrogen was evaluated
and lost on crypto pedigree, backend interop, per-packet overhead (36 vs 16
bytes), the missing associated data parameter — and then decisively on the
bench: at 1000 players x 60 Hz its packet crypto alone exceeds one core
where sodium uses about a fifth, scalar versus scalar on arm64.

Interim, so construction moves quickly: brew libsodium as is on the dev box.
Before anything ships: a gutted two-file vendor in the shape netcode and
yojimbo already carry, extended with the Ed25519 and sealed box families the
public key token design needs.

`rocketnet_hydrogen.cpp` is [mas-bandwidth/hydrogen](https://github.com/mas-bandwidth/hydrogen),
vendored byte-for-byte; it stays until the sodium construction replaces it.
Patch upstream and re-vendor; do not edit it here. Hydrogen and
[proton](https://github.com/mas-bandwidth/proton) remain the kernel/XDP
experiment track in their own repos, maintained by the open source sweep.

## Vendoring

The space game vendors this whole SDK into `game/ThirdParty/rocketnet/`,
byte-for-byte. Fix here, then re-vendor there — and note the game's build
compiles vendored third-party with `-O3 -DNDEBUG` even in Debug, deliberately,
so the game can be debugged without debugging its dependencies.
