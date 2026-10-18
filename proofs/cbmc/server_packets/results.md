# Server per-type packet readers: CBMC results

CBMC 6.6.0, one bench core. Flags from `proofs/cbmc/Makefile`
(`--bounds-check --pointer-check --signed-overflow-check --unsigned-overflow-check --div-by-zero-check --unwinding-assertions`), plus
`--unwind 64`, `--object-bits 12`, `-DNDEBUG`, `-I$(SDK)` and
`-I$(SDK)/../sodium`.

The readers are `sdk/rocketnet_server_protocol.cpp`, included by
`proof_server_packets.c`; no sdk code is copied. libsodium is modeled, not
proved: each model checks its own reads with `__CPROVER_r_ok` and its own
writes with `__CPROVER_w_ok` at the documented lengths, writes nondet bytes,
and returns a nondet success or failure. The server struct is built by the
code's own create path at the smallest accepted config (one client slot);
the opaque sub-tables (encryption map, client slots, used-token history,
acks, reassembly) are created by modeled create functions and looked up by
modeled find/entry functions that check their reads, write nondet state and
return a nondet result. A packet buffer is allocated at exactly the symbolic
`packet_bytes`, so a read one past the caller's length is an
out-of-bounds failure and not slack.

| harness | properties | passed | bound proved | unwind |
| --- | ---: | ---: | --- | ---: |
| `harness_connection_request` | 3489 | 3489 | `packet_bytes` symbolic in 0..ROCKETNET_MAX_PACKET_BYTES (1472), every connect-token byte; no read past `packet_bytes`, no write past `ROCKETNET_CONNECTION_CHALLENGE_WIRE_BYTES`, result and `*response_bytes` inside their documented ranges | 64 |
| `harness_connection_response` | not proved | - | - | 64 |
| `harness_sealed_packet` | not proved | - | - | 64 |
| `harness_process_packet` | not proved | - | - | 64 |

`harness_connection_request` proves at the full bound. The other three
harnesses do not yet reach verification: they fail on "no body for callee"
for the sdk helper functions the deeper reader paths call from other
translation units — `rocketnet_client_slots_connected`,
`rocketnet_client_slots_mapping_index`, `rocketnet_client_slots_find`,
`rocketnet_client_slots_num_free`, `rocketnet_client_slots_assign`,
`rocketnet_used_token_history_record`, `rocketnet_encryption_map_address`,
`rocketnet_spam`, `rocketnet_write_sealed_packet`,
`rocketnet_server_acks_reset_slot`,
`rocketnet_reassembly_set_reset_connection`, and the ack block helpers
(`rocketnet_ack_endpoint_mark_keep_alive`, `rocketnet_ack_block_build`,
`rocketnet_ack_endpoint_record_sent_classified`,
`rocketnet_ack_count_sent_result`) — which have no model in this harness yet.
These are harness limits (missing models), not sdk defects; every failed
property is a "no body for callee" or an overflow/dereference that follows
from an unmodelled helper returning none, reported by CBMC at the sdk source
line of the call. Modeling these helpers soundly is the remaining work; the
card is not yet done.

Findings of note from the connection-request proof:

- The reader rejects any `packet_bytes != ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES`
  (710) before reading past the prefilter header, and the fixed-offset field
  reads all lie inside a 710-byte request.
- `counters[ROCKETNET_COUNTER_*]++` writes were proved only after the
  counters struct was zero-initialized in the harness (the corrupters are not
  symbolic); an uninitialized counters array makes the increments overflow
  and is a harness artifact, not a reader defect.
- `memcmp` on the fixed-length request fields (cookie field, address, salt,
  server key) unwinds cleanly at 64; at `--unwind 8` the model's loop is the
  only failure.
