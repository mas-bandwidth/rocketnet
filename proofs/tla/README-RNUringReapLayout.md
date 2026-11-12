# RNUringReapLayout: the io_uring reap at the code's layout

`RNUringReapLayout.tla` models the reap of the io_uring server transport
(`sdk/rocketnet_server_uring_linux.cpp`) with the code's own index
arithmetic: the completion ring the reap reads oldest first, each queue's
ring of receives held for the fill bank's room, the turn the copy of held
receives takes across the queues, and the room test. `RNUringDriver` is the
abstract model of the same driver; its completion queue is a set, so it
cannot show what the order of the completion ring does. This model can.
The `io-uring-reap-layout` row in `COVERAGE.tsv` tracks the coverage.

## The layout

| Model | Code |
|---|---|
| `cqArr`, `cqHead`, `cqTail`, entry at `counter % CqCap` | the kernel's completion ring, read by `rocketnet_server_uring_ring_peek` and consumed by `rocketnet_server_uring_ring_advance`, oldest first |
| `RecvE(b)`, `SendE` | a receive CQE naming its provided buffer, a send CQE |
| `Q`, `QueueOf(b)` | `num_queues`, the queue whose buffer ring provided buffer `b` |
| `bank`, `Seg`, `Room`, `HasRoom` | the fill bank's `num_packets`, `ROCKETNET_URING_MAX_SEGMENTS`, `rocketnet_server_uring_bank_size`, `rocketnet_server_uring_bank_has_room` |
| `heldArr[q]`, `hHead[q]`, `hCount[q]`, `HeldCap` | `held[q]`, `held_head[q]`, `held_queue_count[q]`, `ROCKETNET_URING_RECV_BUFFERS` |
| `HeldTotal` | `held_count`, the held receives of every queue |
| `PushIdx(q)` | `( held_head[q] + held_queue_count[q] ) % ROCKETNET_URING_RECV_BUFFERS` in `rocketnet_server_uring_receive_or_hold` |
| `MustHold(q)` | the hold test in `rocketnet_server_uring_receive_or_hold`: the queue holds a receive already, or the bank has no room |
| `turn`, `CopyFrom` | `held_turn`, and the scan from it to the first queue that holds a receive in `rocketnet_server_uring_copy_held` |
| `ring`, `Buffers` | the provided buffers the kernel may fill; a held receive keeps its buffer |
| `Budget`, `work` | `ROCKETNET_URING_SEND_BUDGET` and the bank size, one constant here |

## Actions

| Action | Code |
|---|---|
| `KernelRecv(b)`, `KernelSend` | the kernel posts a receive or a send completion (environment actions) |
| `SendBegin` | `rocketnet_server_uring_send_packets` submits, then reaps |
| `RecvBegin` | `rocketnet_server_uring_receive_packets` begins its reap |
| `CopyHeld` | `rocketnet_server_uring_copy_held` at the start of every reap: while the bank has room, the oldest held receive of the queue `turn` names (or the next queue that holds any) into the bank, its buffer back to the ring, and the turn to the queue after it |
| `Reap` | one CQE of `rocketnet_server_uring_reap_available`, once the held receives are copied as far as the room goes: a send retired; a receive copied, or held when the bank has no room or an earlier receive of its queue is held |
| `CallEnd` | the call returns once the ring is empty or the budget spent; `receive_packets` then swaps, which empties the fill bank |

Holding on "the receive's own queue holds one, or no room" is the same, at
`Reap`, as holding on "any queue holds one, or no room": `Reap` runs only
once `CopyHeld` has stopped, so a held receive anywhere means the bank has
no room. `RNUringDriver`, whose hold is the latter, needs no change.

## Properties

- `HeldMatches`: each queue's hold ring holds exactly its held FIFO, oldest
  at `hHead[q]`, each slot read at its modulus, and never more than
  `HeldCap`.
