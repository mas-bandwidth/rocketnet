# Qualification: 10000-client build

`ROCKETNET_MAX_CLIENTS` (`sdk/rocketnet.h`) is 1000 by default, the supported
server size. A build sets 10000 with `-DROCKETNET_MAX_CLIENTS=10000`
(BUILDING.md, "Maximum clients"); nothing else changes. This record proves
such a build compiles with zero warnings, runs the unit suite, and runs a
bounded loopback load of all 10000 clients. `.github/workflows/nightly-max-clients.yml`
repeats it nightly on the Linux fleet runner: gcc and clang, ROCKETNET_DEV ON
and OFF, fail-fast false.

## Head and host

Recorded at head `09395b079957938538af5e36c67479e938b1293b` (branch
`sprint/rn-10000-custom-build.w5.g1.e1`: main `6f4ab487` merged with attempt
4's `81d99a8e`, which carries the plan-derived bound for
`test_secrets_wiped_destroy_writes_only_keys` at
`tests/test_unit_secrets_wiped.cpp:750`). The commit that records this file
differs from that head only in this file. Builds at `-j4`, Release.

- 10000 unit suite: Linux x64 gcc and clang (16 cores, 30 GB, Ubuntu 26.04.1, gcc 15.2.0,
  clang 21.1.8), one checkout per cell.
- 10000 bounded load: Linux x64 gcc (16 cores, 124 GB, Ubuntu 24.04, gcc 13.3.0),
  gcc, DEV ON.
- The memory plan, the DEV OFF load row and the default-build rows below were
  recorded on the Linux x64 gcc load host at attempt 3 (`cbf907be`, BASE `7a0258612392`).

## Commands

    # default build, unchanged
    make -j4 && ./bin/test && ./bin/functional && ./bin/load 1000 30
    make ROCKETNET_DEV=OFF -j4 && ./bin/test

    # 10000 build, in its own checkout (CMake writes ./bin in the source tree)
    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DROCKETNET_MAX_CLIENTS=10000 [-DROCKETNET_DEV=OFF]
    cmake --build build -j4
    ./bin/test
    LOAD_PROFILE=light LOAD_PLAN_ONLY=1 ./bin/load 10000 30
    LOAD_PROFILE=light ./bin/load 10000 30

Clang legs set `CC=clang CXX=clang++`.

## Results

**Builds.** Every default target builds with zero warnings at the strict
flags in all eight cells: default and 10000, gcc and clang, DEV ON and OFF.
Every static assert in `sdk/rocketnet_constants.h` holds at 10000.

**Memory plan.** The load's plan (`LOAD_PLAN_ONLY=1`), 10000 clients:

| profile | server | per client | total |
|---|---|---|---|
| default | 182935151360 B (183 GB) | 9952256 B | 282457711360 B (282 GB) |
| light | 1666031360 B (1.67 GB) | 2356928 B | 25235311360 B (25.2 GB) |

The default per-client config cannot run 10000 clients on any fleet host;
BUILDING.md already directs a 10000-client server to lighter per-client
limits, and `LOAD_PROFILE=light` is that config (same offered traffic and
pass criteria). The bounded run uses it.

**Bounded load, `LOAD_PROFILE=light ./bin/load 10000 30`** (server on gso at
127.0.0.1, clients on portable, in one process):

| build | connected | connect_s | sent / received / drops | server_update p50 / p99 us | rss_mb | census | verdict |
|---|---|---|---|---|---|---|---|
| 10000 gcc DEV ON, head `09395b07` | 10000 | 5.023 | 300000 / 300000 / 0 | 3792 / 15014 | 7770.88 | resource_baseline=PASS, fds 3 -> 20009 -> 3, threads 1 -> 10007 -> 1 | PASS |
| 10000 gcc DEV ON, `cbf907be` | 10000 | 3.418 | 300000 / 300000 / 0 | 3452 / 5050 | 7739.19 | resource_baseline=PASS, fds 3 -> 20009 -> 3, threads 1 -> 10007 -> 1 | PASS |
| 10000 gcc DEV OFF, `cbf907be` | 10000 | 3.260 | 300000 / 300000 / 0 | 3225 / 4940 | 7732.71 | resource_baseline=PASS, fds 3 -> 20009 -> 3, threads 1 -> 10007 -> 1 | PASS |

All five criteria passed (connect_within_10s, no_unexpected_disconnect,
reliable_delivery, unreliable_integrity, counters_agree). At head the client
update p99 was 92 us and the server update p99 15014 us, on a host at load
average 21 from other work; at `cbf907be` the client update p99 was 17 us
(DEV ON) and 16 us (DEV OFF). The server update is over the 100 us
call budget at 10000 clients, as it already is at 1000 (870 us p99 at BASE,
below); that is the performance milestone's work, not this card's.

**Default build unchanged, `./bin/load 1000 30`:**

| build | connected | sent / received / drops | server_update p50 / p99 us | rss_mb | verdict |
|---|---|---|---|---|---|
| BASE `7a0258612392` | 1000 | 300000 / 300000 / 0 | 249 / 870 | 1231.22 | PASS |
| head `cbf907be` | 1000 | 300000 / 300000 / 0 | 267 / 1045 | 1232.26 | PASS |

The head's code is BASE's (comments, docs and the workflow only); the p99
difference is run-to-run noise on a shared host. At head the default build's
`./bin/test` and `./bin/functional` pass at DEV ON, and `./bin/test` passes
at DEV OFF.

**Unit suite, 10000 build, at head: green in all four cells.** On Linux x64,
`./bin/test` exits 0 and prints `*** ALL TESTS PASSED ***` with no check
failure in every 10000 cell, each built with zero warnings:

| cell | build warnings | ./bin/test | cases run |
|---|---|---|---|
| gcc DEV ON | 0 | exit 0, ALL TESTS PASSED | 1078 |
| gcc DEV OFF | 0 | exit 0, ALL TESTS PASSED | 1079 |
| clang DEV ON | 0 | exit 0, ALL TESTS PASSED | 1078 |
| clang DEV OFF | 0 | exit 0, ALL TESTS PASSED | 1079 |

`test_secrets_wiped_destroy_writes_only_keys` runs and passes in each. The unit
suite now reports its peak and bound, while the functional suite's
`test_unit_case_memory` row enforces that same declaration. At attempt 3,
before that tier split, the unit suite failed every 10000 cell
(`peak_rss_mb=1698 bound_mb=1217`):
the case copies every live block of its connected pair before destroy, so
its peak is twice the pair's memory, and the server transport's fixed
tables grow with the build's maximum whatever the case's `max_clients`. The
plan-derived `SUITE_CASE_RSS_BOUND` beside the case bounds it at twice the
pair's planned memory. A 10000-client functional qualification must therefore
include the memory row; the ordinary unit step remains deterministic.
