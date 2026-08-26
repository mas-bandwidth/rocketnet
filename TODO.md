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

## TODO

- Sodium in the build: link libsodium (brew on mac, libsodium-dev on linux),
  CI deps for both legs. No hydrogen removal yet; it leaves when the sodium
  construction fully replaces it.
- Crypto expectations: pin every primitive the standard uses against real
  libsodium. Sizes vs headers; sign/verify round trip + refusals; sealed box
  round trip + wrong-keypair refusal + the anonymous-box hazard; AEAD round
  trip + AD/nonce/truncation negative controls; generichash keyed and
  unkeyed, 16-byte output is not a truncation of 32; scalarmult small-order
  rejection; the four KDF outputs reproducing Appendix A exactly.
- Test vector data: the Appendix A fixed inputs and frozen outputs as checked
  in arrays (generated once from the vectors harness, never transcribed by
  hand).
- Connect token: private block, signed data, token write/read at the exact
  offsets; mint for tests and the dev path; server-side verify-then-open with
  the inner consistency checks; token identifier (unkeyed generichash, 16).
  Frozen-token vector recovers exact fields; negative controls per refusal.
- Prefilter header: test title tag b7 59 01, write and structural check, pure
  functions tested to death.
- Packet layouts: write/read every type, exact sizes from the table,
  assembled wire vectors matched byte for byte, malformed rejection.
- Sealing: per-type seal/open with sequence-derived nonce and the 21/53-byte
  AD; per-type vectors matched exactly; negative controls: wrong key, altered
  AD, altered sequence, zero sequence, truncation.
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
