# Connect-token readers: CBMC results

The benchmark has `cbmc 5.95.1 (cbmc-5.95.1)` and `goto-cc`; the measured
version is recorded here rather than the 6.6.0 the card expected. Flags come
from `proofs/cbmc/Makefile`
(`--bounds-check --pointer-check --signed-overflow-check --unsigned-overflow-check --div-by-zero-check --unwinding-assertions`), plus
`--unwind 1`, `--object-bits 12`, `-DNDEBUG`, `-I$(SDK)` and
`-I$(SDK)/../sodium`.

The reader is `sdk/rocketnet_connect_token.cpp`, included by
`proof_connect_token.c`; no sdk code is copied. Each token, signed block and
private block is a stack object of exactly its wire size, so a read one past
the documented block is an out-of-bounds failure, not slack. libsodium is
modeled, not proved: each model checks its own reads with `__CPROVER_r_ok` and
its own writes with `__CPROVER_w_ok` at the documented lengths, writes nondet
bytes, and returns a nondet success or failure. The `crypto_box_seal_open`
model also asserts that the reader passes
`ROCKETNET_SIGNED_OFFSET_SEALED_BLOCK` and `ROCKETNET_SEALED_BLOCK_BYTES` inside
the 576-byte signed block, and that it writes `ROCKETNET_PRIVATE_BLOCK_BYTES`.

| harness | properties | passed | bound proved | unwind |
| --- | ---: | ---: | --- | ---: |
| `harness_read_connect_token` | 1253 | 1253 | token exactly `ROCKETNET_CONNECT_TOKEN_BYTES` (768) symbolic bytes; fields read at 0, 576, 640 and 672, last byte read at 703, no read past 768 | 1 |
| `harness_read_signed_connect_data` | 1253 | 1253 | signed block exactly `ROCKETNET_SIGNED_CONNECT_DATA_BYTES` (576) symbolic bytes; every field at its documented offset, the sealed block ends at 89 + 432 = 521 < 576 | 1 |
| `harness_read_connect_token_private` | 1253 | 1253 | private block exactly `ROCKETNET_PRIVATE_BLOCK_BYTES` (384) symbolic bytes; every field at its documented offset, the last user-data byte at 343 < 384 | 1 |
| `harness_verify_signed_connect_data` | 1093 | 1093 | signed block 576, signature 64 and backend public key 32 symbolic bytes; the model reads exactly those lengths | 1 |
| `harness_open_connect_token_private` | 1253 | 1253 | signed block exactly 576 symbolic bytes; sealed block at offset 89 for 432 bytes (ends at 521), private block write 384 bytes | 1 |
| `harness_private_consistent` | 1093 | 1093 | every private field symbolic and the whole 576-byte signed block symbolic; protocol id at 4 and expire at 20 read inside the block | 1 |
| `harness_connect_token_identifier` | 1093 | 1093 | signed block exactly 576 symbolic bytes, identifier write 16 bytes | 1 |

All 8291 properties pass; there is no failed or unproved property. The full
bounds were proved directly; no reduced bound was needed. The target runs in
about ten seconds on one bench core. The whole `make -C proofs/cbmc` (the
per-commit `all` set) does not include this target and is unchanged.

Notes and scope:

- The models cover only the libsodium calls the included unit references:
  `crypto_box_seal_open`, `crypto_sign_verify_detached`, `crypto_generichash`,
  and the writer-only `crypto_box_seal`, `crypto_scalarmult_base`,
  `crypto_generichash_init`, `crypto_generichash_update`,
  `crypto_generichash_final`, `crypto_box_easy`, `crypto_sign_detached` and
  `sodium_memzero`. The writer models keep the unit linkable; no writer harness
  is part of this claim.
- libsodium itself is outside the claim and is not proved.
- `harness_private_consistent` starts with an uninitialized
  `struct rocketnet_connect_token_private_t`, so every private field is
  symbolic; its assertion relates the result to the private protocol id and
  expire and the signed fields at `ROCKETNET_SIGNED_OFFSET_PROTOCOL_ID` and
  `ROCKETNET_SIGNED_OFFSET_EXPIRE`.
