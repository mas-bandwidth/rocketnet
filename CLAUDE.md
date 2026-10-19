# rocketnet — working notes

## What this is

RocketNet is a merger of the game's transport bits and zero copy API with a
netcode-like connection protocol and security: a packet level transport
library worthy of a **1000 player (or above)** game. The game's
transport goodness, productized for its first customer, and once done
re-integrated into the game as its foundation. The requirements are
locked in [REQUIREMENTS.md](REQUIREMENTS.md); scope questions argue against
that file.

**Messages are opaque byte buffers.** rocketnet does not own serialization. That is
the central design decision and it shapes everything: at 1000 players, trading
a little bandwidth for simpler code and less CPU is the right side of the
trade, so bit-packing moved *above* rocketnet (the game fills buffers with a
schema compiler) and rocketnet ships bytes. There is not one bit-packing routine on
the send or receive path.

The same line holds inside the connection layer, ruled 2026-08-26: **protocol
packets are byte aligned and written that way** — plain little-endian
read/write helpers against the standard's packed layouts, no serialize, no
schema. The wire format has no sub-byte fields, so a bitpacker buys nothing;
the security core carries minimum dependencies; and the assembled wire vectors
in the standard diff against the implementation with a straight memcmp.
serialize and schema remain available above the line, in payloads, where they
already earn their keep.

## The layering

    libsodium            pinned two-file vendor (sodium/sodium.c, sodium/sodium.h)
    leaf utilities       hash, base64, address, trackers
    rocketnet_platform        threads, mutexes, time, sockets — the only system headers
    wire format          constants, packets, tokens, packet filter
    the two transports   client_transport, and server_transport over
                         runtime-selected drivers (portable, xdp, gso, msgx, iocp)

`rocketnet.{h,cpp}` sits beside rather than above: process init, assert/log/allocator
hooks, RNG.

**The strongest structural property, worth protecting:** the two server
implementations satisfy one header with **zero `#ifdef` at the call site** —
the game's `network_server.cpp` is byte-identical whether it builds against
XDP or portable. Any third implementation drops into that same seam.

## What rocketnet owns, and what it does not

**Owns:** socket lifecycle and tuning, threads/time, `rocketnet_address_t`, the
6-byte `{type, epoch, filter value}` prefilter header, packet buffering and batching, the
whole XDP dataplane including Ethernet/IP/UDP synthesis, crypto primitives —
and, on the connection protocol, the general acks: every session packet's
sealed plaintext opens with an ack block, and the transport surfaces the
acknowledged packet ids per update. Delivery notification is the primitive;
the transport informs and never resends. Fragmentation and reassembly are the
transport's own self-contained mechanism: a payload requested above the
wire's capacity at start_packet splits into type 9 fragments at finish and
reassembles per connection on the far side, unreliable semantics standing —
a lost fragment loses the whole payload, counted, never resent.

**Leaves to the caller:** serialization and bit packing (absolutely — this is
the design), reliability policy on top of the notifications, sequencing,
payload encryption above the seal, DNS, and on the
direct path all connection semantics.
`rocketnet_server_socket_t` has no client table; `ROCKETNET_MAX_CLIENTS` is only an
array-sizing constant.

## Current state: the secure connect is landed

The transport tier is the library: netcode-style connect tokens on public
key crypto (no secret shared between backend and servers), the
address-validated two-RTT handshake, per-connection derived keys, sealed
packets opening with the general ack block, fragmentation, and server-side
client slots — `client_transport` and `server_transport` over the runtime
driver ladder (portable everywhere; xdp and gso on linux; msgx ahead of
portable on macOS; iocp behind portable on windows). `client.c` and
`server.c` connect out of the box under the published dev keys.

What remains is REQUIREMENTS.md's tail: reliability, flows and broadcast
above the transport. docs/roadmap.sexp is the live ordering (ROADMAP.md renders it).

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

1. **The umem fill-ring refill path's edges are unverified**: the common
   release/refill accounting has run live (the test-box bring-up echoed
   1800 payloads through the AF_XDP rings), but the pending-return spill —
   taken when the fill ring cannot absorb a full drain's frames — has not
   been driven on hardware. The worker now shares a tested all-or-zero pending
   frame helper: repeated zero reservations retain every frame and full success
   submits each exactly once. That model is not a hardware saturation result.

2. **XDP construction validation remains incomplete:** queues are now value-
   constructed before resource acquisition, with explicitly initialized atomics.
   Portable allocation-failure tests exercise the shared allocator and transport
   cleanup. Privileged XDP syscall-failure coverage still requires its own lab;
   construction fixes do not establish a tested committing dataplane.

Refuted on inspection, recorded so nobody re-hunts it:

- The fill ring SETUP is sound: 2048 slots reserved, 2048 valid addresses from
  an initialized 32768-entry pool. The unverified part is the per-poll refill's
  spill edge, above.

## Verification ladder

Correct, then fast, then durable. Three instruments, three disjoint defect
classes — none substitutes for another, and the library has not shipped until
all three pass:

1. **Unit and functional tests** prove correctness. Every layer exhaustively,
   simplest to most complex; negative tests assert the specific reason counter
   AND the packet type's arrival counter, so "rejected" always means "rejected
   here"; known-answer vectors (standard/TRANSPORT.md Appendix A) prove interop.
   On success a test prints its name and nothing else — measurements and
   diagnostics belong to failures and to the profiling instruments, never to
   a passing run's output.
2. **Load profiling** proves performance, and without it the library does not
   work — performance is the spec ("fastest correct", 1000+ players, not IO
   bound). Real shape: ~1000 real connections at game-like rates and sizes
   with connect/disconnect churn, not ten connections at line rate. Baselines
   (packets/sec/core, p99 processing time) are recorded and diffed per change
   like tests: a perf regression goes red, it does not drift. This is the
   instrument that sees what tests cannot: false sharing, lock convoy,
   allocation storms, syscall costs. Truth lives on Linux on real hardware;
   the mac is the edit loop.
