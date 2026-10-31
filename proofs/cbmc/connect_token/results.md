# Connect-token readers: ESBMC 8.5 results

The nightly target includes the actual `sdk/rocketnet_connect_token.cpp` through
`proof_connect_token.cpp`. The shared `../esbmc_shim.h` models the CBMC-style
assume/assert/extent/nondeterministic-write calls. Libsodium calls are modeled
at their documented buffer lengths; libsodium itself is not verified. Each
input array is exactly its documented size: token 768 bytes, signed data 576
bytes, private data 384 bytes, signature 64 bytes, and key 32 bytes.

The target uses `--std c++17 --z3 --overflow-check` with ESBMC's default
pointer, bounds and divide-by-zero checks, `-DROCKETNET_ESBMC -DNDEBUG`,
a 30-minute wall timeout and 8 GiB address-space cap per harness. The
field-copy paths use unwind 577, 433, and 257 respectively to cover ESBMC's
byte-copy library model; the verifier and consistency paths use 1, the
identifier hash-output model 17, and the private-block open model 385 to
cover the shim's nondeterministic byte-write loop.

| Harness | Bound | Unwind | Proved | Refuted | Timed out | Other/unproved |
| --- | --- | ---: | ---: | ---: | ---: | --- |
| `harness_read_connect_token` | 768-byte symbolic token | 577 | 25/25 | 0 | 0 | 0 |
| `harness_read_signed_connect_data` | 576-byte symbolic signed block | 433 | 88/88 | 0 | 0 | 0 |
| `harness_read_connect_token_private` | 384-byte symbolic private block | 257 | 61/61 | 0 | 0 | 0 |
| `harness_verify_signed_connect_data` | 576-byte signed block, 64-byte signature, 32-byte key | 1 | 12/12 | 0 | 0 | 0 |
| `harness_open_connect_token_private` | 576-byte signed block, 432-byte sealed block, 384-byte output | 385 | 57/57 | 0 | 0 | 0 |
| `harness_private_consistent` | symbolic private fields and 576-byte signed block | 1 | 36/36 | 0 | 0 | 0 |
| `harness_connect_token_identifier` | 576-byte signed block, 16-byte identifier output | 17 | 10/10 | 0 | 0 | 0 |

All seven ESBMC 8.5.0 runs ended `VERIFICATION SUCCESSFUL` at the full listed
array bounds: 289/289 checked properties passed, with zero refuted, timed out,
or otherwise unproved. Raw output is saved in the allocated Linux gate job's
`evidence/harness_*.log`; the make summary is `evidence/make-connect-token-r3.log`.
Each run used one CPU at nice 19; the target applied `timeout 1800s` and
`prlimit --as=8589934592` separately to each ESBMC invocation. No reduced
buffer bound was used.

The existing per-commit `make -C proofs/cbmc` target also exited 0 on this
tree. It ran its original CBMC leaves only; the new reader remains
nightly-only.

The first trial with unwind 1 failed only an unwinding assertion in ESBMC's
byte-copy model (`/esbmc-vfs/libc/library/string.c:284`), not in RocketNet.
Raising that harness to 577 discharged it. The carried prior-attempt CBMC
5.95.1 results remain historical and are not counted as ESBMC evidence.
