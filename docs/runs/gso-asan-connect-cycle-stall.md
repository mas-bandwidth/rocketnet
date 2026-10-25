# The ASan connect-cycle stall in the gso/gso suite, rerun

The record of rerunning the one AddressSanitizer failure in
docs/runs/linux-transport-cells-50x.md: under the gso/gso suite,
`test_posix_connect_disconnect_1000` printed "connect stalled at cycle 621",
failed its check at `functional/posix_lifecycle_stress.cpp:1106`, and the log
ended with a LeakSanitizer report (2609752 bytes in 68 allocations, all from
`plan_rss_malloc`). That run was taken at load 44 to 85 on a 16-processor
bench; its log directory is gone, so the stall's diagnostic lines (client
state, packets and counters, printed by `posix_print_stall`) were never kept.

- Date (`date -u`): Sun Oct 25 03:25:08 PM UTC 2026
- Head: b159a67c (`git log --oneline -1`: `b159a67c land rn-live-matrix-bound-time-scale (sprint stream rocketnet-final)`); every run below is at this head
- Machines: two 16-processor Linux benches, `uname -r` 7.0.0-38-generic, gcc 15.2.0
- Builds: `cmake -DCMAKE_BUILD_TYPE=Debug -DROCKETNET_SANITIZE_ADDRESS=ON` (first bench) and `-DCMAKE_BUILD_TYPE=Release -DROCKETNET_SANITIZE_ADDRESS=ON` (second bench, the build type of the original run), target `functional`, `--parallel 4`

## Which cell stalled

The failing line names the cell `[server portable, client portable]`, not
gso. The suite pins `ROCKETNET_SERVER_TRANSPORT=gso ROCKETNET_CLIENT_TRANSPORT=gso`,
but every POSIX lifecycle case clears both pins before its cell
(`posix_cell_begin`, `functional/posix_lifecycle_stress.cpp:550-554`) and sets
the cell's transport in the config instead. On Linux the case is registered
once per server cell; the stalled one is
`test_portable_transport_posix_connect_disconnect_1000`
(`functional/rocketnet_functional_tests.cpp:9903`): a portable server with
128 slots and a portable client, cycled 1000 times. The gso transport is not
in that case. Every suite case runs in its own process
(`tests/suite_runner.h`), so `./bin/functional --run <case>` is the same
process shape as the suite run.

## Reruns

Every run: `ROCKETNET_TEST_VERBOSE=1 ROCKETNET_SERVER_TRANSPORT=gso ROCKETNET_CLIENT_TRANSPORT=gso ./bin/functional --run <case>` through the bench slot lock.

```
TOTAL dbg-portable case=test_portable_transport_posix_connect_disconnect_1000 type=Debug pins=gso/gso runs=10 pass=10 fail=0 stalled_runs=0
TOTAL rel-portable-load case=test_portable_transport_posix_connect_disconnect_1000 type=Release pins=gso/gso runs=20 pass=20 fail=0 stalled_runs=0
TOTAL dbg-gso case=test_gso_transport_posix_connect_disconnect_1000 type=Debug pins=gso/gso runs=10 pass=10 fail=0 stalled_runs=0
```

- Ten runs of the stalled case, Debug ASan, first bench: 10 pass, 0 stalls.
  Every run printed `fds 5 -> 5, threads 1 -> 1, heap umems 0 -> 0` in
  4.9 to 5.2 s. Bench load 3.9 to 7.7 (from other work, falling).
- It did not reproduce in ten, so twenty more on the second bench, Release
  ASan, under load: four busy loops ran beside every run (the most the bench
  rules allow, started at load 4.8, stopped after the twentieth run). 20 pass,
  0 stalls, 3.9 to 4.0 s each, load 4.7 to 5.0 throughout.
- Ten runs of the gso server cell of the same case
  (`test_gso_transport_posix_connect_disconnect_1000`, `[server gso, client portable]`),
  Debug ASan, first bench: 10 pass, 0 stalls, load under 1.
- No run of the 40 printed a LeakSanitizer report.

Stall count: 0 in 40 runs (0 in 30 of the case that stalled).

## The leak is the harness's

