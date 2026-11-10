# Source-bound reconstruction results

## Historical result

The inventory previously recorded a source-bound pass at `a26abaf8` on
vision: `0 of 608 failed`, `VERIFICATION SUCCESSFUL`. The older
`d5dd954f` result in `full-bounds-partition-results.md` covered the copied
36-property harness; it is retained as historical evidence and does not
establish correspondence with the current SDK.

## Type-9 integration refresh (2026-11-01)

Frozen RocketNet source: `59752f6f4878360534f03ef7112a27e09013e9ca`,
integrating type9 `7047d7c68d2f18caf2dc9c54a4b9ead4a16cad14`.
The harness includes the current `sdk/rocketnet_protocol.cpp` and
`sdk/rocketnet_message.h`; it does not copy the reconstructed operations.
Native source hashes were checked against the frozen source copy.

Both targets ran serially on hulk (Linux x86_64) with CBMC
`6.6.0 (cbmc-6.6.0)`, executable SHA-256
`ceefc9de0ef9d6319b68d0aad889cbc2bd86f9b58f2f7a2f5a9e0c552bca6cc2`:

```sh
make -C proofs/cbmc reconstruct
make -C proofs/cbmc reconstruct-reachability
```

The unchanged target flags enable bounds, pointer, signed and unsigned
overflow, division-by-zero and unwinding checks. Unwind remains 1.
Each invocation had an outer 1800-second timeout and a 16000000-KB
address-space cap.

| Target | Actual result | Elapsed | Maximum RSS |
| --- | --- | ---: | ---: |
| `reconstruct` | 0/608 failed; VERIFICATION SUCCESSFUL | 3.30 s | 49280 KiB |
| `reconstruct-reachability` | Exactly 1/609 failed on `reconstruct reachability control`; wrapper EXPECTED FAILURE | 2.81 s | 46528 KiB |

Both Make invocations exited zero. The negative control failed only
`main.assertion.10` at harness line 166, after the production operations.

That run's scope was 32-bit expected values in `[2^32, 2^64 - 2^32)` and
24-bit references in `[2^23, 2^64 - 2^23]`; the first lap was assumed
away. The section below supersedes it.

## First lap, floors removed (2026-11-09)

Harness commit `4f69c60897f7bc84b78ebdc6f938d046bbb7e196` on base
`830562c4ef9e2777bacee7986773f24b60a56662`. The floor assumptions
(`expected >= 2^32`, `highest_opened >= 2^32 - 1`, `reference >= 2^23`)
are removed; the SDK is unchanged. The entry point is
`harness_reconstruct_sequence`; `main` calls it so the Make targets are
unchanged.

What is now proved, from zero:

- `rocketnet_reconstruct_near` and `rocketnet_reconstruct_sequence`
  (32-bit): the result keeps the wire low 32 bits and is the nearest
  non-negative candidate, the larger on a tie. The two neighbouring
  candidates decide it (distance falls then rises along candidates 2^32
  apart): the one above is strictly farther, the one below, when
  non-negative, no nearer. `|result - expected| <= 2^31` is asserted from
  `expected >= 2^31` on; below that the nearest candidate can be farther,
  because no candidate below zero exists. Every non-negative in-window
  value round-trips.
- `rocketnet_message_u24_delta` (24-bit): where the delta is zero or more,
  or the behind value is non-negative, `reference + delta` keeps the wire
  low 24 bits and lies within 2^23 of the reference. Where the behind value
  would sit below zero (first lap only), `rn_window_receive_accept` refuses
  it as behind without subtracting; the proof shows the nearest
  non-negative candidate then lies at least 2^23 ahead, outside the window.
  The 24-bit tie still goes to the lower value. Every non-negative in-window
  value round-trips.

Bounds stated as assumptions, unchanged from the earlier run: the ceilings
`expected < 2^64 - 2^32` (32-bit) and `reference <= 2^64 - 2^23` (24-bit)
remain assumed, and the uint64 ceiling above them is not proved. Unwind 1
(the reconstructions are straight-line). No ACK property is proved.

Run on vision (Linux x86_64, 64 cores) with CBMC `6.6.0 (cbmc-6.6.0)`,
each through `nova-reserve run --`:

| Command | Actual result | Elapsed | Maximum RSS |
| --- | --- | ---: | ---: |
| `cbmc proofs/cbmc/proof_reconstruct_sequence.c --function harness_reconstruct_sequence -I sdk` | 0/856 failed; VERIFICATION SUCCESSFUL | 6.21 s | 60284 KiB |
| `make -C proofs/cbmc reconstruct` | 0/617 failed; VERIFICATION SUCCESSFUL | 10.08 s | 71812 KiB |
| `make -C proofs/cbmc reconstruct-reachability` | Exactly 1/618 failed on `reconstruct reachability control`; wrapper EXPECTED FAILURE | 8.20 s | 63524 KiB |

Red first: the base harness with only its three floor assumptions deleted
fails 4 of 608 with the Make target's flags: both 32-bit
`|result - reference| <= 2^31` claims, the 24-bit `<= 2^23` claim, and the
unsigned underflow `reference - magnitude` in the harness's 24-bit helper.
Those claims were false on the first lap; the new assertions state what
the code does there.

Planted bugs (SDK copies outside the tree, never committed), each run
against this harness and the base harness with the Make target's flags:

| Planted bug | This harness | Base harness |
| --- | --- | --- |
| `if ( base == 0 ) return wire_value;` in `rocketnet_reconstruct_near` (first lap never picks the candidate above) | VERIFICATION FAILED, 4/617: candidate above strictly farther, `<= 2^31`, both round-trips | VERIFICATION SUCCESSFUL, 0/608 |
| `rocketnet_message_u24_delta` treats every delta as ahead while `reference < 2^23` | VERIFICATION FAILED, 2/617: 24-bit `<= 2^23`, 24-bit round-trip | VERIFICATION SUCCESSFUL, 0/608 |
