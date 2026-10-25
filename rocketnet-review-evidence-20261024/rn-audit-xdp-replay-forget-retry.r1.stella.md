# Review evidence: rn-audit-xdp-replay-forget-retry, round 1

Card: `rn-audit-xdp-replay-forget-retry` (kind fix-red). Repository:
`mas-bandwidth/rocketnet`. Base: `main` at `2729a55f339e451d93a6a9ed873635859c280348`.
Audit read: `docs/audits/2026-10-24-area-transports-linux-af-xdp.md`.

Verdict: LAND. Findings 3 and 18 are repaired, the repair is pinned by unit
cases, by the XDP socket seam and by a TLA+ model that holds the new
ownership rule, and the gate is green. The reconnect-freshness gap is named
below and is not claimed closed here.

## The two findings

- **Finding 3 (risk), `sdk/rocketnet_server_socket_linux.cpp:2341` at the
  audit sha.** A forget the kernel refuses (`bpf_map_delete_batch` failing
  with anything but `ENOENT`, `EINVAL` or `ENOTSUPP`, or `bpf_map_delete_elem`
  failing) was counted and never retried: the walk had already cleared the
  slot's mark record when it issued the forget
  (`sdk/rocketnet_server_replay_marks.cpp:115`). The stale high-water stayed
  in the kernel map, so a later connection from the same address and port had
  every session packet dropped in the kernel as replayed and timed out.
- **Finding 18 (quality), `sdk/rocketnet_xdp_replay_marks.h:67` at the audit
  sha.** `rocketnet_xdp_replay_marks_reset` zeroed `refused` at every commit
  and no production code read it, while `sdk/rocketnet_server_socket.h:76`
  documented that a full buffer "counts in the buffer's refused count"; and
  `rocketnet_xdp_replay_marks_forgets` returned only a count, unlike its
  `marks` sibling.

## The repair

- `sdk/rocketnet_xdp_replay_marks.h`: `rocketnet_xdp_replay_marks_reset_keeping( marks,
  forget_failed )` moves every forget the kernel refused to the front of the
  pending buffer, in their order, clears the marks, and hands the buffer's own
  refusal count to the caller. `rocketnet_xdp_replay_marks_forgets` now
  returns the keys, like its `marks` sibling.
- `sdk/rocketnet_server_socket_linux.cpp`: `rocketnet_server_socket_replay_commit`
  walks a refused batch delete one element at a time, so each forget's outcome
  is known, and keeps every forget that failed with anything but a missing key
  pending for the next commit; the kept forgets are applied before that
  commit's marks. The buffer's refusals are added to the socket's
  `replay_commit_failures`. `rocketnet_server_socket_replay_forget` returns
  whether the forget was queued.
- `sdk/rocketnet_server_replay_marks.cpp`: the walk keeps the slot's seen and
  marked records when the sink refuses the forget, and the mark pass does not
  replace the old identity with a new mapping while the old forget is owed.
- `sdk/rocketnet_driver.h`, `sdk/rocketnet_driver_builtin.cpp`: the driver's
  `replay_forget` returns `bool`.
- `sdk/rocketnet_server_socket.h`: the header sentence now says where the
  refusal count lands: the next commit adds it to the commit failure count.

## The pins

- `tests/test_unit_xdp_replay_marks.cpp`:
  `test_xdp_transport_replay_marks_failed_forgets_kept` marks three of six
  forgets failed, resets keeping, and checks that exactly those three remain
  in their order, that the marks are zero, and that the next walk's forget
  follows them.
  `test_xdp_transport_replay_marks_refusals_reach_the_socket` fills the
  buffer past `ROCKETNET_XDP_REPLAY_MAX_ENTRIES` and checks that the refusal
  count is carried to the caller, not zeroed.
- `tests/test_unit_server_replay_marks.cpp`:
  `test_server_replay_marks_refused_forget_keeps_the_slot` checks that a
  refused forget keeps the slot's old address and that the new address is
  marked only after the old forget lands.
- `tests/test_xdp.cpp` (the XDP socket seam, run by `./bin/test_xdp`):
  `test_xdp_transport_replay_marks_buffered_and_committed` checks that the
  refused forget stays pending, that the marks clear, and that a full
  buffer's refusals reach the socket's commit failure count.
