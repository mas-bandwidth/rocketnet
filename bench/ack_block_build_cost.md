# Ack block build cost

Measurement only, not a gate. `ack_block_build_cost.cpp` times
`rocketnet_ack_block_build` over fixed-seed replay windows (splitmix64,
same seeds on every host). Every window and the output buffer are built
before timing; nothing is allocated inside the timed loop. Each shape runs
20,000 builds per repetition, five repetitions, after a 200-build warm-up;
the table quotes the median repetition in nanoseconds per build.

Shapes: sparse loss (98% received), dense loss (20% received), alternating
bits, a full window, an empty window (`highest_opened` 0), a window wrapped
across the 4096-bit boundary (largest 4223, floor 128), and a largest below
4096 (2000). Random shapes use largest 20000 unless named.

## Reproduce

Built as a CMake tool target in a Release build:

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target ack_block_build_cost
./bin/ack_block_build_cost
```

## Results — Linux and macOS Release measured 2026-10-27

- **Linux bench (`amd-1` / `b1`)**: AMD Ryzen 7 255 (x86_64), Linux 7.0.0-38-generic, GCC 15.2.0, Release (`-DCMAKE_BUILD_TYPE=Release`).
- **macOS bench (`mini-m5`)**: Apple M5 Pro (arm64), Darwin 27.2.0, Apple clang 21.0.0, Release (`-DCMAKE_BUILD_TYPE=Release`).

One bounded bench run per code state under `benchslot` with no parallel load. "Before" is the base bit-by-bit builder; "After" is the word-level builder at head. The output checksum was identical before and after (`4475265`) on both hosts, as the equivalence case requires byte-identical blocks.

| Shape | Linux Before (ns/build) | Linux After (ns/build) | macOS Before (ns/build) | macOS After (ns/build) | Bytes |
| --- | ---: | ---: | ---: | ---: | ---: |
| sparse_loss | 2224.989 | 527.256 | 1636.194 | 449.573 | 331 |
| dense_loss | 1085.460 | 1051.096 | 877.396 | 814.712 | 1027 |
| alternating | 564.517 | 1027.041 | 408.517 | 727.285 | 1027 |
| full_window | 2154.254 | 104.841 | 1287.190 | 235.510 | 7 |
| empty_window | 2.262 | 2.863 | 1.512 | 1.727 | 7 |
| wrapped_window | 850.178 | 1034.934 | 843.642 | 761.671 | 1027 |
| largest_below_4096 | 837.512 | 1040.806 | 821.092 | 752.148 | 1027 |

The word scan is fastest where runs are long (`full_window` is 20.5x faster on Linux and 5.5x faster on macOS; `sparse_loss` is 4.2x faster on Linux and 3.6x faster on macOS) and slower on the pathological `alternating` window where every run and gap is one bit and per-range emission (`max_ranges` = 255) dominates.
