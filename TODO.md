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
- Server pacing and the sweep at the server level: rocketnet_server_transport_update
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
  to the rocketnet 4-byte prefilter: config
  map carrying public address, port, filter value and epoch (written at create;
  until then the program passes all traffic untouched), the shared wire
  rule from rocketnet_wire.h -- one header included by the BPF program,
  the sdk and the tests, so the kernel filter and the userspace refusal
  ladder cannot drift -- and a per-CPU stats map counting every verdict.
  The AF_XDP socket de-flowed: no pittle/chonkle, no app header, frames
  are complete wire bytes; config map actually populated (it never was);
  create-failure destroy crash fixed. The server transport gained the
  compile-time XDP driver behind a reserve/commit/abort frame seam, the
  portable path unchanged. Contracted: the old packet filter and its
  tests, rocketnet_packets.h, the earlier portable server socket. PROVEN
  LIVE on the test box, veth pair in a netns, real NIC untouched: full
  handshake and 1800 pattern payloads echoed through kernel program,
  AF_XDP rings and crafted-frame sends; six garbage packets injected at
  the filter counted exactly -- five wrong tag, one too small -- with
  nothing reaching userspace.
- The blessed real-NIC bring-up, done on the test box in a window the owner
  opened by stopping the game (dead-man auto-restart armed first, players
  quiet). The XDP program loaded and attached NATIVE on the production
  Intel NIC, two queues, AF_XDP sockets on both, xdp threads pinned to
  cores 1 and 2 with core 0 left to the OS per the game server's own
  isolation recipe, and the frame sync holding SCHED_FIFO on core 0. A
  client on a laptop connected across the public internet twice, full
  handshake in about 600ms each time. The kernel filter counted 1816
  packets redirected, and garbage aimed at the port from the internet was
  counted exactly -- seven wrong tag, one too small -- none of it
  reaching userspace. Shutdown detached the program and left the NIC
  clean; the game restarted with its own program attached at the same tag
  it carried before. Bench note: the client's payload tally was lost to a
  SIGTERM before its buffered stdout flushed, so the redirect count is
  the payload evidence rather than the client's own line; kill it with
  SIGINT, which it handles.
- The public surface is rocketnet_server_transport_* and
  rocketnet_client_transport_*, ruled on #77: the combined objects ARE the
  transport tier and say so explicitly, so the forest header reads
  rocketnet_server_transport_create/update/payloads/send_packet/is_client_connected/
  disconnect_client and the client equivalents. The plain client/server
  names are vacated for a future high-level tier. The protocol layer's
  functions live in the rocketnet_server_protocol_* and
  rocketnet_client_protocol_* namespace; the socket implementations below
  the transport tier are its drivers.
- rocketnet_server_transport_disconnect_client, netcode parity: the deliberate
  server-side disconnect of one connected client. The protocol writes the
  same redundant ten-packet sealed burst the client sends, then releases
  the slot and destroys the mapping immediately; the transport stages the
  burst through the frame seam on either driver. Proven in test with a
  negative control on the release, and the reasons live above the library.
- The v2 prefilter header, designed in now so the future hooks without a
  wire break: 6 bytes -- packet type, filter epoch, and a 4-byte filter
  value whose meaning the epoch decides. Epoch 0 is the whole of 1.0 and
  carries a per-title 32-bit constant (up from 24 bits, one survivor every
  five minutes against a 10 Gbps random flood); epochs 1-255 name magic
  generations for a keyed MAC, fully specified in the standard and
  deliberately unimplemented, with NO default magic published -- that is
  the one place the dev-keys pattern is refused, because a default magic
  makes a secured-LOOKING path work out of the box. The epoch is on the
  wire because it makes verification one MAC instead of the relay's six
  trial computations. The standard also gained the identity posture: no
  client address in the token (identity beats IP pinning without NAT's
  failures), and the connected-set requirement written as a deployment
  obligation with the client id and the disconnect verb as what the
  protocol provides. Every wire size moved +2, the two assembled vectors
  and Appendix A regenerated from the code, and the keyless derived-bytes
  idea was ruled out in writing because a NAT'd sender cannot know the
  addresses its receiver observes. Suite green on macos and on the test
  box, BPF program compiles, both new gates negative-controlled, soaks and
  the live UDP examples proven on the new format.
- Step 2 begins: THE TRANSPORTS, the game's IO carrying
  the connection protocol — the client transport keeps the dual-stack bind,
  preferred ports and receive-thread double buffering verbatim, with the
  protocol at the main-thread seam; the server transport keeps the
  zero-copy shape (payloads out as pointers, sends sealed in place into
  frames, flushed together). The dev surface implemented: published dev
  keys embedded (asserted byte-equal to the test vectors so the copies
  cannot drift) and local dev-token minting. client.c and server.c are
  the simplest out-of-the-box demos — make && ./bin/server, ./bin/client,
  connected in ~234ms over real UDP with the state machine narrating at
  info level, payloads echoing, clean disconnect. Proven by the transport
  UDP loopback functional test and by running the real binaries. The old
  client socket contracted (deleted, no callers); old server socket files
  remain until the XDP transport rung ports the protocol into them.
