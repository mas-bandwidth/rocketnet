# TODO

Step 1 of REQUIREMENTS.md: the minimal secure connect, built bottom up per
STANDARD.md's build order. TDD, expand/contract, one rung at a time on a
proven foundation, clip in at every green. Lines move up into DONE as they
land. The milestone view is issue #28; this file is the live ordering.

## DONE

- The connection standard merged: two cold reads, verdict yes, vectors and
  assembled packets frozen (#26). 1.0 endpoints are IP only (#27). Packets
  byte aligned, written plainly (#29).
- Read/write helpers: pointer-advancing, shift-and-mask little-endian,
  endian independent by construction; tested including exact Appendix A byte
  patterns.
- Sodium in the build: pkg-config on both platforms, CI deps for both legs.
- Crypto expectations: every primitive pinned against real libsodium — sizes,
  sign/verify with refusals, sealed box with the anonymous-box hazard, AEAD
  with AD/nonce/truncation negatives, generichash outlen binding, small-order
  scalarmult refusal, and the four KDF outputs + cookie MAC + all six sealed
  packet vectors reproducing Appendix A byte for byte.
- Test vector data: checked-in arrays generated from the vectors harness
  (a generator parsing bug produced truncated key arrays; caught by the first
  real crypto call, regenerated at full length — never transcribe by hand,
  and never trust a generator without a consumer).
- Connect token: layouts at the exact offsets (locked against the frozen
  token's own bytes), mint, verify-then-open in the standard's order, inner
  consistency, token identifier. The frozen token verifies, opens and
  recovers every published field; refusals proven per check including the
  anonymous-box hazard and a hostile-backend inner mismatch.
- Prefilter header: rocketnet_protocol.{h,cpp} started; write and structural
  check as flat early-return guards, locked to the assembled vectors' first
  bytes; every refusal proven at every length and tag byte.
- Packets: the AD builder, sealed write/open, request and cookie writers,
  and sequence reconstruction with the tie rule; every per-type vector and
  both assembled vectors matched byte for byte; refusals per input including
  zero sequence, type-swap via AD, and truncation at every cut.
- Cookie: the exact input encoding and mac locked to Appendix A; validate as
  an early-return gate with constant-time compare; refused when zero,
  expired, from another address or port, for another salt, extended, or
  tampered; rotation accepts the previous key for one period; ipv6 encoding
  proven little-endian.
- Per-connection keys: rocketnet_keys as its own aspect; both sides derive
  the vector keys from their own halves and agree; handshake-only derivation
  for the pre-ephemeral client; small-order refusal fatal to the whole
  derivation; secrets zeroed before return.
- Counters framework: the request-path and mapping-lifecycle names, every
  one named, indices the implementation's contract, monotonic. Landed just
  ahead of the ladder that increments them.
- Token tables: used-token history and admitted-token set as one
  data-oriented structure — parallel fixed arrays, hot identifiers split
  from cold fields, chained buckets under a per-run SipHash key, free list,
  unlink/relink so every entry is findable from its own bucket. First
  open-addressing attempt broke exactly there (evicted-in-place entries
  landed off their own probe chain, caught red by the tests) and was
  restructured, not patched. History: bind-to-first-address, oldest-first
  eviction, expiry-as-absence, the flush shape demonstrated. Admitted set:
  never evicts unexpired, fails closed, idempotent add, expiry reclaim.
- Encryption map: the mapping aspect in the same array shape — hot address
  array walked by find under a per-run SipHash of the canonical address
  bytes (extracted from the cookie encoder for both users), cold mapping
  payload by the same index. Never evicts; full refuses. Remove zeroes key
  material, proven byte by byte. Sweep enforces the two deadlines with their
  counters: handshake deadline never refreshed, idle refreshed by traffic,
  established ignores the handshake deadline.
- Server request path: the full ladder as flat early-return functions (the
  post-open half its own function, the scrub in one place, zero gotos), a
  debug log at every counter site, the slot view as hooks. Tests: the happy
  path completes the client's whole side from wire bytes alone and matches
  the mapping's keys; byte-identical resend; every refusal asserts its exact
  reason + arrival with all other counters untouched; the no-slots denied
  opens client-side; mapping-table-full counts reason + rollup; a
  cross-address replay refused after one hash. Negative control run: broke
  the expire check, suite went red, restored. Spec's ladder gained the
  zero-timeout bullet and the counter table matched to implementation order.
  COLD READER round (probed the built library): confirmed the order exactly
  the spec's and the security properties hold; found the derive buffer
  leaving the shared secret on the stack (scrubbed) and the mapping
  discarding the client ephemeral the type-3 AD needs — a spec omission the
  code inherited; mapping now retains it, spec bullet and rationale fixed.
  Its probes folded as tests: multi-fault ordering, both slot hooks, sealed
  mismatch, seal-open failure, ipv6 advertised, clamp, negative timeout,
  previous cookie key, garbage-body resend, nonce freshness. Cookie gate
  reordered to spec (zero, mac, expiry) so forgeries never hide under
  expired; the duplicate cookie validator deleted — one implementation,
  tested where it runs; three silent returns now count INTERNAL_ERROR so
  received == sum of outcomes holds everywhere.
- Replay protection and client slots: replay check separate from advance so
  only opened packets mark the window, ring-lap inheritance proven
  impossible, below-window refused as too old; slots as parallel arrays
  with a constant-time client-id map, lowest-free assignment, lifecycle
  logs, identity zeroed on release; rocketnet_free NULL-safe centrally.
- Server receive path: one entry for every packet — prefilter, reserved and
  wrong-direction refusals, the request ladder, then the sealed types
  through mapping lookup, phase-keyed sequence reconstruction, replay
  (session phase only), open, and per-type size proofs including the
  response's required zero padding. Response processing assigns the slot,
  consumes the token in the admitted set (slots checked FIRST, so a denied
  client's token survives — proven by denying a full server then completing
  the same response after a slot frees), establishes the mapping, and
  answers the first keep-alive. Keep-alives confirm; payloads deliver with
  the client index; disconnects release everything, never answered. The
  interrupted process's admitted-add-before-slots order was a token-burning
  bug, fixed before tests. Spec gained the two response bullets defining
  the add placement. 63 tests green.
- The client state machine, and the first full connect: monotonic climb
  1-2-3-4 entering each state once, per-type processing as early-return
  functions with the state gate first, temporaries committed only after the
  open, the ephemeral secret erased at commit, the response stored and
  retransmitted byte-identical, one sending rule at the send rate, the
  -(N+1) deadline pairing plus the total connect deadline, the redundant
  disconnect burst, state transitions logged at info in netcode's voice.
  Server gained keep-alive generation and payload send. PROVEN IN MEMORY:
  client and server pumped against each other -- the whole handshake, then
  payloads both ways, then the disconnect burst leaving both sides clean;
  every handshake packet type dropped once and the connect still completes;
  a full server denies cleanly; all five timeout pairings fire to their
  exact states; the wrong-salt cookie injection refused with nothing
  stored. 69 tests green.
- Server pacing and the sweep at the server level: rocketnet_server_update
  owns the sweep (the map's sweep could not release slots — a dangling-slot
  bug waiting; replaced by a map iterator, the decision made where the slots
  live, CLIENT_TIMED_OUT counted); keep-alives paced per client and
  suppressed by ordinary traffic — every send stamps the mapping, if_due
  emits only when idle; time passed in everywhere, nothing reads a clock.
- Connected traffic complete: keep-alives both ways with suppression proven
  on both sides, payload delivery both directions, the redundant disconnect
  burst, timeouts, replay windows, zero-sequence rejection — and a
  reduce-cases deletion: netcode's keep-alive prefixing for unconfirmed
  clients is gone from the spec, because our session keys derive at the
  challenge independent of the client index, so payloads are accepted from
  sending-connection-response on and there is no window to prefix around.
  Proven by delivering a payload ahead of the first keep-alive.
- Network simulator: the seeded in-memory wire — latency, jitter, loss,
  duplicates under mocked time, failing runs replay exactly; a debug and
  test instrument, never in the measurement path. The lossy-wire connect
  proven at 5% loss + 5% duplicates, every injected duplicate refused
  exactly once at the replay window.
- THE SWARM: 1000 real clients with churn (800 stay, 100 connect then
  leave, 100 abandon mid-handshake) plus the garbage generator hammering
  the reject rungs throughout. All 900 non-abandoners connect; exactly 100
  mappings reclaimed at the handshake deadline; received == sum of outcomes
  asserted per packet family over the whole storm; ten must-never-fire
  reasons proven exactly zero. FIRST BASELINES (dev machine, in-process,
  single-threaded, diffed from here on — red by regression, never drift):
  5,759 handshakes/sec; 1,249,675 payloads/sec through the server.
- Contract pass: dead-counter scan clean (every counter in the enum has a
  live code path), scaffolding scan clean, client keep-alive suppression
  completed and proven.

- The server loop as API, a netcode regret repaired: rocketnet_frame_sync in
  the library to the settled four-platform design from #56 -- an absolute
  deadline per frame, the platform sleep that honors it (clock_nanosleep
  ABSTIME / mach_wait_until / high resolution waitable timer), priority
  taken on the waiting thread at the first wait (SCHED_FIFO 80 / time
  constraint policy / TIME_CRITICAL), and the capped adaptive spin guard.
  The measured unprivileged-spin death spiral is refused by construction:
  no priority means plain absolute sleep, the cap binds the guard estimate
  itself, and overrun frames never feed the adaptation. Dropped frames
  detected, counted and resynchronized, never chased; stats expose frame,
  drops, priority, guard and wakeup error. Composable with any loop; the
  transports stay loop-agnostic. server.c runs it at 60Hz out of the box.
  The prototype and its windows dispatch workflow contracted with the
  landing, their findings recorded in #56.
- The XDP server transport, first two rungs. The kernel program rewritten
  from flow's byte-range filter to the rocketnet 4-byte prefilter: config
  map carrying public address, port and per-title tag (written at create;
  until then the program passes all traffic untouched), the shared wire
  rule from rocketnet_wire.h -- one header included by the BPF program,
  the sdk and the tests, so the kernel filter and the userspace refusal
  ladder cannot drift -- and a per-CPU stats map counting every verdict.
  The AF_XDP socket de-flowed: no pittle/chonkle, no app header, frames
  are complete wire bytes; config map actually populated (it never was);
  create-failure destroy crash fixed. The server transport gained the
  compile-time XDP backend behind a reserve/commit/abort frame seam, the
  portable path unchanged. Contracted: the old packet filter and its
  tests, rocketnet_packets.h, the flow-era portable server socket. PROVEN
  LIVE on the space box, veth pair in a netns, real NIC untouched: full
  handshake and 1800 pattern payloads echoed through kernel program,
  AF_XDP rings and crafted-frame sends; six garbage packets injected at
  the filter counted exactly -- five wrong tag, one too small -- with
  nothing reaching userspace.
- Step 2 begins: THE TRANSPORTS (Glenn's naming). flow's socket IO carrying
  the connection protocol — the client transport keeps the dual-stack bind,
  preferred ports and receive-thread double buffering verbatim, with the
  protocol at the main-thread seam; the server transport keeps the
  zero-copy shape (payloads out as pointers, sends sealed in place into
  frames, flushed together). The dev surface implemented: published dev
  keys embedded (asserted byte-equal to the test vectors so the copies
  cannot drift) and local dev-token minting. client.cpp and server.cpp are
  the simplest out-of-the-box demos — make && ./bin/server, ./bin/client,
  connected in ~234ms over real UDP with the state machine narrating at
  info level, payloads echoing, clean disconnect. Proven by the transport
  UDP loopback functional test and by running the real binaries. The old
  client socket contracted (deleted, no callers); old server socket files
  remain until the XDP transport rung ports the protocol into them.

## TODO

- ClientTransport and ServerTransport virtual interfaces (with Glenn):
  light C++ like flow, sparingly -- the free-function transport surfaces
  map one to one onto methods, the portable implementations become the
  first concrete transports, and the interface is what makes runtime
  transport selection by preference possible later. The protocol core
  below stays as it is.
- The public surface above it all, ruled: the root header Dean sees is
  netcode-style pure C -- rocketnet_client_t with functions,
  rocketnet_server_t with functions, maybe the address type, nothing
  else. Real non-mangled symbols, so C# lands on P/Invoke and every other
  language binds the same way; whether the inside shims to C++ is an
  implementation privilege, not a surface concern. The Dean Surface (#22)
  is this surface's usage spec.
- The XDP server transport, remaining rungs: ipv6 in the kernel program
  and the userspace path (#19); surface the kernel filter counters
  through the transport counter surface; bring-up on a real NIC (the
  space box, blessed, players quiet); then io_uring and IOCP. Compile-time
  transport selection stays for now; runtime configuration with
  preference ordering is a named follow-on.
- The socket-level swarm with Go orchestration, against real transports.
- The multi-day soaks: soak_sim and soak run unbounded on real hardware
  (the Studio, manually kicked) for days of parallel clients before the
  connect layer is believed; the nightly CI soak is the bounded sentinel.
- The ultimate soak: re-integrate rocketnet into the space game (vendored
  byte-for-byte per the vendoring law) and the space server on ssh space
  soaks it under real conditions -- how flow itself was developed.
  Integration note that matters: the space server pegs to real time with
  drop-frame detection and spinlock-plus-sleep to exact frame ends, never
  naive sleeps -- rocketnet's update is non-blocking with time passed in
  and bounded O(max) walks, which is exactly what that loop requires, and
  nothing in the transports may ever block or take unbounded time in
  update.
- XDP test harnesses, its own thing at its own rung: packet blaster
  loads to prove line rate, and simulated clients against the XDP server.
  Right on the line; complicated; not now.
- Soak: hours-to-days at load once sockets exist, per the verification
  ladder — polite and garbage traffic, counters asserted while running,
  past 2^32 packets on one connection so sequence reconstruction's wrap is
  routine.
- The sodium re-vendor: the gutted two-file flatten with Ed25519 and sealed
  boxes, replacing brew libsodium before anything ships.
- The Dean Surface (#22, Glenn's): the outside-in C API design, an input to
  the top-layer client/server objects.
- Step 3 (after transports): reliability, flows, broadcast.
