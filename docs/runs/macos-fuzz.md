# macOS fuzzing campaign, 2026-10-29 UTC

Source head: `a82dbf37c8c5da5ef7b387b7a02097c2bbdac5da` (main).

Machine label: `macos-bench`. macOS 27.2, arm64 (Apple silicon), 18 cores. The
campaign ran at most 5 targets at a time (half the budget the shared bench
allows; the host also serves two CI runners and was already loaded, load
average about 14 at start).

Toolchain: LLVM clang 22.1.8 (`$HOME/sdk/llvm`), the LLVM the nightly's
`fuzz-macos` leg names, because Apple clang carries no libFuzzer runtime.
CMake 3.31.

Flags, exactly as `.github/workflows/fuzz-nightly.yml` `fuzz-macos`:

- Library (`cmake -B build-fuzz -DCMAKE_BUILD_TYPE=Debug`, target
  `rocketnet_dev`): `-fsanitize=fuzzer-no-link,address,undefined
  -fno-sanitize-recover=all -fno-omit-frame-pointer`.
- Each target: `clang++ -g -O1 -std=c++17 -ffp-contract=off -DROCKETNET_DEV=1
  -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all
  -fno-omit-frame-pointer -Isodium -Isdk fuzz/<target>.cpp
  build-fuzz/librocketnet_dev.a -lpthread -framework CoreFoundation -framework
  SystemConfiguration`.
- Run: `ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`
  `<target> <corpus> <seeds> -dict=fuzz/dict/<target>.dict -max_total_time=900
  -print_final_stats=1 -rss_limit_mb=4096 -timeout=25`. The corpus directory
  started empty and the checked-in seed corpus was the second argument, as in
  the nightly. Leak detection is off for the reason in the workflow comment
  (libFuzzer's RSS thread).

Duration: 900 s per target (the nightly's default), 901 s wall each. Targets
are the ten the nightly's macOS leg runs (`rocketnet_fuzz_differential` is a
Linux-leg target).

| Target | Executions | Execs/s | Coverage (cov / ft) | Corpus files (seeds in) | Crashes |
| --- | ---: | ---: | --- | ---: | ---: |
| rocketnet_fuzz | 80,057,413 | 88,853 | 383 / 1471 | 281 (135) | 0 |
| rocketnet_fuzz_connect_token | 548,273 | 608 | 558 / 840 | 103 (18) | 0 |
| rocketnet_fuzz_receive_structured | 90,456 | 100 | 2017 / 9578 | 600 (21) | 0 |
| rocketnet_fuzz_message | 68,552,259 | 76,084 | 292 / 1188 | 174 (100) | 0 |
| rocketnet_fuzz_token_private | 162,027 | 179 | 1071 / 1675 | 68 (4) | 0 |
| rocketnet_fuzz_round_trip | 51,724,746 | 57,408 | 516 / 1093 | 295 (18) | 0 |
| rocketnet_fuzz_config | 29,777,116 | 33,048 | 373 / 875 | 168 (34) | 0 |
| rocketnet_fuzz_server_transport | 127,349 | 141 | 4128 / 16697 | 560 (155) | 0 |
| rocketnet_fuzz_client_transport | 110,051 | 122 | 4099 / 16274 | 604 (85) | 0 |
| rocketnet_fuzz_session | 13,339 | 14 | 5407 / 27507 | 1202 (22) | 0 |

Total: 231,163,029 executions; all ten processes exited 0 with no `crash-`,
`oom-`, `timeout-` or `leak-` artifact, no sanitizer report and no libFuzzer
error. The corpus count is the persisted corpus directory after the run; the
cov/ft figures are the last status line.

Result: ten targets, 900 s each, 0 crashes, 0 findings, so no regression was
added under `tests/pcapng/regress`. The low execs/s of the pair targets
(server transport, client transport, session) and the token/structured targets
is their per-input cost (real handshake and crypto work under ASan+UBSan on a
loaded host), not a stall.

Not covered: the nightly's corpus cache persists across nights; this one-off
run did not publish its corpus.