- Runtime driver selection (#106), replacing compile-time transport
  selection outright: one build carries every driver its platform has, and
  create walks an ordered candidate list -- the config's driver field,
  overridden by ROCKETNET_DRIVER -- granting the first rung that accepts.
  The old ROCKETNET_SERVER_XDP / ROCKETNET_SERVER_GSO macros are refused by
  a #error rather than merely retired. The ladder is {portable, gso, xdp,
  msgx}: xdp then gso then portable on linux, msgx ahead of portable on
  macOS for both endpoints, portable alone elsewhere. "auto" expands to that
  table, an explicit list is a preference order, and a one-element list is a
  pin that fails loudly instead of falling. The grant is reported
  request-vs-granted through rocketnet_server_transport_driver, with one
  create-time log line naming why each earlier candidate refused.
  Port-zero discovery became a ladder-enforced per-driver property (#109):
  the XDP rung refuses a port-zero address at create rather than binding
  something it cannot discover.
- The sodium re-vendor landed: the gutted two-file flatten (`sodium/sodium.c`
  and `sodium/sodium.h`) with Ed25519 and sealed boxes is the default build,
  replacing brew libsodium. An independently supplied libsodium is opt-in for
  integration testing.

## TODO

- ClientTransport and ServerTransport virtual interfaces:
  light C++ like the game, sparingly -- the free-function transport surfaces
  map one to one onto methods, and the portable implementations become the
  first concrete transports. The protocol core below stays as it is. Note
  the original motive is spent: runtime selection by preference landed
  without virtual interfaces (#106, in DONE), so what is left to decide is
  the C++ ergonomics on their own merits.
- The public surface above it all, ruled: the root header the customer sees is
  netcode-style pure C -- rocketnet_client_transport_t with functions,
  rocketnet_server_transport_t with functions, maybe the address type, nothing
  else. Real non-mangled symbols, so C# lands on P/Invoke and every other
  language binds the same way; whether the inside shims to C++ is an
  implementation privilege, not a surface concern. The public C API surface (#22)
  is this surface's usage spec.
- The XDP server transport, remaining rungs: ipv6 in the kernel program
  and the userspace path (#19); surface the kernel filter counters
  through the transport counter surface; then io_uring and IOCP.
- The socket-level swarm with Go orchestration, against real transports.
- THE SESSION LAYER, a v2 offering (ruled 2026-08-27, the frame sync
  argument at the connection layer: every game has to work this out, so the
  library should hand it to them). THE HEADER FORMAT IS ALREADY IN -- the
  6-byte prefilter with its epoch byte ships in 1.0 at epoch 0, so adopting
  the keyed form later is a configuration change and not a wire break. What
  remains is the system the epoch selects, and it arrives whole. The
  requirement: done completely, it MUST have a backend the
  server talks to, and that backend must handle identity-based auth whose
  consequence is giving away your slot -- one identity, one seat, so a
  shared token steals the sharer's own place and the incentive does the
  enforcement. That needs the backend to track the connected set across
  servers live, and to kick the old player when the same user hash pops on
  elsewhere -- which is exactly what rocketnet_server_transport_disconnect_client
  exists to serve. The game's own backend already runs it, so the epic is an
  EXTRACTION, not a design: proven in the game, generalized into the library
  as an OPTIONAL layer, wanted for the game itself, sequenced strictly behind
  the first deliverable.
  Then the rolling magic feeds the keyed filter value (SipHash over type,
  length and the four bytes after the header, one MAC per packet because
  the epoch names the generation), the per-title generator emits the
  constant and the keyed reader in every language plus verifier-clean BPF
  behind a differential gate, and the aggressive replay window moves into
  the kernel (the replay_protection_buffer at 16 entries, the
  kernel dropping but never advancing so a forged sequence cannot poison
  it). Deliberately NOT in v1: no default magic ships, ever -- a published
  secret would make a secured-LOOKING path work out of the box, which is
  worse than not having the feature; and standalone IP pinning is out for
  good, because identity beats it on the merits without NAT's problems.
- Replay protection in the kernel, and the buffer size question (owner,
  2026-08-27: "replay protection would need to be in XDP program to be
  rad" and "I like doing it with 16 entries. That's nicer"). This needs no
  backend and no magic -- a BPF map keyed by source address holding the
  high-water sequence, checked in the kernel so a replayed packet never
  costs a ring slot. THE SAFETY CONDITION IS THE DESIGN: the kernel drops
  but never advances. Only userspace, after the AEAD opens, writes the
  high-water back, so a forger who wins the filter's odds cannot poison a
  session by setting the mark to 2^32. Read-only in the kernel, the filter
  can lag a beat behind truth and can never be corrupted by it.
  ONE TENSION TO SETTLE: the owner prefers 16
  entries on memory grounds, which is right for a per-connection kernel
  map at a thousand clients (128 KB against 8 MB), but STANDARD.md's
  replay window sizing rule says never smaller than 256 and at least one
  second of the maximum receive rate -- sized for loss-burst recovery, not
  reordering. Both can be true if the kernel map and the userspace window
  are sized separately, which is probably the answer, but the standard
  currently states one rule for both and should not be quietly narrowed.
  RocketNet sizes the kernel map and the userspace replay window separately, and
  the standard should say so.
- The multi-day soaks: soak_sim and soak run unbounded on real hardware
  (the test machine, manually kicked) for days of parallel clients before the
  connect layer is believed; the nightly CI soak is the bounded sentinel.
- The ultimate soak: re-integrate rocketnet into the game (vendored
  byte-for-byte per the vendoring law) and the game server soaks it under
  real conditions.
  Integration note that matters: the game server pegs to real time with
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
- The public C API surface (#22): the outside-in C API design, an input to
  the top-layer client/server objects.
- Step 3 (after transports): reliability, flows, broadcast.
