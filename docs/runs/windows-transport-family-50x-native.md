# Windows native server cells, fifty times in a row

The record of the run of `scripts/transport-cells-repeat --runs 50` for the
iocp/portable and rio/portable cells on captain (Windows 11, MSVC), the cells
the nightly job `windows-transport-cells-repeat`
(`.github/workflows/nightly.yml`) runs every night beside portable/portable and
loopback/loopback. The cases are read from ci.yml's `TRANSPORT_MATRIX_WINDOWS`
step: IPv4 (`test_loopback_transport_udp`), IPv6
(`test_loopback_transport_ipv6`), an IPv4 client of a dual-stack server
(`test_transport_dual_stack_ipv4_client`) and the environment pin
(`test_transport_env_pin_is_honoured`).

- Date (`date -u` in Git Bash): Sat, Oct 24, 2026 2:47:15 PM (UTC)
- Head: a727e146 (main at cd424514 plus the nightly job)
- Machine: `hostname` captainamerica; `uname -r` (Git Bash) 3.6.10-710e5275.x86_64; Windows 10.0.26200.9711
- NIC: none; every cell runs over loopback, so no `ethtool` line applies

## Commands

```
git log --oneline -1
a727e146 Merge remote-tracking branch 'origin/sprint/rn-win-transport-family-50x-native.w1.g3.e1' into sprint/rn-win-transport-family-50x-native.w3.g1.e1
cmake -B build -A x64 && cmake --build build --config Release --parallel 4
BUILD exit=1
```

## Result

The Release build failed, so neither the cells nor `./bin/Release/test.exe`
ran:

```
tests\test_unit_public_link_simulator.cpp(313,5): warning C4324: 'test_public_link_simulator_revision_1_prefixes::client_revision_1_t': structure was padded due to alignment specifier
tests\test_unit_public_link_simulator.cpp(313,5): error C2220: the following warning is treated as an error
```

- iocp/portable, every case: not made
- rio/portable, every case: not made
- `./bin/Release/test.exe`: not made

## Second attempt (build only)

Main's C4324 break was fixed by #565 (17d3bf34). The branch at 6bb1e287
(main at 6cd2744e plus the nightly job) was built on captain on
2026-10-24 at about 15:37 UTC under a shared `winlock --slots 3` slot, in
Git Bash:

```
cmake -B build -A x64 && cmake --build build --config Release --parallel 4
BUILD exit=0, 0 MSVC warnings
./bin/Release/test.exe
*** ALL TESTS PASSED *** (983 test_ lines)
```

- iocp/portable, every case: not made
- rio/portable, every case: not made

The fifty-run cells were not made: the worker ended before taking the
exclusive lock, and the rebuild at the next head (bd9694d9, main at 5805e488)
waited 40 minutes in captain's winlock queue behind other exclusive runs and
was withdrawn at the card's deadline.

## Verdict

Not done: the build and the unit suite are green on captain, and the
iocp/portable and rio/portable cells at N=50 on IPv4, IPv6 and dual stack are
still owed under `winlock --exclusive`.
