# Packet prefilter and sealed-packet opener: CBMC results

CBMC 6.6.0, one bench core. Flags from `proofs/cbmc/Makefile`
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

| harness | properties | passed | bound proved | unwind |
| --- | ---: | ---: | --- | ---: |
| `harness_prefilter` | 1244 | 1244 | `packet_bytes` symbolic in 0..1473; packets below 6 return `ROCKETNET_PREFILTER_TOO_SHORT`, above 1472 (`ROCKETNET_RECEIVE_CEILING`) return `ROCKETNET_PREFILTER_TOO_LARGE`, and no path reads past `packet_bytes` | 64 |
| `harness_filter_epochs_install` | 1511 | 1511 | `live_count` symbolic in 0..4, every epoch byte; the table indices stay in `0..ROCKETNET_FILTER_EPOCHS_LIVE-1` | 64 |
| `harness_filter_epochs_check` | 1528 | 1528 | `live_count` symbolic in 0..4, every epoch byte; the table indices stay in `0..ROCKETNET_FILTER_EPOCHS_LIVE-1` | 64 |
| `harness_open_sealed_packet` | 1515 | 1515 | `packet_bytes` symbolic in 0..1472, `out_capacity` symbolic in 0..1416, every packet type byte, sequence, session-nonce and ephemeral pairing; no read past `packet_bytes`, no write past `out_capacity`, `-1` for a short packet or a small output, result in `[-1, out_capacity]` | 64 |
| `harness_write_packet_associated_data` | 1511 | 1511 | 53-byte object with the ephemeral key, 21-byte object without; the writer writes only inside the returned length | 64 |
| `harness_sequence_nonce` | 1244 | 1244 | 12-byte object, every sequence; four zero bytes then the sequence little-endian, all inside 12 bytes | 64 |

All 8553 properties pass; there is no failed or unproved property.

The full bound was proved directly; no reduced bound was needed. The whole
target runs in about three seconds on one bench core.

Notes and scope:

- `harness_filter_epochs_check` feeds a full `ROCKETNET_RECEIVE_CEILING`
  packet object with symbolic bytes. The claim for this reader is the epoch
  table index range on every epoch byte, as the card names it; a
  packet-length claim is not made for this reader here. `rocketnet_keyed_filter_value`
  reads the four bytes after the prefilter header, so a packet-length proof
  would need the caller to guarantee at least ten bytes.
- `harness_open_sealed_packet` proves the refusal pairing for a session nonce
  on a type that does not carry one, an ephemeral key on a type that does not
  carry one, and a required nonce or ephemeral key that is missing.
- libsodium itself (`crypto_aead_chacha20poly1305_ietf_decrypt`,
  `crypto_shorthash_siphash24`, `sodium_memzero`, `crypto_generichash`) is
  outside the claim and is modeled in the harness.
