# RNReceiveHandoff: the server receive handoff and its delivery

`RNReceiveHandoff.tla` models the receive handoff a server transport's
worker and main thread share over three buffers,
`sdk/rocketnet_receive_handoff.h`, as the GSO server uses it in
`sdk/rocketnet_server_gso_linux.cpp`. The worker banks datagrams into its
buffer outside the mutex (`Bank`) and at the end of each pass with results
commits its count under the mutex (`Commit`,
`rocketnet_receive_handoff_commit`), moving to the free buffer once main has
started on its buffer. Main's receive call (`Call`,
`rocketnet_server_gso_receive_from_queue`) resets a finished tail and
try-acquires the mutex; on success (`rocketnet_receive_handoff_begin`), when
the worker has left main's buffer and the tail is read, main frees the tail,
makes its buffer the tail and starts on the worker's, and reads up to that
buffer's committed count. It returns the tail's unread datagrams, then its
own buffer's, within the call's budget.

A call is one step: main reads only below counts the worker no longer
changes, and the worker writes only past them, so the worker's steps during a
call commute with main's reads. A buffer's datagrams are consecutive, so a
buffer is its first datagram and its count.

## What it checks

- `DeliveredByNextCall`: a datagram the worker committed before main's call
  took the mutex is returned by that call, unless the mutex was busy, the
  budget cut the call, or the call began with datagrams left from a cut call.
  This is the property the handoff exists for: before 2026-11-13 the GSO
  worker published a filled buffer and main claimed it at its next call, so
  every datagram waited one more server update.
- `FreeIffShared`, `NoSharedSlot`, `FreeIsEmpty`: the roles are distinct, a
  free buffer exists exactly while main and the worker share one, main reads
  below the committed count and the worker writes at or past it, a buffer
  the worker left keeps its final count, and the worker moves only to an
  empty buffer.
- `InOrder`, and each `Call`'s returned run following `delivered`: every
  datagram is returned once, in order.

## Cases

The 2026-11-13 run on amd-2 with the pinned `tlacheck` and TLC jar (records
in `RUNS.tsv`); `Cap = 2`, `MaxRx = 5`, `Budgets = {1, 6}`.

| Config | Expected | Distinct states | Seconds | What it shows |
|---|---|---|---|---|
| `MCRNReceiveHandoff.cfg` | pass | 515 | 2.1 | Every invariant. |
| `MCRNReceiveHandoffBrokenReadOnlyLeft.cfg` | `DeliveredByNextCall` | 26 | 2.0 | `ReadOnlyLeft = TRUE`: main reads only a buffer the worker has left, the old ready-slot rule. |

## Counterexample checked against the code

- **ReadOnlyLeft.** The worker banks datagram 1 into the buffer it shares
  with main and commits it, moving to the free buffer; it banks datagram 2
  there and commits it. Main's call then advances to the worker's buffer but,
  under the switch, reads none of it: it returns datagram 1 from the tail and
  leaves datagram 2, committed before the call, for the next one. In the code
  `rocketnet_server_gso_receive_from_queue` sets `main_limit` to the
  committed count of the buffer main starts on; the trace-check plant
  (`ROCKETNET_RECEIVE_HANDOFF_PLANT`) sets it to 0 for the worker's buffer,
  and the walk of `tests/tla_trace_receive_handoff.cpp` fails
  `TraceConsistent` on it.

## Trace check

`TraceRNReceiveHandoff.tla` checks the seeded walk of
`tests/tla_trace_receive_handoff.cpp` (layer `receive-handoff` of
`tools/tla_trace_check.py`, seeds 1-4) against this module with the code's
buffer size, `ROCKETNET_GSO_RECV_QUEUE_SIZE`. Every model variable but the
ghost `late` is read off the queue as the library holds it
(`rocketnet_server_gso_test_receive_state`).
