# AArch64 ChaCha20-Poly1305: constant-time review

The review of `local/arm64.c.in` (README.md, "AEAD dispatch and AArch64"):
what the code may branch on and index by, what it wipes, what a refused open
writes, and how each was checked. A change to `local/arm64.c.in` repeats the
reading below and the secret-tracking run.

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
  `h + 5`, never by a branch (proved for every input by
  `proofs/cbmc/proof_poly1305_mul64.c`).
- **Tag check**: `crypto_verify_16`, the bundle's constant-time compare.
- **Wiping**: `sodium_memzero` clears, before each function returns, every
  stack copy of key or keystream material the AArch64 code makes: the input
  words and working state of each ChaCha20 step (`in_v` and `x` in the
  four-block step and in the one-pass step, `a_in`, `b_in`, `x` and `y` in
  the eight-block step, `in` and `x` in the scalar block), the transposed
  keystream rows (`row`), the partial-chunk and partial-block buffers, the
  key block, the Poly1305 key, state and pads, and the computed tag. The
  portable reference wipes less (its block words and `tmp` stay on the
  stack). Values held only in vector or general registers are not wiped, as
  on every other path.
- **Speculation**: open checks the tag before it computes any plaintext. It
  MACs the ciphertext, compares in constant time, and only on success runs
  `ACQUIRE_FENCE` and then the NEON keystream from block 1 over the
  ciphertext, as the composed open does. A forgery's plaintext is never
  computed, so no read of the output during or after a refused call can see
  keystream. The one pass (MAC and cipher in the same step) is used for seal
  only, where every byte it writes is ciphertext.

## A refused open

`crypto_aead_chacha20poly1305_ietf_decrypt` returns -1 without writing a
byte of plaintext under either implementation (`local/dispatch.h.in`): the
composed AEAD zeroes the output as upstream does, the AArch64 open leaves it
as it was. Input and output are exactly in place or apart; partial overlap is
unsupported, stated in `local/dispatch.h.in` and asserted at the SDK's seal
and open (`sdk/rocketnet_protocol.cpp`).

## Checks

- `bin/test_sodium_dispatch`, on every commit on the macOS arm64 runners in
  Debug (the unit row), Release and asan + ubsan (the `sodium-arm64` rows),
  and on every x86 row:
  - RFC 8439 and 316 Wycheproof vectors, the Poly1305 carry edges, and
    every implementation set against the composed reference at every length
    0 to 4096;
  - forgery (a tag, ciphertext or associated data bit) at every length 0 to
    4096 that is 0, 1 or 15 mod 16: refused, length 0, the output's poison
    pattern untouched by the AArch64 open (zeroed by the composed one), and
    nothing past the output written; apart, exactly in place, detached;
  - the sweep: every length 0 to 4096, seal and open, apart and exactly in
    place, with 16 guard bytes after every output checked untouched;
  - the ChaCha20 entry points the AEAD does not use (the 8-byte-nonce
    stream, and the 64-bit counter across the word-12 to word-13 carry and
    its wrap; the IETF counter to its last blocks), against the reference.
- Nightly: the exhaustive sweep (`--sweep-full`) and a 10,000,000-case
  differential on macOS arm64 and Linux x64.
- A DEV build with the bundled pair poisons an apart output before
  `rocketnet_open_sealed_packet` opens, and asserts a refusal left only
  poison or zeros; every DEV build then poisons a refused output
  (`sdk/rocketnet_protocol.h`), which `test_sealed_packet_refusals` and
  `test_server_payload_capacity_uses_live_session_path` check.

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
code is.

**The gap.** The fleet has no Linux arm64 runner, so no CI job runs this
today. The nightly `sodium-secrets-linux-arm64` row is written and waits on
the repository variable `ROCKETNET_LINUX_ARM64_RUNNER`; until a runner
exists, the receipts below are the evidence, and a change to
`local/arm64.c.in` needs a new pair of them by hand.

Receipts at the committed code (RECEIPTS_PENDING).
