# AArch64 ChaCha20-Poly1305: constant-time review

The review of `local/arm64.c.in` (README.md, "AEAD dispatch and AArch64"):
what the code may branch on and index by, how that was checked, and where a
failed open writes. A change to `local/arm64.c.in` repeats the reading below
and the secret-tracking run.

## The rule

No branch, no memory index and no variable-time instruction depends on a
secret: the key, the ChaCha20 keystream, the Poly1305 key and accumulator,
the message, the associated data, the ciphertext before its tag is checked,
or the tag. The one place a secret decides control flow is the tag check's
result, 0 or -1, which is the public outcome the caller is told.

## By reading

- **Branches** test only lengths (`len`, `n`, `first`, `off`, `absorbed`,
  `len - off`), block counts (`pdone < pblocks`, `blocks > 0`), whether `m`
  or `in` is NULL, the Poly1305 buffer fill `leftover` (a count of bytes),
  and the public block counter. The loop bounds are lengths.
- **Memory indexes** are lengths and loop counters. The one table,
  `rn_neon_rotl8_index`, is read whole into a register at a constant
  address; `vqtbl1q_u8` then permutes register lanes, which is not a memory
  access.
- **Instructions**: ChaCha20 uses vector add, eor, shl, sri, rev32, tbl, trn
  and the scalar add, eor, ror for the tail block; Poly1305 uses mul, umulh,
  adds, adc, and, shifts. These are instruction classes Arm lists as data
  independent under its Data Independent Timing feature (Armv8.4 and later,
  when PSTATE.DIT is set; this code does not set it, and N1 predates it), no
  data-dependent latency is documented for them on Neoverse or Apple cores,
  and they are the classes the portable donna64 Poly1305 already compiles to
  on AArch64. Setting DIT around the AEAD is a possible hardening, not done.
- **Final reduction**: `h - p` is kept by a mask built from the carry out of
  `h + 5`, never by a branch.
- **Tag check**: `crypto_verify_16`, the bundle's constant-time compare.
- **Wiping**: keystream, the Poly1305 key, state and pads, and the partial
  blocks on the stack are wiped with `sodium_memzero`. Vector registers are
  not wiped, as on the x86 paths.
- **Speculation**: after a tag check passes, the one pass runs
  `ACQUIRE_FENCE` before it returns, as the composed open does before it
  decrypts. The composed open never computes plaintext for a forgery; the one
  pass computes it, writes it to `m` and zeroes it before it returns -1. A
  forgery's plaintext is the keystream for the forged nonce XOR bytes the
  sender chose, so the bytes in `m` between the write and the zeroing are
  keystream for that nonce. They are readable only by code that reads `m`
  during the call, which is the reason for the next section.

## Where a failed open writes

`crypto_aead_chacha20poly1305_ietf_decrypt` writes `m` before the tag check
in the one pass; `m` is defined only when it returns 0, and holds zeros when
it returns -1 (`local/dispatch.h.in`). In the SDK every open goes through
`rocketnet_open_sealed_packet` (`sdk/rocketnet_protocol.cpp`), the one AEAD
open outside the tests. Its callers and where each opens (audited
2026-11-12, at the commit that added this file):

| caller | destination | readable by anyone else before the open returns? |
|---|---|---|
| `sdk/rocketnet_client_io.h`, the client I/O thread's open | the stack array `opened`; copied into the packet buffer only on success | no |
| `sdk/rocketnet_client_protocol.cpp`, the challenge | the stack `server_ephemeral_public_key` | no |
| `sdk/rocketnet_client_protocol.cpp`, the two denied opens | the stack `opened` | no |
| `sdk/rocketnet_client_protocol.cpp`, the session open | `opened`, passed by its four callers, each a stack array | no |
| `sdk/rocketnet_server_protocol.cpp`, `process_received` | `opened`, passed by its callers: | |
| - `sdk/rocketnet_server_protocol.cpp`, the stack caller | a stack array | no |
| - `sdk/rocketnet_server_transport.cpp`, `rocketnet_server_receive_one` | the next payload slot, `payload_storage + num_payloads * SLOT`, or the stack `payload_overflow` when the batch is full | no: see below |

The server's payload slots are owned by the server transport and touched on
the thread that runs `rocketnet_server_transport_update` (the application's
own call). `update` resets `num_payloads` to 0 and then receives, and slot
`num_payloads` is published only by `num_payloads++` after an accepted open.
The application reads payloads through `rocketnet_server_transport_payload`,
which refuses an index at or past `num_payloads`, and between updates, not
during one; the driver threads (XDP, io_uring, GSO, RIO, IOCP and the
portable receive thread) fill receive frames and never touch
`payload_storage`. So no caller decrypts into memory that another thread or
the application can read, and none keeps verify-then-decrypt.

Checks that hold this:

- `bin/test_sodium_dispatch`: for every implementation set, at every length
  0 to 2048 that is 0, 1 or 15 mod 16, a forged packet (tag, ciphertext or
  associated data) is refused with the reported length 0, every output byte
  zero and nothing past the output written, into a separate buffer, in
  place, and through the detached function.
- A DEV build's `rocketnet_open_sealed_packet` asserts a failed open left
  zeros, then fills the output with `ROCKETNET_OPEN_POISON_BYTE`
  (`sdk/rocketnet_protocol.h`). `test_sealed_packet_refusals` and
  `test_server_payload_capacity_uses_live_session_path` (a forged payload into
  the server's open buffer: nothing handed out, the replay window and the
  delivered count unchanged, the buffer poisoned) check it.
- A new caller of `rocketnet_open_sealed_packet` must open into memory private
  to it until the call returns, and this table gets its row.

## By tool: secret tracking

`bin/test_sodium_secrets` (`tests/test_sodium_secrets.c`, built when
`ROCKETNET_SODIUM_SECRET_TRACKING` is on, Linux with the valgrind headers)
runs under `valgrind --error-exitcode=1`. It marks the key, the message and
the associated data undefined, and the ciphertext and tag undefined before
open, then seals and opens every size in {0, 1, 15, 16, 63, 64, 65, 191,
192, 193, 200, 256, 257, 1200, 1400} through every implementation set the
CPU runs; a branch or memory index on an undefined byte is an error. The tag
check's result is the one declassification: the bundle marks the 0/-1 of
`crypto_verify_16` defined in a secret-tracking build only
(`SODIUM_DECLASSIFY` in `local/dispatch.h.in`), so the run is clean when the
code is. The nightly row runs it on a Linux arm64 runner. Recorded runs:

- 2026-11-12, Neoverse N1 (c6gn), valgrind 3.26.0, the tracer build: 30
  errors from 2 contexts, both the branch on the `crypto_verify_16` result
  (the composed open's and the one pass's), nothing else.
- 2026-11-12, Neoverse V2 (c8g), valgrind, the tracer build: 18 errors from
  1 context, the composed open's branch on the `crypto_verify_16` result.
