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

## TODO

- Server request path: the ordered list check by check over the encryption
  mapping, used-token history, admitted-token set; one negative test per
  check asserting its reason counter AND the arrival counter; the stored
  challenge resend; the handshake deadline and idle deadline, separately.
- Challenge and response, client state machine: monotonic climb 1-4, per-type
  client processing, temporaries committed only on successful open, every
  packet loss walked.
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
