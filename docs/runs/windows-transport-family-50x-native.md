# Windows native server cells, fifty times in a row

The record of the run of `scripts/transport-cells-repeat --runs 50` for the
iocp/portable and rio/portable cells on the native Windows runner (Windows 11,
MSVC), the cells the nightly job `windows-transport-cells-repeat`
(`.github/workflows/nightly.yml`) runs every night beside portable/portable and
loopback/loopback. The cases are read from ci.yml's `TRANSPORT_MATRIX_WINDOWS`
step: IPv4 (`test_loopback_transport_udp`), IPv6
(`test_loopback_transport_ipv6`), an IPv4 client of a dual-stack server
(`test_transport_dual_stack_ipv4_client`) and the environment pin
(`test_transport_env_pin_is_honoured`).

- Date (`date -u` in Git Bash, at the start of the cells run): Sat Oct 24 20:04:28 UTC 2026
- Head: 689f0947 (main at af807522 plus the nightly job)
- Machine: the native Windows runner; `uname -r` (Git Bash) 3.6.10-710e5275.x86_64; Windows 10.0.26200
- NIC: none; every cell runs over loopback, so no `ethtool` line applies
- Lock: the build and the unit suite under a shared `winlock --slots 3` slot;
  the cells under `winlock --exclusive`, nothing else running

## Commands

```
git clone --branch <this branch> https://github.com/mas-bandwidth/rocketnet.git && git checkout 689f0947
cmake -B build -A x64 && cmake --build build --config Release --parallel 4
BUILD exit=0, 0 MSVC warnings
git log --oneline -1
689f0947 Merge remote-tracking branch 'origin/main' into <this branch>
scripts/transport-cells-repeat --runs 50 --cell iocp/portable --cell rio/portable
./bin/Release/test.exe
```

## Result

```
CELL iocp/portable test_loopback_transport_udp runs=50 pass=50 fail=0
CELL iocp/portable test_loopback_transport_ipv6 runs=50 pass=50 fail=0
CELL iocp/portable test_transport_dual_stack_ipv4_client runs=50 pass=50 fail=0
CELL iocp/portable test_transport_env_pin_is_honoured runs=50 pass=50 fail=0
CELL rio/portable test_loopback_transport_udp runs=50 pass=50 fail=0
CELL rio/portable test_loopback_transport_ipv6 runs=50 pass=50 fail=0
CELL rio/portable test_transport_dual_stack_ipv4_client runs=50 pass=50 fail=0
CELL rio/portable test_transport_env_pin_is_honoured runs=50 pass=50 fail=0
TRANSPORT-CELLS-REPEAT os=windows runs=50 asan=0 result=pass
rc=0 elapsed=88s
```

The unit suite at the same head, under a shared slot (Sat Oct 24 20:55 UTC
2026, finished):

```
./bin/Release/test.exe
test-rc=1 (600 test_ lines)
test_server_disconnect_all_1000_real_time
check failed: ( running != NULL ), function mass_disconnect_server_create, tests/test_unit_server_mass_disconnect.cpp, line 140
```

This failure is inherited, not caused by this change: the head's source tree
differs from main (af807522) only in `.github/workflows/nightly.yml` and this
record, and neither is read by the unit suite. It is recorded here and left
to the change that owns that test.

## Earlier attempts

An earlier head failed to build on main's C4324 break (fixed by #565). A later
head built green and passed the unit suite, but its cells were never run: the
first exclusive try died before the first case because the clone used object
alternates that Git Bash's git could not read, and the second was withdrawn in
the queue. This run used a plain clone.

## Verdict

Pass: iocp/portable and rio/portable, every case (IPv4, IPv6 and dual stack,
plus the environment pin), fifty runs each with no failure, at 689f0947.
The unit suite at that head stops at the inherited mass-disconnect failure
above.
