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
