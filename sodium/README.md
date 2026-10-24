# RocketNet sodium subset

The SDK compiles `sodium.c` and includes `sodium.h`. Ordinary builds need no
system sodium, generator or download. This is a protocol-focused subset, not a
replacement for the complete libsodium API. Do not include this private header
and a system sodium header in the same translation unit.

## Provenance

The input baseline is the byte-for-byte netcode pair at
`fc473eb628e080acaaebed2659e1f9ee85c6a7c2`, also shared by yojimbo. Its upstream
provenance and 1.0.21/1.0.22 hardening patches are recorded in netcode's
`sodium/NOTES.md`. It contains optimized ChaCha20/Poly1305, random, core,
runtime and utilities. We verify its two SHA256 values before extending it;
we do not claim to reconstruct its historical local patches from upstream.

The extension comes from the official libsodium 1.0.22 release archive, whose
SHA256 is `adbdd8f16149e81ac6078a03aca6fc03b592b89ef7b5ed83841c086191be3349`
and release commit is `77e1ce5d6dee871c49ef211222ba18ef0c486bda`.
`manifest.json` records both input pins and the exact 34 added C sources.
They supply BLAKE2b, SHA-512, SipHash-2-4, X25519, Ed25519, XSalsa20,
secretbox and sealed boxes. Unneeded password hashes, AEAD families and
external assembly are omitted. X25519 uses upstream ref10 (51-bit limbs when
the compiler has 128-bit arithmetic, 25.5-bit otherwise); BLAKE2b retains its upstream runtime-selected C intrinsics implementations.
Salsa20 uses upstream's portable reference implementation: the x86 SIMD tail
uses unaligned typed loads that UBSan correctly rejects on frozen token input.
Selecting the reference avoids that C alignment violation without changing
cryptographic arithmetic or suppressing sanitizer checks. Salsa20 serves sealed
boxes; the packet AEAD remains on optimized ChaCha20/Poly1305. ChaCha20
and Poly1305 retain the baseline's optimized paths.

The ISC license is in `LICENSE`; original source notices and the BLAKE2 CC0
notice remain in the generated files. Installing the SDK installs the sodium
license too. The private symbol prefix prevents collisions when an application
also links its own libsodium; the SDK shared library exports no sodium symbols.

## Warning adaptations

