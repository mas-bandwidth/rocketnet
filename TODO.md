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

## TODO

- Cookie: MAC with the exact 7/19-byte address input, generate/validate,
  expiry, rotation overlap, Appendix A vector match.
- Counters framework: lands HERE, with its first real users — one counter per
  distinct rejection reason, arrival counted separately, monotonic, never
  reset; check_counter/check_no_counter test helpers. (Deliberately not
  earlier: a counter with no code path is a dead counter.)
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
