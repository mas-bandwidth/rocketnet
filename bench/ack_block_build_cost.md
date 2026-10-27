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

After a Release build (`make`, or the equivalent cmake build):

```sh
c++ -O2 -std=c++11 -Isdk bench/ack_block_build_cost.cpp build/librocketnet_dev.a -lsodium -lpthread -framework CoreFoundation -framework SystemConfiguration -o /tmp/ack_block_build_cost
/tmp/ack_block_build_cost
```

Drop the two `-framework` flags on Linux.

## Results — macOS measured 2026-10-27, Linux and Windows not run

Host: `studio.local`, Apple M3 Ultra (arm64), macOS Darwin 27.2.0,
Apple clang 21.0.0, Release. One bounded bench run per code state, no
parallel load. "Before" is the base bit-by-bit builder; "after" is the
word-level builder at head. The output checksum was identical before and
after (4475265), as the equivalence case requires byte-identical blocks.

| Shape | Before (ns/build) | After (ns/build) | Linux | Windows |
| --- | ---: | ---: | --- | --- |
| sparse_loss | 2761.2 | 469.4 | NOT RUN | NOT RUN |
| dense_loss | 1154.1 | 871.1 | NOT RUN | NOT RUN |
| alternating | 544.2 | 775.8 | NOT RUN | NOT RUN |
| full_window | 2422.2 | 248.6 | NOT RUN | NOT RUN |
| empty_window | 1.6 | 2.0 | NOT RUN | NOT RUN |
| wrapped_window | 1189.0 | 817.1 | NOT RUN | NOT RUN |
| largest_below_4096 | 1170.1 | 817.9 | NOT RUN | NOT RUN |

The word scan is fastest where runs are long (full window about 9.7x,
sparse loss about 5.9x) and slower on the alternating window (about 0.7x),
where every run and gap is one bit and per-range emission dominates; the
empty window differs by about 0.4 ns in absolute terms. Linux and Windows
numbers were not run in this card and are named here as gaps, not
estimated.
