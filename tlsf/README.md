# RocketNet's tlsf

Each RocketNet client owns one heap: tlsf, Matthew Conte's two-level
segregated fit allocator, over memory carved from the one block the client's
create takes from the installed allocator (`sdk/rocketnet_allocator.h`, "the
instance heap"). Every allocation the client makes after create comes from
it, so connect calls the installed allocator not at all.

The code is not in this directory: it is a generated section of
`sdk/rocketnet_allocator.cpp`, between the lines
`// BEGIN VENDORED TLSF` and `// END VENDORED TLSF`, so RocketNet's allocation
lives in one place. This directory holds its license, which installs with the
SDK, and this note.

## Provenance

Upstream is <https://github.com/mattconte/tlsf> at
`deff9ab509341f264addbd3c8ada533678591905` (tlsf 3.1), the files `tlsf.c`
(SHA-256 `2a0f8cfc9cfe6114ccdc6cf22339059440b16f1149b5107bead4ae4c3a0d50e2`)
and `tlsf.h` (SHA-256
`f7f73c48810ba60203095667c226e5a600a6ea0f69afba48efff6efbaa628d4f`). The
license is BSD-3-Clause (`LICENSE`); the original notice is kept in the
section, and the file's header repeats it.

framework's `game/ThirdParty/tlsf` was not used as the source: it is
reformatted and renames `next_free` to `flow_free`, so it is not upstream.

## Reproducing the section

    python3 tools/generate_tlsf.py --fetch            # write the section
    python3 tools/generate_tlsf.py --fetch --check    # refuse a section that differs

The Linux and macOS Release unit legs in CI run the check. The section is never edited by hand:
change the generator and run it.

## What the generator changes, and nothing else

- `tlsf.h` joins `tlsf.c` in place of its include; its guard, its
  `<stddef.h>` and its `extern "C"` block go, and the system includes move
  above the section's namespace (a compiler intrinsics include keeps the
  condition of the `#elif` that selects it).
- Every name upstream declares, in code and never in comments or strings,
  takes RocketNet's prefix: each `tlsf` and `_tlsf` name becomes
  `rocketnet_tlsf`, and `pool_t`, `test_ffs_fls`, `TLSF_64BIT`,
  `INCLUDED_tlsf` and `printf` become `rocketnet_tlsf_pool_t`,
  `rocketnet_tlsf_test_ffs_fls`, `ROCKETNET_TLSF_64BIT`,
  `ROCKETNET_INCLUDED_TLSF` and `rocketnet_tlsf_printf` (RocketNet's log). The
  whole section is in `namespace rocketnet_tlsf`, so its internal types are
  RocketNet's own too. Nothing can collide with an application's own tlsf.
- `FL_INDEX_MAX` is 38 on 64-bit targets (upstream 32, with upstream's own
  note that it can be raised), so one pool holds a whole heap, a 1000-client
  server's included. `FL_INDEX_COUNT` is then 31, so every first-level index
  and its successor still shift an `unsigned int` by at most 31.
  `rocketnet_heap_malloc` refuses a single request of 2^34 bytes or more,
  below the size where upstream's size-class rounding would shift an `int`
  past its width.

## Warning adaptations

The SDK compiles the section as C++ with warnings as errors at `-Wall -Wextra
-Wpedantic` and the strict set (`-Wshadow -Wconversion -Wsign-conversion
-Wcast-qual -Wformat=2 -Wundef -Wdouble-promotion`) on GCC and Clang, and `/W4`
on MSVC. Each warning the pinned input raises is fixed by one exact
replacement in the generator that must match once; none changes arithmetic:

- `-Wsign-conversion`: explicit casts where upstream converts implicitly
  between `int`, `unsigned int`, `size_t` and `ptrdiff_t` (the builtin
  `ffs`, `fls` of the high and low words, the size-class rounding mask, the
  integrity check's bitmap words, and the negative offsets to a pool's first
  block, which wrap to the same address as before).
- `-Wcast-qual`: the user pointer and its block header are a fixed offset
  apart; the offset is applied to the address as an integer instead of
  through a cast that drops `const`.
- `-Wformat-pedantic`: the walker's `%p` gets a `void *`.
- `-Wundef`: `#if _DEBUG` becomes `#if defined (_DEBUG) && _DEBUG`.
