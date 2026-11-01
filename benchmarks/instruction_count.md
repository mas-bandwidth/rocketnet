# Deterministic instruction-count gate

`benchmarks/instruction_count.cpp` and `tools/check_instructions.py` implement a
deterministic instruction-count gate for the client per-frame main-thread path
and the server tick. Unlike `benchmarks/client_main_thread_cost.cpp`, which uses OS
sockets, a background receive thread, and `std::chrono::steady_clock` on a
quiet nightly host, `instruction_count` executes in a single process on the
in-memory `loopback` transport (`sdk/rocketnet_driver_loopback.h`) with no
sockets, no background threads, no wall clock, and a fixed random seed, so it
can gate every push in CI.

## The scenario

Both regions initialize the runtime identically before `rocketnet_init(NULL, NULL, NULL)`:

- A deterministic `randombytes_implementation` seeded from a fixed 64-bit
  constant (`0x524F434B45544E54`) supplies every key, nonce, salt, and
  hash-table seed so open-addressing probe lengths and cryptographic paths are
  identical across runs.
- The simulated clock starts at `1000.0` and advances by a fixed `dt = 0.01`
  per step. Both client and server configure `token_epoch_unix_seconds = 1000`, and
  every client configures `time_callback` to read the simulated clock and
  `step_driven = true` so all I/O steps execute synchronously on the caller
  thread.

### Region `client_frame`

One server (`max_clients = 2`) and one step-driven client connect over the
`loopback` transport. Each frame writes `P = 32` payloads of `200` bytes with
`rocketnet_client_transport_start_packet` and
`rocketnet_client_transport_finish_packet`, then runs:

- `rocketnet_client_transport_send_packets`
- `rocketnet_client_transport_update`

After `32` warm-up frames, `64` frames are measured. Only `send_packets` and
`update` on those `64` measured frames are inside the instrumented window
(matching the window timed by `client_main_thread_cost`). The payload writes,
the lockstep server update and `200`-byte replies, and the client receive drain
run outside the instrumented window.

### Region `server_tick`

One server (`max_clients = 16`, on a loopback hub of 16 clients) and
`16` step-driven clients connect over the `loopback` transport. Each tick, every
connected client writes `4` payloads of `200` bytes (`64` datagrams total) and
flushes them into the server's loopback endpoint queue outside the instrumented
window. After `16` warm-up ticks, `64` server ticks are measured. Inside the
instrumented window, each measured tick runs:

- `rocketnet_server_transport_update`
- walking `rocketnet_server_transport_payloads` and framing one `200`-byte
  reply per received payload
- `rocketnet_server_transport_send_packets`

The clients then step and drain the replies outside the instrumented window.

### The server's stats

The server transport keeps its per-slot ack statistics current on the ack path
itself (running aggregates, proofs/tla/RNServerStats.tla), with no worker thread, so
every instruction of a server tick runs on the measured thread inside the window.

### Region isolation

When built on a host with `<valgrind/cachegrind.h>`, `instruction_count` uses
`CACHEGRIND_START_INSTRUMENTATION` and `CACHEGRIND_STOP_INSTRUMENTATION` with
`valgrind --tool=cachegrind --instr-at-start=no --cache-sim=no --branch-sim=no`
(`isolation mode=client_requests`) so only instructions inside the measured
window are counted. When compiled without `<valgrind/cachegrind.h>`, the
checker falls back to `isolation mode=subtract_setup`, running `<region>` and
`<region> setup` (warm-up only, zero measured units) and subtracting the setup
instruction count.

## The threshold and verdict rules

`tools/check_instructions.py` compares the measured `Ir` count for each region
against `benchmarks/instruction_baseline.json`:

- **Toolchain match**: `toolchain_id`, `compiler_id`, `valgrind_version`, and
  `build_type` must match the baseline exactly. Any difference fails immediately naming the
  mismatched field and the pinned toolchain, because instruction counts across
  different compiler or valgrind versions are not comparable.
- **Regression threshold (`+2.0%`)**: if any region exceeds its baseline `Ir`
  by more than `2.0%`, the checker prints `verdict=FAIL` for that region and
  exits `1`.
- **Improvement note (`-2.0%`)**: if any region is more than `2.0%` under its
  baseline `Ir`, the checker prints `verdict=PASS` along with a `NOTE`
  suggesting a baseline re-pin so future regressions cannot hide inside the
  headroom gained by an optimization.

Output format:

```
isolation mode=client_requests
instructions region=client_frame ir=<n> per_unit=<n> baseline=<n> delta_pct=<+x.xx%> verdict=PASS|FAIL
instructions region=server_tick ir=<n> per_unit=<n> baseline=<n> delta_pct=<+x.xx%> verdict=PASS|FAIL
```

## The pinned toolchain

The gate never measures with a host's gcc or valgrind. CI reads the two
versions from `benchmarks/instruction_baseline.json`, builds them with
`tools/build_pinned_toolchain.sh` from release source tarballs checked against
the sha256 pinned in that script, and caches the prefix keyed by the two
versions, the host OS and C library, and the script. The build and the
measurement then name every tool by its path in that prefix:

```sh
bash tools/build_pinned_toolchain.sh "$PWD/pinned-toolchain" 15.2.0 3.26.0
tc="$PWD/pinned-toolchain"
cmake -B build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER="$tc/bin/gcc" -DCMAKE_CXX_COMPILER="$tc/bin/g++" \
  -DCMAKE_C_FLAGS="-I$tc/include" -DCMAKE_CXX_FLAGS="-I$tc/include" \
  -DCMAKE_EXE_LINKER_FLAGS="-Wl,-rpath,$tc/lib64"
cmake --build build --target instruction_count --parallel
python3 tools/check_instructions.py --bin ./build/instruction_count --toolchain "$tc"
```

`-I$tc/include` puts the pinned valgrind's `cachegrind.h` ahead of any the host
carries, and the rpath loads the pinned gcc's own `libstdc++`. The script writes
`$tc/toolchain-id` (both versions and the first 12 hex digits of the script's
sha256); `--toolchain` measures with that prefix's valgrind and checks the id
against the baseline's `toolchain_id`, so a host or distro gcc that reports the
same version is never compared, and any change to the script is a new
toolchain that needs a re-pin. A cold cache
builds gcc (C and C++ only, one stage) and valgrind; a warm cache restores the
prefix and builds nothing. Moving to a new gcc or valgrind is a re-pin: add
its sha256 to the script, then re-pin the baseline with the new toolchain.

## Re-pinning the baseline

When an intentional protocol change, hot-path optimization, or CI runner image
upgrade moves the instruction count or changes the toolchain version:

1. Build `instruction_count` in `Release` (`-ffp-contract=off`) with the pinned
   toolchain, as above, and run `--repin` with a mandatory `--reason`:

   ```sh
   python3 tools/check_instructions.py --bin ./build/instruction_count \
     --toolchain "$tc" --repin --reason "<why the baseline moved>"
   ```

2. Commit `benchmarks/instruction_baseline.json` alone in its own commit with the
   same stated reason.

### Why a re-pin requires a stated reason

`--repin` refuses to run without `--reason <text>` or without `--toolchain`, and
records the reason, git commit SHA, toolchain ID, compiler ID, valgrind version,
and build type inside
`benchmarks/instruction_baseline.json`. Requiring an explicit reason prevents silent
baseline ratcheting: every baseline shift in git history records whether the
count moved because of a deliberate wire/protocol change, a compiler upgrade,
or an optimization ratcheting the ceiling down.
