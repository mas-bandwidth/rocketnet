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

None changes cryptographic arithmetic; `test_sodium` and the oracle check
below cover the result. Upstream's own `#pragma GCC diagnostic ignored
"-Wlong-long"` lines in its public headers are kept as written. The strict
set (`ROCKETNET_STRICT_WARNINGS`: `-Wconversion`, `-Wsign-conversion` and
the rest) is not yet clean here; that is a known issue in ROADMAP.md.

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