3. **Soak at load** proves durability — hours to days, and it is what finds
   the difficult problems, because duration converts probability into
   certainty: leaks, table exhaustion under churn, timer and key-rotation
   edges, and the one-in-a-billion interleavings that become routine at
   billions of events per hour. The harness asserts counters *while running* —
   must-stay-zero counters stay zero, received == sum of outcomes per type —
   so "fast" can never be achieved by silently dropping, and a days-long run
   stays diagnosable with no logs.

This requires a **synthetic traffic generator as part of step 1's
deliverable**, not later tooling: a swarm client built on the real library
(real handshakes, real crypto, churn schedules, a percentage abandoning
mid-handshake so the half-open reclaim soaks under pressure), plus a garbage
generator so the reject paths soak too — a polite-traffic-only soak is half a
soak. The swarm is the library exercising itself and lives with the tests in
C++; the orchestration around it — spin-up, counter collection, baseline
diffing — is tooling and therefore Go.

Two cadences: a short smoke load per change; the long soak scheduled on a
Linux box.

**The discipline behind all three: ALWAYS BE LOADED, and no rare codepaths.**
The game server does the work of 1000 players all the time — snapshot
deltas, packet construction, everything — even with one client connected, so
the cost profile is flat, the 999th client cannot surprise, and any
scale-dependent bug is hit trivially by booting the server and connecting one
real client. For this library the same discipline lands two ways. Where
load-independent cost is cheap (fixed tables, full-table sweeps), prefer
constant shape over clever load-proportional work. Everywhere else, the rule
is: **if a codepath is rare, make it not rare — something always gone through,
even if redundant.** Instances already in the design: the cookie exchange is
always-on for every connection, so the under-attack path is the normal path;
dev runs the full ship handshake under published test keys, so the laptop path
is the production path; every disconnect sends the full redundant burst.
Standing consequences: cookie key rotation runs always at its 60 s period so
the previous-key acceptance window is exercised every minute of every session;
and the soak runs long enough that a connection crosses 2^32 packets, so
sequence reconstruction's wrap region is routine rather than theoretical. What
cannot be made non-rare by design (real wire I/O, per-packet crypto at scale)
is made non-rare by posture: the swarm supplies load in every dev loop, so the
library is never observed unloaded.

## The non-root server implementation: the GSO driver

For hosts where XDP is not available — AWS GameLift and similar, no
`CAP_NET_ADMIN` — the third server implementation exists (#97):
`rocketnet_server_gso_linux.cpp` behind the same seam, Linux only,
unprivileged. Since #106 selection is runtime, not compile time: one Linux
build carries xdp, gso and portable, and the transport walks the config's
ordered candidate list at create (`driver` field / `ROCKETNET_DRIVER`,
default `auto` = xdp,gso,portable) — first grant wins, pins refuse loudly,
the granted tier is reported by `rocketnet_server_transport_driver` and one
create log line. The config system is one X-macro table per transport in
`rocketnet.h` deriving struct, defaults, env names (`ROCKETNET_` +
uppercased field), parse and validation; precedence default < struct < env,
overrides logged with the displaced value, validation refuses and never
clamps. standard/TRANSPORT.md "Driver selection and transport configuration" is the
ruling form.

The shape is the one this section always planned, confirmed by the #81
measurements (GSO/GRO 11.62x over portable standalone on the test box's
loopback; io_uring 1.13x, not worth its complexity): an `SO_REUSEPORT` group
with one socket per worker thread, kernel 4-tuple hash sharding receives with
zero shared state between queues, `recvmmsg`/`sendmmsg` batching, and
`UDP_SEGMENT`/`UDP_GRO` so a batch traverses the kernel's UDP path once.
Transmit is main-thread (the phase contract), rotated across the group's
sockets for buffer headroom — the XDP path's per-queue sender threads are
not reproduced, deliberately. The
driver probes every feature at create and degrades to plain mmsg batching
where a kernel says no; a live GSO refusal mid-flush flips the tier and
resends plain, lossless. It recovers the syscall win and the single-traversal
win. It does not recover skb-allocation avoidance — worth knowing when sizing
instances, because it is a solid middle tier rather than parity, and it never
advertises that scale.

## Crypto

Ruled 2026-08-26: **sodium** (see REQUIREMENTS.md). Hydrogen was evaluated
and lost on crypto pedigree, backend interop, per-packet overhead (36 vs 16
bytes), the missing associated data parameter — and then decisively on the
bench: at 1000 players x 60 Hz its packet crypto alone exceeds one core
where sodium uses about a fifth, scalar versus scalar on arm64.

The switch is complete: every crypto call in the tree is sodium, from the
pinned two-file vendor (`sodium/sodium.c` and `sodium/sodium.h`) in the shape
netcode and yojimbo already carry, extended with the Ed25519 and sealed box
families the public key token design needs. An independently supplied
libsodium is an opt-in integration test, not the default.

[Hydrogen](https://github.com/mas-bandwidth/hydrogen) and
[proton](https://github.com/mas-bandwidth/proton) remain the kernel/XDP
experiment track in their own repos, maintained by the open source sweep;
nothing here vendors them.

## Vendoring

The game vendors this whole SDK into `game/ThirdParty/rocketnet/`,
byte-for-byte. Fix here, then re-vendor there — and note the game's build
compiles vendored third-party with `-O3 -DNDEBUG` even in Debug, deliberately,
so the game can be debugged without debugging its dependencies.
