# Linux transport cells, fifty times in a row, and the functional suite under ASan

The record of one run of `scripts/transport-cells-repeat --runs 50` and one
run of `scripts/transport-cells-repeat --asan` on a Linux bench, for the
final-deliverable row "Linux: every transport cell fifty times in a row with
zero flakes, and the functional suite under ASan per transport". The cells
and the cases are read from ci.yml's `TRANSPORT_MATRIX_LINUX` step: in each
socket cell, IPv4 (`test_loopback_transport_udp`), IPv6
(`test_loopback_transport_ipv6`), an IPv4 client of a dual-stack server
(`test_transport_dual_stack_ipv4_client`) and the environment pin
(`test_transport_env_pin_is_honoured`).

- Date (`date -u`): Sat Oct 24 05:45:58 PM UTC 2026
- Head: ac4206a69acd83840c27d2ea024f6cc3712d734f (at `git log --oneline -1`, `ac4206a6 carry attempt 5's work (99c1a25a12f0) onto the tip of main (1dbdfcacd634)`); every run below is at this head
- Machine: `hostname` hetzner1; `uname -r` 6.8.0-139-generic; 16 processors; `/proc/sys/kernel/io_uring_disabled` 0 (the io_uring cell ran, it was not skipped)
- NIC: none; every socket cell runs over the loopback interface, so no `ethtool` line applies
- Load: the bench is shared and other cards' builds and runners were active.
  The 50-run cells ran at load 4 to 5 on 16 processors. The ASan suites ran
  while the load climbed to 84 (load average 84.70 at the end), driven by
  other cards' builds on the same machine. The ASan results below are taken
  at that load.

## Commands

```
git log --oneline -1
ac4206a6 carry attempt 5's work (99c1a25a12f0) onto the tip of main (1dbdfcacd634)
make -j16
[100%] Built target test
scripts/transport-cells-repeat --runs 50 --cell portable/portable --cell gso/portable --cell gso/gso --cell portable/gso --cell io_uring/portable --cell loopback/loopback
scripts/transport-cells-repeat --asan --runs 1 --jobs 12 --cell portable/portable --cell gso/portable --cell gso/gso --cell portable/gso --cell io_uring/portable --cell loopback/loopback
```

The six socket cells are exactly the socket cells `--list` names; the xdp
cell is not selected (see below).

## The xdp cell

`xdp/portable` (`test_xdp_transport_max_size_both_ways_ipv4`,
`test_xdp_transport_max_size_both_ways_ipv6`): not made. This card was cut
to the Linux socket cells; the xdp cell belongs on the dedicated XDP machine
in a virtme-ng guest and is owed to its own card.

## Result: fifty runs per cell

```
CELL portable/portable test_loopback_transport_udp runs=50 pass=50 fail=0
CELL portable/portable test_loopback_transport_ipv6 runs=50 pass=50 fail=0
CELL portable/portable test_transport_dual_stack_ipv4_client runs=50 pass=50 fail=0
CELL portable/portable test_transport_env_pin_is_honoured runs=50 pass=50 fail=0
CELL gso/portable test_loopback_transport_udp runs=50 pass=50 fail=0
CELL gso/portable test_loopback_transport_ipv6 runs=50 pass=50 fail=0
CELL gso/portable test_transport_dual_stack_ipv4_client runs=50 pass=50 fail=0
CELL gso/portable test_transport_env_pin_is_honoured runs=50 pass=50 fail=0
CELL gso/gso test_loopback_transport_udp runs=50 pass=50 fail=0
CELL gso/gso test_loopback_transport_ipv6 runs=50 pass=50 fail=0
CELL gso/gso test_transport_dual_stack_ipv4_client runs=50 pass=50 fail=0
CELL gso/gso test_transport_env_pin_is_honoured runs=50 pass=50 fail=0
CELL portable/gso test_loopback_transport_udp runs=50 pass=50 fail=0
CELL portable/gso test_loopback_transport_ipv6 runs=50 pass=50 fail=0
CELL portable/gso test_transport_dual_stack_ipv4_client runs=50 pass=50 fail=0
CELL portable/gso test_transport_env_pin_is_honoured runs=50 pass=50 fail=0
CELL io_uring/portable test_loopback_transport_udp runs=50 pass=50 fail=0
CELL io_uring/portable test_loopback_transport_ipv6 runs=50 pass=50 fail=0
CELL io_uring/portable test_transport_dual_stack_ipv4_client runs=50 pass=50 fail=0
CELL io_uring/portable test_transport_env_pin_is_honoured runs=50 pass=50 fail=0
CELL loopback/loopback test_loopback_transport_udp runs=50 pass=50 fail=0
CELL loopback/loopback test_loopback_transport_ipv6 runs=50 pass=50 fail=0
CELL loopback/loopback test_transport_dual_stack_ipv4_client runs=50 pass=50 fail=0
CELL loopback/loopback test_transport_env_pin_is_honoured runs=50 pass=50 fail=0
TRANSPORT-CELLS-REPEAT os=linux runs=50 asan=0 logs=.../run.enxJ7v result=pass
```

Every one of the 24 CELL lines is pass=50 fail=0 (1200 runs, no failure).

## Result: ASan (Release, `ROCKETNET_SANITIZE_ADDRESS=ON`)

Every socket cell's cases once on the ASan build: all 24 CELL lines
runs=1 pass=1 fail=0. The whole functional suite once per cell:

```
SUITE portable/portable runs=1 pass=1 fail=0
SUITE gso/portable runs=1 pass=1 fail=0
SUITE gso/gso runs=1 pass=0 fail=1 exit=1
SUITE portable/gso runs=1 pass=1 fail=0
SUITE io_uring/portable runs=1 pass=1 fail=0
SUITE loopback/loopback runs=1 pass=0 fail=1 exit=1
TRANSPORT-CELLS-REPEAT os=linux runs=1 asan=1 logs=.../run.KecMel result=fail
```

The two failures, each recorded once and not retried:

- `gso/gso`, first failing case `test_posix_connect_disconnect_1000`
  (posix lifecycle stress), first log `gso.gso.suite.log`:

  ```
  test_posix_connect_disconnect_1000 [server portable, client portable]: connect stalled at cycle 621
  posix lifecycle stress check failed: .../functional/posix_lifecycle_stress.cpp:1106: rocketnet_client_transport_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED
  ```

  The same log ends with a LeakSanitizer report (`SUMMARY: AddressSanitizer: 2609752 byte(s) leaked in 68 allocation(s)`, all from `plan_rss_malloc`), emitted because the failed check aborted the suite before teardown.

- `loopback/loopback`, first failing case
  `test_loopback_transport_live_message_matrix`, first log
  `loopback.loopback.suite.log`:

  ```
  test_loopback_transport_live_message_matrix
  check failed: test_loopback_transport_live_message_matrix exceeded 120s
  ```

A failed check ends the suite, so the cases after each failure did not run
under that cell's transports under ASan. Both failures are at this head, and
both were taken while other cards' builds held the load at 44 to 85 on this
16-processor bench; each is a defect for its own card (a functional case that
is not clean under load), never a reason to loosen its bound.

## Verdict

Hold. Every Linux socket cell fifty times in a row: pass, zero flakes
(1200 runs). The functional suite under ASan: green under portable/portable,
gso/portable, portable/gso and io_uring/portable; red under gso/gso
(`test_posix_connect_disconnect_1000` connect stalled at cycle 621, plus a
LeakSanitizer report) and loopback/loopback
(`test_loopback_transport_live_message_matrix` exceeded 120s). The xdp cell
is not made.
