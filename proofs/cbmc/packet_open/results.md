# Packet prefilter and sealed-packet opener: CBMC results

Flags from `proofs/cbmc/Makefile`
(`--bounds-check --pointer-check --signed-overflow-check --unsigned-overflow-check --div-by-zero-check --unwinding-assertions`), plus
`--unwind 64`, `--object-bits 12`, `-DNDEBUG`, `-I$(SDK)` and
`-I$(SDK)/../sodium`.

The reader is `sdk/rocketnet_protocol.cpp`, included by
`proof_packet_open.c`; no sdk code is copied. libsodium is modeled, not
proved: each model checks its own reads with `__CPROVER_r_ok` and its own
writes with `__CPROVER_w_ok` at the documented lengths, writes nondet bytes,
and returns a nondet success or failure. A packet buffer is allocated at
exactly the symbolic `packet_bytes`, so a read one past the caller's length
is an out-of-bounds failure.

Run on hetzner1 (Linux x86_64), CBMC 6.6.0 and CaDiCaL 2.1.3, at 49c36e8f:
RocketNet 8e792181 plus the harness commits of #754 (the harness parses
`rocketnet_protocol.cpp` in the C front end again). `sdk/rocketnet_protocol.cpp`
and every header it includes are identical at main 212f6da6.

| harness | properties | passed | bound proved | unwind |
| --- | ---: | ---: | --- | ---: |
| `harness_prefilter` | 765 | 765 | `packet_bytes` symbolic in 0..1453 (`ROCKETNET_RECEIVE_CEILING + 1`); packets below 6 (`ROCKETNET_PREFILTER_BYTES`) return `ROCKETNET_PREFILTER_TOO_SHORT`, above 1452 (`ROCKETNET_RECEIVE_CEILING`) return `ROCKETNET_PREFILTER_TOO_LARGE`, and no path reads past `packet_bytes` | 64 |
| `harness_open_sealed_packet` | 928 | 928 | `packet_bytes` symbolic in 0..1452 (`ROCKETNET_RECEIVE_PACKET_BYTES`), `out_capacity` symbolic in 0..1416 (`ROCKETNET_MAX_PACKET_PAYLOAD_BYTES`), every packet type byte, sequence, session-nonce and ephemeral pairing; no read past `packet_bytes`, no write past `out_capacity`, `-1` for a short packet or a small output, result in `[-1, out_capacity]` | 64 |
| `harness_write_packet_associated_data` | 924 | 924 | 53-byte object with the ephemeral key (`ROCKETNET_AD_WITH_EPHEMERAL_BYTES`), 21-byte object without (`ROCKETNET_AD_BYTES`); the writer writes only inside the returned length | 64 |
| `harness_sequence_nonce` | 765 | 765 | 12-byte object, every sequence; four zero bytes then the sequence little-endian, all inside 12 bytes | 64 |

All 3382 properties pass; there is no failed or unproved property. The four
harnesses take about two seconds together on one core.

Notes and scope:

- `harness_open_sealed_packet` proves the refusal pairing for a session nonce
  on a type that does not carry one, an ephemeral key on a type that does not
  carry one, and a required nonce or ephemeral key that is missing.
- libsodium itself (`crypto_aead_chacha20poly1305_ietf_decrypt`,
  `crypto_shorthash_siphash24`, `sodium_memzero`, `crypto_generichash`) is
  outside the claim and is modeled in the harness.
