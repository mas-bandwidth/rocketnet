# client_packets: CBMC proofs of the client's packet readers

`make -C proofs/cbmc proofs-client_packets` runs every harness in
`proof_client_packets.c`. The target is on the nightly list only, through
`proof.mk`. It is not in `all`, and not in `.github/workflows/cbmc.yml`.

## Checker and bound

- CBMC 6.6.0, with the shared `CBMC_FLAGS` (`--bounds-check --pointer-check
  --signed-overflow-check --unsigned-overflow-check --div-by-zero-check
  --unwinding-assertions`). This proof adds `--unwind 64 --object-bits 12
  --arrays-uf-always --slice-formula`. The harnesses do not pass
  `-DNDEBUG`, so `rocketnet_assert` stays live and every assert on these
  paths is a property.
- The bound is the full one. `packet_bytes` is symbolic in
  0..`ROCKETNET_MAX_PACKET_BYTES` (1472, which equals
  `ROCKETNET_RECEIVE_CEILING`). The packet is a `malloc` of exactly
  `packet_bytes` bytes, so a read one byte past the end fails the bounds
  check. The dispatcher and record harnesses also make the output capacity
  symbolic in 0..1472, over an object of exactly that size.
- Unwind 64 is enough. Every unwinding assertion passes. The longest loop on
  these paths is the 32-byte key write in the associated-data writer.
- Why `--arrays-uf-always`: without it, the payload harness used up the
  bench's 30 GB of memory in about 3 minutes and was killed. The cause is
  the symbolic-length copies out of the 1472-byte opened buffer.
