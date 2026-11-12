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
- **Wiping.** The guarantee is the portable reference's: named buffers of
  key, state and keystream are cleared, and nothing more is promised.
  - *Cleared with `sodium_memzero` before return:*
    - the ChaCha20 state (the key words) and the Poly1305 key, accumulator
      and pads;
    - the key block, the computed tag, and the partial-chunk and
      partial-block buffers (`ks`, `block`, `tmp`);
    - the named arrays of each ChaCha20 step: `in_v` and `x` (four-block
      and one-pass steps), `a_in`, `b_in`, `x` and `y` (eight-block step),
      `in` and `x` (scalar block), and the transposed keystream rows
      (`row`).
  - *Not cleared:*
    - values the compiler keeps in vector or general registers;
    - values it spills to unnamed stack slots. The eight-block step holds
      more live vectors than there are registers, so it spills.
    - scalar locals: the Poly1305 limbs `h0`-`h2`, `r0`, `r1`, `s1`, and
      the counters. At `-O0` these live on the stack.
  - *The portable reference* clears its ChaCha20 context, the AEAD's key
    block, its Poly1305 state and its computed tag. It leaves its block
    words, its `tmp` partial block and any spills. Neither path promises
    that no key-derived value survives in a stack frame.
- **Speculation**: open checks the tag before it computes any plaintext. It
  MACs the ciphertext, compares in constant time, and only on success runs
  `ACQUIRE_FENCE` and then the NEON keystream from block 1 over the
  ciphertext, as the composed open does. A forgery's plaintext is never
  computed, so no read of the output during or after a refused call can see
  keystream. The one pass (MAC and cipher in the same step) is used for seal
  only, where every byte it writes is ciphertext.

## A refused open

`crypto_aead_chacha20poly1305_ietf_decrypt` returns -1 without writing a
byte of plaintext under either implementation (`local/dispatch.h.in`):
- the composed AEAD zeroes the output, as upstream does;
- the AArch64 open leaves the output as it was.

Input and output must be exactly in place or apart; partial overlap is not
supported. `local/dispatch.h.in` states this, and the SDK asserts it at seal
and open (`sdk/rocketnet_protocol.cpp`).

The associated data may lie inside the output: seal absorbs it before
writing any byte, as the composed seal does.

Above upstream's bound of 64 * (2^32 - 1) bytes, the AArch64 seal and open
call `sodium_misuse()` before touching any data.

## Checks

### `bin/test_sodium_dispatch`, every commit

It runs on the macOS arm64 runners in Debug (the unit row), Release and
asan + ubsan (the `sodium-arm64` rows), and on every x86 row. It checks:

- **Vectors:** RFC 8439, the 316 Wycheproof vectors, and the Poly1305 carry
  edges.
- **Agreement:** every implementation set against the composed reference at
  every length 0 to 4096.
- **Forgery:** a flipped bit in the tag, the ciphertext or the associated
  data, at every length 0 to 4096 that is 0, 1 or 15 mod 16; apart, exactly
  in place, and detached. Each must be refused with length 0, and nothing
  outside the output may be written.
  - *AArch64 open:* the output is read-only for the call, so a write that is
    undone before return still faults by name.
  - *Composed open:* the output ends zeroed, and the open runs no ChaCha20
    over the message (the test seam counts its IETF xor calls, and a good
    open counts one).
- **Associated data inside the output:** seal gives the same bytes as with a
  separate copy.
- **The sweep:** every length 0 to 4096, seal and open, apart and exactly in
  place. The 16 guard bytes after every output must be untouched.
- **ChaCha20 entry points the AEAD does not use**, against the reference:
  - the 8-byte-nonce stream;
  - the 64-bit counter started at 0xFFFFFFFB to 0xFFFFFFFF, so the carry
    from word 12 to word 13 falls inside both four-block halves of the
    eight-block step, and its wrap at 2^64;
  - the IETF counter at its last blocks.
- **The bound:** the length check at 64 * (2^32 - 1) and one byte above it,
  through a seam and with no data. Above the bound, `sodium_misuse()` must be
  called for seal and open.
- **Built and picked:** on 64-bit ARM Linux and macOS, the AArch64
  implementations must be built and, unless the forced fallback is on,
  picked. Otherwise the test fails by name. `tools/check_sodium.py` asserts
  the same picks for the oracle library.

### Nightly

The exhaustive sweep (`--sweep-full`) and a 10,000,000-case differential,
on macOS arm64 and Linux x64.

### The SDK

A DEV build with the bundled pair poisons an apart output before
`rocketnet_open_sealed_packet` opens. It then asserts that a refusal left
every byte poison under neon-fused, or zero under the composed AEAD, going
by the pick the pair reports.

Every DEV build then poisons a refused output (`sdk/rocketnet_protocol.h`).
`test_sealed_packet_refusals` and
`test_server_payload_capacity_uses_live_session_path` check this.

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

### Receipts at 3a90b5d89 (2026-11-12)

Four runs: Neoverse-V2 and Neoverse-N1, each built with gcc and with clang. All four exit 0 with this valgrind summary:

    ERROR SUMMARY: 0 errors from 0 contexts (suppressed: 0 from 0)

**Tools**
- gcc (Ubuntu 15.2.0-16ubuntu1) 15.2.0
- Ubuntu clang version 21.1.8 (6ubuntu1)
- valgrind-3.26.0
- Linux 7.0.0-1016-aws

**Environment.** Every run had `GLIBC_TUNABLES=glibc.malloc.hugetlb=0`. Secret tracking is on because CMake found `valgrind/memcheck.h` and defined `ROCKETNET_SODIUM_SECRET_TRACKING=1` on `test_sodium_secrets`.

**Commands.** Configure, build and run, with `<cc>` either `gcc`/`g++` or `clang`/`clang++`:

    cmake -S src -B <w> -DCMAKE_BUILD_TYPE=Release -DROCKETNET_DEV=ON \
      -DCMAKE_COMPILE_WARNING_AS_ERROR=ON -DCMAKE_C_COMPILER=<cc> \
      -DCMAKE_CXX_COMPILER=<c++> -DCMAKE_RUNTIME_OUTPUT_DIRECTORY=<w>/bin
    cmake --build <w> --parallel 16 --target test_sodium_secrets test_sodium_dispatch
    valgrind --error-exitcode=1 <w>/bin/test_sodium_secrets

The build had 0 warnings.

**Sets.** Every run covered five sets, with 15 sizes each:
- composed, ref, ref
- composed, neon, ref
- composed, ref, mul64
- composed, neon, mul64
- neon-fused, neon, mul64

| host | instance | gcc run (UTC) | clang run (UTC) |
|---|---|---|---|
| Neoverse-V2, m8g.4xlarge | i-0ebc69c9a8290548e | 22:53:32Z, exit 0 | 22:53:32Z, exit 0 |
| Neoverse-N1, c6gn.4xlarge | i-0970835fe02a146fe | 22:54:30Z, exit 0 | 22:54:30Z, exit 0 |