The SDK builds `sodium.c` with the rest of the tree, with warnings as errors
at `-Wall -Wextra -Wpedantic` (GCC, Clang) and `/W4` (MSVC), and no flag or
pragma is added for it. Each warning the pinned inputs raised there is fixed
at its input by the generator (`BASELINE_ADAPTATIONS`, `UPSTREAM_ADAPTATIONS`
and the manifest's `renames` in `tools/generate_sodium.py`); every
replacement must match exactly once, so a re-vendor that changes the text
fails loudly and the edit is reviewed again:

| Input | Warning | Edit |
| --- | --- | --- |
| baseline `sodium.h` (`uint128_t`) | `-Wpedantic` (`__int128` is an extension) | `__extension__ typedef` |
| baseline `sodium.h` (`ACQUIRE_FENCE`) | `-Wtsan` (GCC: ThreadSanitizer does not support a standalone fence) | a ThreadSanitizer build (`__SANITIZE_THREAD__`, test builds only) takes upstream's no-fence fallback; the fence is a speculation barrier, not synchronisation |
| baseline `sodium_verify.c` | `-Wunknown-pragmas` (Clang: no GCC target pragma) | the SSE2 target pragma is GCC-only; the block already requires `__SSE2__` |
| `crypto_core/ed25519/ref10/ed25519_ref10.c` | `-Wunused-function` | `ge25519_p3_to_precomp`, never called by the subset, removed |
| `crypto_core/ed25519/ref10/ed25519_ref10.c` | C4459 (local `d` hides the curve constant `d`) | the local in `ge25519_is_canonical` and `ristretto255_is_canonical` is `d_` |
| `crypto_scalarmult/curve25519/scalarmult_curve25519.c` | C4459 (amalgamation: the local `d` hides ed25519's file-scope `d`) | `d` added to the file's `renames` |
| `crypto_generichash/blake2b/ref/blake2b-compress-avx2.h` | `-Wunused-function` | `LOADU64`, never called, removed |
| `crypto_generichash/blake2b/ref/blake2b-ref.c` | C4245 (`int` -1 into `uint64_t`) | `(uint64_t) -1`, the same value |
| baseline `sodium.h` (`sodium_private_common.h`) | none: it turned C4127, C4244, C4668 and C4456 off for every file including `sodium.h` | the four `#pragma warning(disable:...)` lines removed; the warnings they hid are fixed at their sites |
| `fe_25_5/fe.h` `fe25519_tobytes`; `ed25519_ref10.c` `sc25519_mul`, `sc25519_muladd`, `sc25519_reduce`, `slide_vartime`; `salsa20_ref.c` `stream_ref`, `stream_ref_xor_ic` | C4244 (a 64- or 32-bit limb stored into a byte) | each such store spelled with the cast the implicit conversion performs, `(unsigned char) (expr)` (`NARROWING_STORES`): the same value |

The strict set (`ROCKETNET_STRICT_WARNINGS`: `-Wconversion`,
`-Wsign-conversion`, `-Wcast-qual`, `-Wundef`, `-Wshadow`) is clean too. Its
edits are pattern rules (`UPSTREAM_CONVERSIONS`, `BASELINE_CONVERSIONS`),
each applied inside one named function (or the whole input) and required to
match exactly its recorded number of times. Each spells the conversion the
language already performs at the site, so every value is unchanged:

| Input | Warning | Edit |
| --- | --- | --- |
| `ed25519_ref10.c` `sc25519_mul`, `sc25519_muladd`, `sc25519_reduce`; `fe_25_5/fe.h` `fe25519_frombytes`, `fe25519_reduce`; `ed25519_ref10_fe_25_5.h` `fe25519_mul`, `fe25519_sq`, `fe25519_sq2` | `-Wsign-conversion` (signed carry step `h -= carry * ((uint64_t) 1L << k)`: the usual arithmetic conversions make it unsigned, the store makes it signed again) | `h = (int64_t) ((uint64_t) h - (uint64_t) carry * ((uint64_t) 1L << k))` (`int32_t`/`uint32_t` in `fe25519_reduce`), the same modular step |
| the same scalar and field loaders | `-Wsign-conversion` (`int64_t` limb initialised from `load_3`/`load_4`'s `uint64_t`, below 2^32) | `(int64_t) (expr)` |
| `ed25519_ref10.c` `sc25519_*`; `fe_25_5/fe.h` `fe25519_tobytes` | `-Wsign-conversion` (signed limbs in `(lo >> j) \| (hi * ((U) 1 << i))`) | both limbs written `(U) (lo >> j)`, `(U) hi`, as the expression already converts them |
| `fe_25_5` `fe25519_reduce` (`q = (19 * h9 + ((uint32_t) 1L << 24)) >> 25`) | `-Wsign-conversion` | `q = (int32_t) (((uint32_t) (19 * h9) + ((uint32_t) 1L << 24)) >> 25)`: upstream's unsigned shift kept exactly as it computes |
| `ed25519_ref10_fe_25_5.h` `fe25519_cmov`, `fe25519_cswap` | `-Wsign-conversion` (`int32_t x &= uint32_t mask`) | `x = (int32_t) ((uint32_t) x & mask)` |
| `ed25519_ref10_fe_51.h` `fe25519_mul32`; `fe_51/fe.h` `fe25519_reduce` | `-Wconversion` (`uint128_t` into a `uint64_t` limb) | `(uint64_t)` on the stored value |
| `ed25519_ref10.c` (`fe25519_cmov`/`fe25519_cneg` flags, `equal(babs, k)`, `babs`, `negative`'s asm result, `e[i] -= carry * 16`, the canonical checks' `c`, `d_`, `n`, `s[31] ^= sign << 7`, `curve25519_A[0]` into `fe25519_mul32`) | `-Wconversion`, `-Wsign-conversion` (0/1 flags, -8..8 digits and small constants between int, unsigned int, signed and unsigned char) | the cast the assignment or argument already performs; compound assignments spelled `x = (T) (x op e)` |
| `blake2b-compress-ssse3.c`; `blake2b-compress-avx2.c` | `-Wsign-conversion` (`uint64_t` message words and counters into `_mm_set_epi64x`'s `long long`) | the message words are `const int64_t m0 = (int64_t) ...` (used only as those arguments); the counters are passed `(int64_t) S->t[0]` |
| `blake2b-compress-ref.c` | `-Wsign-conversion` (`int i` times `sizeof`) | `(size_t) i` |
| `blake2b-compress-avx2.c` | `-Wshadow` (amalgamation: the macro local `d` hides ed25519's file-scope `d`) | `d` added to the file's `renames` |
| `stream_salsa20.c` (`#if HAVE_AMD64_ASM`); baseline `sodium.c` (`HAVE_WEAK_SYMBOLS`, `HAVE_ALLOCA`, `NETCODE_CRYPTO_LOGS`); baseline `sodium.h` (`NETCODE_X64`, `NETCODE_AVX`, `NETCODE_AVX2`) | `-Wundef` | `defined(X) && X`, which `#if` already evaluates for an unset `X` |
| baseline `sodium_compare`, `sodium_add`, `sodium_sub` | `-Wconversion` (0/1 flags in `unsigned char`; `uint_fast16_t` may be 16 bits wide) | compound assignments spelled `x = (T) (x op e)` |
| baseline `crypto_verify_n` (SSE2) | `-Wcast-qual` (each load cast the `volatile` off the pointed-to data for `_mm_loadu_si128`) | the pointers stay `volatile` and point to non-`volatile` data, which every load already treated it as |
| baseline ChaCha20 (SSSE3, AVX2) and Poly1305 (SSE2) | `-Wsign-conversion`, `-Wconversion` (`uint32_t`/`uint64_t` words into and out of the intrinsics' `int`/`long long`; `~mask` into the length's type) | `(int)`, `(int64_t)`, `(uint32_t)` and `(unsigned long long) ~mask` at each site |
| baseline `_cpuid` (MSVC only), `_randombytes_linux_getrandom` | `-Wsign-conversion`, `-Wshorten-64-to-32` | `(int) cpu_info_type`; `(int) getrandom(...)` (at most 256 bytes are requested, asserted) |

None changes cryptographic arithmetic; `test_sodium` and the oracle check
below cover the result. Upstream's own `#pragma GCC diagnostic ignored
"-Wlong-long"` lines in its public headers are kept as written.

## MemorySanitizer

Two baseline writes reach memory through calls MemorySanitizer cannot see:
the raw `syscall(SYS_getrandom)` fill in `_randombytes_linux_getrandom`, and
libc's `explicit_bzero` in `sodium_memzero` (the build defines
`HAVE_EXPLICIT_BZERO` on Linux). Both left the written bytes poisoned, so
every value derived from a random fill, or read after a wipe, was reported by
the MSan gate (`tools/run_msan.sh`). The generator's `SANITIZER_ADAPTATIONS`
follow each write with `SODIUM_MSAN_UNPOISON` of exactly the bytes it wrote
(the bytes `getrandom` returned, the whole wiped range). The macro is
`__msan_unpoison` only when the compiler reports
`__has_feature(memory_sanitizer)`, and `((void) 0)` in every other build, so
no production build changes.

## Reproduce

From the repository root:

```sh
python3 tools/generate_sodium.py --inputs build/vendor-inputs --fetch --check
```

`--fetch` downloads only missing inputs and verifies their hashes before saving
them. Omit it for a fully offline check with previously downloaded inputs.
Omit `--check` to regenerate the committed pair. `--output` selects a scratch
output directory. A missing input, hash mismatch, unresolved include or changed
output fails the check.

The generator keeps architecture conditions and header guards at their original
include sites, expands implementation body headers in place, brackets recorded
static-name collisions with define/undef, and clears file-local macros. It adds
the three upstream implementation-selector calls to initialization and prefixes
private symbols through header aliases. GCC target options are pushed/popped
at each original C-file boundary; intrinsics headers are loaded before target
pragmas so their declarations do not inherit a preceding AVX2 section. The
standard `__STDC_WANT_LIB_EXT1__` feature macro is retained for the translation
unit rather than undefined between files. Existing `nonnull` declarations are refreshed from the pinned upstream headers:
the baseline carried stale annotations that rejected valid NULL/zero-length
AEAD inputs under UBSan. The fixed-vector test executes that call explicitly.
These are amalgamation adaptations;
cryptographic arithmetic and protocol encodings are unchanged.

## Reproducing the vendored inputs

The release library is built from two vendored inputs that are generated, not
written by hand. CI reproduces each from its pinned inputs and fails when the
committed bytes differ.

| Input | Reproduced by | Runs on | Does not apply |
| --- | --- | --- | --- |
| `sodium/sodium.c`, `sodium/sodium.h` (compiled into the library everywhere) | `python3 tools/generate_sodium.py --inputs build/vendor-inputs --fetch --check` (`python` on Windows), CI step "Reproduce the committed sodium pair" | Linux Release (b), macOS Release, Windows (b) | none |
| The embedded XDP object `server_xdp.o`, compiled from the sources `cmake/XdpSources.cmake` lists and embedded in the Linux library | `python3 tools/check_xdp_archive.py`, CI step "Embedded XDP object is reproducible and current": two builds give the same object, and the build tree's object equals a fresh compile | Linux | macOS and Windows embed no XDP object; there the same tool (`--committed-only` on Windows) only checks that every listed source exists |

`tests/xdp_host/linux` and `tests/xdp_host/bpf` are test-only host shims that
compile the XDP filter as ordinary C for the unit suite; they are written in
the tree, never enter the release library, and have nothing to reproduce.
`.gitattributes` forces LF on every checkout, so a Windows checkout compares
the same bytes as Linux and macOS.

## Check

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target test_sodium rocketnet_sodium_oracle --parallel
./bin/test_sodium
python3 tools/check_sodium.py bin/librocketnet_sodium_oracle.so
```

Use `.dylib` on macOS. The oracle is test-only, excluded from the normal build
and never installed. The Python checker requires an independent system
libsodium, or `--reference /path/to/libsodium.so`. It compares hashes, seeded
keys, signatures, X25519 and AEAD across empty/block-boundary/large inputs;
checks altered signatures/ciphertexts are refused; and opens each library's
randomized sealed boxes with the other library. Fixed published vectors also
run without that external dependency, including on Windows and big-endian
s390x. The normal SDK suites, ABI export and installed/embedded
consumer checks use the default vendor. CI instruments the C file as well as
the C++ SDK under sanitizers and fuzz coverage. The separate pinned-system
mode retains an independently built 1.0.20 backend compatibility check.

To update: choose explicit new inputs, review upstream changes, update the
manifest and any necessary mechanical adaptations, regenerate, run all checks,
and review the generated diff and provenance together. Never hand-edit the
pair or silently track an upstream branch.