- Why `--slice-formula`: with `--arrays-uf-always` alone, the dispatcher
  and record harnesses stop CBMC with an internal invariant failure
  (`flattening/arrays.cpp:201 collect_arrays`, "unexpected array
  expression: member with 'struct'"). `--slice-formula` avoids that failure
  in CBMC 6.6.0, so this target uses it for every harness.
- Reversed witnesses: `proof.mk` runs every harness a second time with
  `-DPROOF_WITNESS`. In that build, each harness asserts that its accepting
  path never runs. The run must fail exactly those `witness:` assertions and
  nothing else. All eleven do, so no harness passes vacuously. Extra
  witnesses cover the cookie leaving requesting cookie and the keep-alive
  that connects the client. The payload
  witness reaches a delivered payload of 1439 bytes
  (`ROCKETNET_MAX_RECEIVE_PAYLOAD_BYTES`), which means the full packet size
  is actually reached.

## Results (amd-1, 2026-10-18)

CBMC reports "0 of 4891 failed" for each harness. 4891 is every property
instrumented in the translation unit, and that total includes code the
harness never reaches. Each row below lists the harness's own assertions on
top of the shared bounds, pointer, overflow, division-by-zero, unwinding and
`rocketnet_assert` checks.

| harness | result | time | harness assertions |
|---|---|---|---|
| harness_process_cookie | proved | 6 s | returns 0/1; the wrong size (not 38) is refused; the wrong state is refused with the state unchanged; a salt mismatch is refused; an accepted cookie is stored and the client moves to sending connection request |
| harness_process_challenge | proved | 6 s | returns 0/1; the wrong size (not 66) is refused; any state but sending connection request is refused with the state unchanged; a zero sequence or a packet that does not open is refused; an accepted challenge moves the client to sending connection response |
| harness_process_denied | proved | 8 s | returns 0/1; the wrong size (not 34) is refused; any state but sending connection request or response is refused with the state unchanged; a zero sequence or a packet that does not open is refused; an accepted denied ends the attempt as denied |
| harness_open_session_packet | proved | 17 s | covers every session type's floor and size rule (keep-alive with its validator, disconnect, ack, fragment, payload): a packet below the floor is refused; outside sending connection response and connected the packet is refused; a zero sequence, a replayed sequence (read from the window before the call), a packet that does not open, and a packet whose ack block the read refuses are all refused; a keep-alive with an invalid index or slot count is refused; an opened packet's block plus type data fill exactly packet_bytes - 26; a fixed-size type opens only at its size, a ranged type only above its floor |
| harness_process_keep_alive | proved | 21 s | the session reader asserts below; the first keep-alive connects with 0 <= index < max_clients <= 1000 |
| harness_process_payload | proved | 21 s | the session reader asserts below; a refused payload delivers 0 bytes; a delivered payload is 1..packet_bytes - 33 bytes and at most 1439 |
| harness_process_ack | proved | 8 s | the session reader asserts below |
| harness_process_fragment | proved | 4 s | the session reader asserts below; outside connected a fragment is refused |
| harness_process_disconnect | proved | 5 s | the session reader asserts below; an accepted disconnect disconnects |
| harness_process_packet | proved | 13 s | the dispatcher asserts below |
| harness_process_record | proved | 160 s | the dispatcher asserts below, over every I/O verdict; the record is left raw afterwards |

The session reader asserts: returns 0/1; a packet below the type's wire
floor is refused; outside sending connection response and connected the
packet is refused with the state unchanged.

The dispatcher asserts: returns 0/1; `*out_payload_bytes` is in
[0, out_payload_capacity] and is nonzero only on an accepted packet; a
capacity below 1439 is refused before any read, with no payload and the
state unchanged; a packet shorter than the 6-byte prefilter header is
refused; a packet that fails the prefilter (wrong epoch or filter value) is
refused; a reserved type (10..255), a request (0) and a response (3) are
refused; only a payload packet delivers bytes, and it delivers at most
packet_bytes - 33.

No property failed and none timed out. Every harness proved at the full
bound, so no smaller bound is recorded.

## Client state

The client is built by `rocketnet_client_protocol_init`, the code's own
create path. Every field a reader branches on is then made symbolic:

- the state, over all twelve client states (-7..4)
- both replay windows and both high-water marks
- both send sequences, from 1
- the integrity-failure count, below `ROCKETNET_INTEGRITY_LIMIT`
- the cookie flag, the rtt fields, the client index and slot count
- the ack debt and the acked count
- the I/O verdict and the record sequence of the record being consumed
  (the per-type harnesses)

The reassembly set is present, as the transport always creates it. The key
hooks stay NULL.

Assumptions, each a bound the code keeps elsewhere:

- High-water marks and send sequences stay below 2^63. An attempt starts
  them at 1. At 2^64 - 2^32 the 32-bit reconstruction's candidate
  arithmetic would wrap, and the reconstruct proofs in proofs/cbmc cover
  only the range below that.
- An opened record carries a nonzero sequence. This is the I/O thread's
  contract (`sdk/rocketnet_client_io.h`,
  `rocketnet_client_io_decrypt_datagram`, which returns
  `ROCKETNET_CLIENT_IO_VERDICT_ZERO_SEQUENCE` before it ever writes
  `OPENED`). Without this assumption, `rocketnet_replay_already_received`'s
  `rocketnet_assert( sequence != 0 )` fails at
  `sdk/rocketnet_client_protocol.cpp:960` on a record that the I/O thread
  cannot produce. The fault is in the harness's input, not in the sdk.
- The verdict is one of the six the I/O thread writes (0..5).
- The ack debt stays below 2^63. The debt goes back to zero on every
  session send, so `ack_debt++` (`rocketnet_client_protocol.cpp:859`)
  cannot overflow.
- The acked count is in 0..`ROCKETNET_SENT_PACKET_BUFFER_SIZE`, the batch
  capacity. The process-block model asserts this on entry.
- The rtt estimate is not negative. It starts at 0 and only takes
  non-negative samples.

## What is modelled, not proved, here

Each model checks its own buffer arguments with `__CPROVER_r_ok` and
`__CPROVER_w_ok`.

- libsodium: `crypto_aead_chacha20poly1305_ietf_decrypt`/`_encrypt`,
  `crypto_generichash`, `crypto_scalarmult`, `crypto_shorthash_siphash24`,
  `sodium_memcmp`, `sodium_memzero`. Each model checks the lengths
  libsodium documents (generichash: output 16..64 bytes, key at most 64), writes nondet bytes, and returns nondet success or
  failure.
- `rocketnet_replay_advance`. The model asserts its precondition (a nonzero
  sequence, not already received), havocs the window, and raises the
  high-water mark. The real advance clears up to 4095 bits in a loop. Its
  deltas 1..3 are proved by `replay-delta-1-3`; the full delta range is not
  proved (`replay-near-4096`).
- `rocketnet_ack_block_read`, `rocketnet_ack_endpoint_process_block`,
  `rocketnet_ack_endpoint_record_received` and `rocketnet_ack_endpoint_reset`.
  `sdk/rocketnet_ack.cpp` is C++ (a template, and `std::initializer_list`
  through `rocketnet_memory.h`), so a C proof cannot include it. The block
  read model asserts that it may read the opened plaintext for its length,
  and returns 0 or a block of 7 + 4 * num_ranges bytes inside that length
  that obeys the validity rules the code documents. The process-block model
  asserts that it is given a block the read accepted and an acked batch it
  can write. The ack block reader is the subject of
  rocketnet-cbmc-ack-block-read.
- `rocketnet_reassembly_process_fragment`. The model asserts that the type
  data is readable for its length and that the length is at least the
  6-byte header plus one byte. The reassembly is the subject of
  rocketnet-cbmc-fragment-reassembly.
- The logging calls do nothing.

The replay-advance, ack-reset and record-received models leave out writes
the real code makes: the window bit, the endpoint zeroing, and the received
high-water mark. No reader on these paths reads those fields afterwards.

Refusals this card does not prove, because a model stands for the check:

- what makes an ack block malformed: the read model refuses
  nondeterministically, and this card proves that a refusal refuses the
  packet
- what makes an AEAD tag wrong: the decrypt model fails nondeterministically
- what makes a salt mismatch: the memcmp model mismatches
  nondeterministically

For each of these, the harnesses prove that an accepted packet is one the
model passed.

## ESBMC

ESBMC 8.5.0 was also tried on the same harness, through a C++17 wrapper
(it parses C++ only from a .cpp file). It cannot run the create path:
`rocketnet_client_protocol_init` memsets the 307,000-byte client struct,
and ESBMC's memset is a byte loop. At `--unwind 64` it fails that loop's
unwinding assertion. With the loop's own bound (`--unwindset 8:307001`) and
`--z3`, Z3 ran out of memory under a 16 GB limit after 245 s on
harness_process_cookie. CBMC models memset natively, so these proofs use
CBMC.