- `InOrder`: each queue's receives reach the bank in the order the kernel
  posted them. Order across queues is not kept, and the code makes no
  promise of it: the queues are separate rings.
- `TurnsTaken`: a queue holding receives has one copied before any other
  queue has two; the ghost `waits[q]` counts the copies of other queues'
  held receives since `q` last had one, and stays at most `Q - 1`.
- `BankWithinRoom`: the bank never takes more than `Room`.
- `SendsNotBehindRoom` (action property): no call ends with a send
  completion still in the ring while it has budget left.

## Cases

Every case has `CqCap = 4`, `Q = 2`, `Buffers = 3` (two of queue 0, one of queue 1),
`HeldCap = 2`, `Budget = 3`, six receives and two sends. Group
`rnuringreaplayout` in `CASES.tsv`; the records are in `RUNS.tsv`.

| Case | Change | Expected |
|---|---|---|
| `MCRNUringReapLayout.cfg` | the code: one datagram per receive, a bank of two | pass |
| `MCRNUringReapLayoutCoalesced.cfg` | the code: two datagrams per receive, a bank of three | pass |
| `MCRNUringReapLayoutBrokenStopAtRoom.cfg` | `HoldRule = FALSE`: the reap stops at a receive without room | `SendsNotBehindRoom` |
| `MCRNUringReapLayoutBrokenNoHeldMod.cfg` | `HeldMod = FALSE`: the push at `hHead[q] + hCount[q]` without the modulus | `HeldMatches` |
| `MCRNUringReapLayoutBrokenHeldCapShort.cfg` | `HeldCap = 1`, one slot short of queue 0's two buffers | `HeldMatches` |
| `MCRNUringReapLayoutBrokenNoTurns.cfg` | `TurnRule = FALSE`: the copy always starts from queue 0 | `TurnsTaken` |

## Counterexamples, checked by hand against the code

**BrokenStopAtRoom**: receives fill the bank of two; a send call submits
and its completion lands behind a third receive; that receive is at the
head with no room; the call ends with budget left and the send completion
still in the ring. Against the reap before the hold:
`rocketnet_server_uring_reap_available` copied while
`rocketnet_server_uring_reap_has_room` held and broke at the first receive
without room, so the CQEs behind it, the send's among them, stayed in the
completion queue until a swap. Under an inbound flood that fills the bank
every update, a send's completion was always behind such a receive,
`sends_pending` never reached 0 at the end of `send_packets`, and the
reservation index never went back to 0. The unit case
`test_io_uring_transport_send_completions_pass_a_full_fill_bank` is that
trace at the code's sizes and fails on the reap before the hold.

**BrokenNoHeldMod**: a receive of queue 0 is held while the bank is full
and copied after a swap, so `hHead[0] = 1` with none held; with the bank
full again, the next receive of queue 0 is pushed at slot 1 and the one
after it at `1 + 1 = 2`, past its ring of two, and is lost while
`hCount[0]` counts it. The code's push takes the modulus.

**BrokenHeldCapShort**: with both of queue 0's buffers held, a ring of one
slot overwrites its oldest receive. The code's ring has a slot for every
provided buffer of its queue, and a held receive owns its buffer, so the
kernel cannot post a receive that would not fit.

**BrokenNoTurns**: both queues hold receives while the bank is full; after
a swap the copy takes queue 0's oldest, then, with room still, queue 0's
next, so queue 1, holding a receive throughout, sees two copies of another
queue's before one of its own. A copy that always starts from one place in
the line lets one queue go first again and again. The fix's
predecessor held every queue's receives in one ring in arrival order: a
queue whose source flooded put a whole buffer ring of receives ahead of
another queue's one, which waited behind all of them, updates long. The
unit case `test_io_uring_transport_held_receives_take_turns_by_queue` is
that trace at the code's sizes: a coalesced flood of 1000 receives on
queue 0 ahead of one receive on queue 1; queue 1's datagram reaches the
bank in update 4 on the code before this change and in update 1 after.