- `tla/RNXdpReplay.tla`: `ForgetOwedUntilApplied` says that an entry for an
  address with no live session is always owed a forget, so no stale kernel
  entry outlives a released slot by more than the commits the forget took to
  succeed. `tla/MCRNXdpReplayBrokenFailedForgetDropped.cfg` drops the
  refused forget instead of keeping it and fails that invariant.

## The model runs

Pinned jar `936a262061c914694dfd669a543be24573c45d5aa0ff20a8b96b23d01e050e88`,
java 21.0.12.1, two workers, group `rnxdpreplay`:

| Configuration | Result |
|---|---|
| `MCRNXdpReplay.cfg` | PASS, 484069 generated, 197106 distinct, 2.757 s |
| `MCRNXdpReplayBrokenKernelAdvances.cfg` | fails `FreshGenuineNeverDropped`, exit 12 |
| `MCRNXdpReplayBrokenNoForget.cfg` | fails `NewSessionNeverDroppedByOldMark`, exit 12 |
| `MCRNXdpReplayBrokenNarrowWindow.cfg` | fails `FreshGenuineNeverDropped`, exit 12 |
| `MCRNXdpReplayBrokenFailedForgetDropped.cfg` | fails `ForgetOwedUntilApplied`, exit 12 |
| `MCRNXdpReplayBrokenUnmarkedNotForgotten.cfg` | fails `ReleasedSlotForgetsItsReplyMac`, exit 12 |
| `MCRNXdpReplayBrokenReconnectFreshness.cfg` | fails `NewSessionNeverDroppedByOldMark`, exit 12 |

The records in `tla/RUNS.tsv` are the bench records carried with the work; they
are fingerprint-current (`tlacheck groups --root . --stale` prints `[]`), and
the run above reproduces their generated and distinct counts exactly. The local
run was not merged into the ledger: no bench machine was reachable from the
card sandbox (`ssh` to the bench host is refused), so the committed rows stay
the bench rows.

## The base failure

The two new unit cases do not build at the base: the keep-failed reset and the
key-returning accessor are new. Compiling the new test file against the base
headers gives

```
tests/test_unit_xdp_replay_marks.cpp:174:27: error: 'rocketnet_xdp_replay_marks_reset_keeping' was not declared in this scope; did you mean 'rocketnet_xdp_replay_marks_reset'?
tests/test_unit_xdp_replay_marks.cpp:100:49: error: cannot convert 'const rocketnet_server_xdp_replay_key_t**' to 'uint32_t*'
```

## The gate

Release builds from a clean `build/`, XDP on (`ROCKETNET_XDP=ON`):

- `CC=gcc CXX=g++ make -j4`: zero warnings, `./bin/test` ALL TESTS PASSED,
  `./bin/functional` ALL TESTS PASSED.
- `CC=clang CXX=clang++ make -j4`: zero warnings, `./bin/test` ALL TESTS
  PASSED, `./bin/functional` ALL TESTS PASSED.
- `CC=clang CXX=clang++ make -j4 ROCKETNET_DEV=OFF`: zero warnings, `./bin/test`
  ALL TESTS PASSED.
- `python3 tools/check_tla_coverage.py`: COVERAGE OK 148 rows.

## Not done

- **The reconnect-freshness gap.** A reconnect that arrives before the walk
  has queued the forget still meets the old kernel entry. The model shows it:
  `MarkNeverAhead` is violated at depth 9 (`Connect(A1)`, a genuine sequence 1
  opens, `Walk` and `Commit` write `high_water = 1` for session 1,
  `Release(A1)`, then `Connect(A1)` takes session 2), so `MarkNeverAhead` is
  left out of the control configuration and the trace is cited in
  `tla/MCRNXdpReplay.cfg` and `tla/README-RNXdpReplay.md`. The remedy is
  split to `rn-fix-xdp-reconnect-freshness`; this card does not close it, and
  the model does not prove it.
- **The transport does not admit a session while a delete is still owed.** That
  is the same split card.
