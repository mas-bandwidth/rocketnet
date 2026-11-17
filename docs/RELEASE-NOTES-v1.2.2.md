# RocketNet v1.2.2

Changes compared with the `v1.2.1` tag. A point release: no packet layout or
protocol version change, no public API or ABI change and no new counter (the
contract still holds 292 counters), and v1.2.2 endpoints interoperate with
every v1.x endpoint. No library code changed.

- Tests, sanitizers and fuzzing: `tests/msan_scale_allocator.h` maps large
  full-capacity allocations via `VirtualAlloc` on Windows AddressSanitizer
  builds (including `clang-cl` with `/MD`) while preserving carved array red
  zones and smaller heap red zones; `tests/fuzz/rocketnet_fuzz_session.cpp`
  separates the handshake timeout and silence oracle from established-session
  directional silence (with regression inputs in `tests/fuzz/corpus_session`
  and `tests/fuzz/corpus_session_raw`), and `tests/red_zones.h` keeps bulk
  state restoration out of UBSan and fuzzer coverage overhead on non-Linux ASan
  legs with 64-bit aligned word operations (`tools/fuzz_floors.tsv` also aligns
  the `rocketnet_fuzz_endpoint` MSan floor with cached-corpus throughput); and
  `tests/functional/expected_skips.txt` lists the `io_uring`-gated rows skipped
  on the `s390x` guest kernel.
- Static analysis, proofs and CI: `tools/run_static.py` runs `g++ -fanalyzer`
  across the parser translation units in parallel with deterministic diagnostic
  collection; `tlc.yml` scales rolling-seed trace worker count and timeout;
  Windows CI and nightly workflows bound per-slot MSBuild parallelism and clean
  build trees after artifact upload; `nightly-toolchains.yml` gates functional
  test execution on build success; `nightly-ubuntu-lts-vms.yml` handles
  `cloud-init` progress output and guest SSH poweroff exits cleanly;
  `tools/check_run_matrix.py` falls back cleanly on older `gh` CLIs; and
  `tools/planted_violations.py` verifies `sed` and `patch` targets in
  `--check-table`.

Upgrade: replace v1.2.1 with v1.2.2. No source change is needed.
