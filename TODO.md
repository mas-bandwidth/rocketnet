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

## TODO

- Connected traffic: keep-alives both ways, confirmed flag and prefixing,
  payload delivery, disconnect with redundant burst, timeouts, replay window,
  zero-sequence rejection.
- Synthetic traffic swarm (part of step 1, not later): real handshakes at
  scale, churn schedules, a fraction abandoning mid-handshake, a garbage
  generator so reject paths run under load. Swarm C++ with the tests;
  orchestration Go.
- Smoke load target and first perf baseline (pps/core, p99), recorded and
  diffed thereafter.
- Contract pass at each rung: remove scaffolding, dedupe, tighten; old
  flow-era wire code contracts when the new connect replaces its callers, not
  before.
