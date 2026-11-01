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
Native source hashes were checked against the frozen snapshot.

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

The scope remains the declared 32-bit and 24-bit wire half-ranges:
32-bit expected values in `[2^32, 2^64 - 2^32)`, and 24-bit references
in `[2^23, 2^64 - 2^23]`. The proof checks preserved low wire bits,
documented nearest-value/tie behavior and in-window round trips.
It does not prove the excluded uint64 floor/ceiling or any ACK property.