`posix_check` ends the process with `exit( 1 )`
(`functional/posix_lifecycle_stress.cpp:122`) where it fails, so a failed
check inside the loop at :1106 leaves the case's server and that cycle's
client alive: neither `rocketnet_client_transport_disconnect`,
`rn_native_client_transport_stop_and_destroy` nor `posix_server_destroy` runs.
LeakSanitizer then reports their memory at exit. Every library allocation in
the functional binary goes through the counting allocator the suite installs
(`rocketnet_init( NULL, plan_rss_malloc, plan_rss_free )`, `functional/main.cpp:58`),
which is why every block's stack ends in `plan_rss_malloc`.

The evidence: a local build (not committed) with one added check that fails
at a chosen cycle, placed just before the check at :1106, and run with the
cycle set to 621 on the Release ASan build:

```
posix lifecycle stress check failed: .../functional/posix_lifecycle_stress.cpp:1106: getenv( "RN_EXP_FAIL_AT" ) == NULL || i != atoi( getenv( "RN_EXP_FAIL_AT" ) )
==2675968==ERROR: LeakSanitizer: detected memory leaks
Direct leak of 7547 byte(s) in 13 object(s) allocated from:
    #0 ... in malloc ../../../../src/libsanitizer/asan/asan_malloc_linux.cpp:67
    #1 ... in plan_rss_malloc(void*, unsigned long) (.../bin/functional+0x83409)
Indirect leak of 2487977 byte(s) in 59 object(s) allocated from:
    #0 ... in malloc ../../../../src/libsanitizer/asan/asan_malloc_linux.cpp:67
    #1 ... in plan_rss_malloc(void*, unsigned long) (.../bin/functional+0x83409)
SUMMARY: AddressSanitizer: 2495524 byte(s) leaked in 72 allocation(s).
```

The same shape as the original report (about 2.5 MB in about 70 blocks, two
frames, all `plan_rss_malloc`): the default fast unwinder stops at the
allocator callback. With `ASAN_OPTIONS=fast_unwind_on_malloc=0:malloc_context_size=12`
the 70 blocks of that run (2059094 bytes) each name their creator; one stack:

```
Direct leak of 4199 byte(s) in 1 object(s) allocated from:
    #1 ... in plan_rss_malloc(void*, unsigned long)
    #2 ... in rocketnet_malloc
    #3 ... in rocketnet_server_transport_create_internal(void*, char const*, rocketnet_server_transport_config_t const*, double, int*, unsigned long const*)
    #4 ... in rocketnet_server_transport_create_ex
    #5 ... in rocketnet_test_posix_connect_disconnect_1000()
    #6 ... in rocketnet_test_posix_connect_disconnect_1000_portable()
    #7 ... in rocketnet_functional_run_named(char const*)
```

Counted over all 70 stacks: 53 pass through `rocketnet_server_transport_create_ex`
called from the case (the one server it creates before the loop) and 17
through `rocketnet_client_transport_create_ex` called from
`posix_client_create` (the cycle's live client); none from anywhere else. And
no passing run of the 40 above reported a leak, since those runs destroy both
before the process ends. So the reported leak is the live server and client
of an aborted case: the harness's exit path, not a library defect. It carries
no information about the stall.

## The stall

Not reproduced: 0 stalls in 40 runs at current main, on both build types the
case can be built with under ASan. The cause is not traced; there is no log of
it to trace. What is known:

- The failing cell is portable/portable, so the gso transport is ruled out as
  the stalled path.
- A stall is reported after 5 s of platform time without the client reaching
  CONNECTED (`posix_wait_connected( ..., 5.0 )`, :1104). A connect here takes
  about 5 ms (1000 cycles in 4 to 5 s including disconnects), so the stall is
  about a thousand times a normal connect.
- The token history cannot fill (128 slots hold 1024 used tokens, more than
  the 1000 cycles, :1085-1090), and each cycle writes a fresh token from the
  wall clock with a 30 s lifetime, so neither the history nor token expiry
  explains it.
- The original run was at load 44 to 85 on 16 processors, five times
  oversubscribed, under ASan. That load was not recreated here: the bench
  rules cap a repro at four busy loops while load is under 8, so the loaded
  runs above reached load 5, not 44.

## Not done

- The stall's cause: not reproduced in 40 runs, so not traced to a file and
  line. No library defect is named and no fix is proposed.
- The original run's load (44 to 85 on 16 processors) was not recreated.
- The stall's own diagnostic lines (`posix_print_stall`) from the original
  run do not exist anymore; a future record of this case keeps the whole
  suite log, since those lines name the side that stopped.
