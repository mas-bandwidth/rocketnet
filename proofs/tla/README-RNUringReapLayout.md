# RNUringReapLayout: the io_uring reap at the code's layout

`RNUringReapLayout.tla` models the reap of the io_uring server transport
(`sdk/rocketnet_server_uring_linux.cpp`) with the code's own index
arithmetic: the completion ring the reap reads oldest first, the ring of
receives held for the fill bank's room, and the room test. `RNUringDriver`
is the abstract model of the same driver; its completion queue is a set, so
it cannot show what the order of the completion ring does. This model can.
The `io-uring-reap-layout` row in `COVERAGE.tsv` tracks the coverage.

## The layout

| Model | Code |
|---|---|
| `cqArr`, `cqHead`, `cqTail`, entry at `counter % CqCap` | the kernel's completion ring, read by `rocketnet_server_uring_ring_peek` and consumed by `rocketnet_server_uring_ring_advance`, oldest first |
| `RecvE(b)`, `SendE` | a receive CQE naming its provided buffer, a send CQE |
| `bank`, `Seg`, `Room`, `HasRoom` | the fill bank's `num_packets`, `ROCKETNET_URING_MAX_SEGMENTS`, `rocketnet_server_uring_bank_size`, `rocketnet_server_uring_bank_has_room` |
| `heldArr`, `hHead`, `hCount`, `HeldCap` | `held`, `held_head`, `held_count`, `ROCKETNET_URING_HELD_RECEIVES` |
| `PushIdx` | `( held_head + held_count ) % ROCKETNET_URING_HELD_RECEIVES` in `rocketnet_server_uring_receive_or_hold` |
| `ring`, `Buffers` | the provided buffers the kernel may fill; a held receive keeps its buffer |
| `Budget`, `work` | `ROCKETNET_URING_SEND_BUDGET` and the bank size, one constant here |

## Actions

| Action | Code |
|---|---|
| `KernelRecv(b)`, `KernelSend` | the kernel posts a receive or a send completion (environment actions) |
| `SendBegin` | `rocketnet_server_uring_send_packets` submits, then reaps |
| `RecvBegin` | `rocketnet_server_uring_receive_packets` begins its reap |
| `CopyHeld` | `rocketnet_server_uring_copy_held` at the start of every reap: the oldest held receive into the bank while it has room, its buffer back to the ring |
| `Reap` | one CQE of `rocketnet_server_uring_reap_available`, once the held receives are copied as far as the room goes: a send retired; a receive copied, or held when the bank has no room or an earlier receive is held |
| `CallEnd` | the call returns once the ring is empty or the budget spent; `receive_packets` then swaps, which empties the fill bank |

## Properties

- `HeldMatches`: the hold ring holds exactly the held FIFO, oldest at
  `hHead`, each slot read at its modulus, and never more than `HeldCap`.
- `InOrder`: receives reach the bank in the order the kernel posted them.
- `BankWithinRoom`: the bank never takes more than `Room`.
- `SendsNotBehindRoom` (action property): no call ends with a send
  completion still in the ring while it has budget left.

## Cases

Every case has `CqCap = 4`, `Buffers = 3`, `Budget = 3`, six receives and
two sends. Group `rnuringreaplayout` in `CASES.tsv`; the records are in
`RUNS.tsv`.

| Case | Change | Expected |
|---|---|---|
| `MCRNUringReapLayout.cfg` | the code: one datagram per receive, a bank of two | pass |
| `MCRNUringReapLayoutCoalesced.cfg` | the code: two datagrams per receive, a bank of three | pass |
| `MCRNUringReapLayoutBrokenStopAtRoom.cfg` | `HoldRule = FALSE`: the reap stops at a receive without room | `SendsNotBehindRoom` |
| `MCRNUringReapLayoutBrokenNoHeldMod.cfg` | `HeldMod = FALSE`: the push at `hHead + hCount` without the modulus | `HeldMatches` |
| `MCRNUringReapLayoutBrokenHeldCapShort.cfg` | `HeldCap = 2`, one slot short of the three buffers | `HeldMatches` |

## Counterexamples, checked by hand against the code

**BrokenStopAtRoom**: three receives are posted; a send call submits and
its completion lands behind them; the call reaps two receives into the
bank of two; the third receive is at the head with no room; the call ends
with one unit of budget left and the send completion still in the ring.
Against the reap before the hold: `rocketnet_server_uring_reap_available`
copied while `rocketnet_server_uring_reap_has_room` held and broke at the
first receive without room, so the CQEs behind it, the send's among them,
stayed in the completion queue until a swap. Under an inbound flood that
fills the bank every update, a send's completion was always behind such a
receive, `sends_pending` never reached 0 at the end of `send_packets`, and
the reservation index never went back to 0. The unit case
`test_io_uring_transport_send_completions_pass_a_full_fill_bank` is that
trace at the code's sizes and fails on the reap before the hold.

**BrokenNoHeldMod**: receives are held while the bank is full, and after a
swap two are copied, so `hHead = 2` with one receive still held; the next
receive without room is pushed at `2 + 1 = 3`, past the ring of three, and
is lost while `hCount` counts it. The code's push takes the modulus.

**BrokenHeldCapShort**: with every one of the three buffers held, a ring of
two slots overwrites its oldest receive. The code's ring has a slot for
every provided buffer of the most queues, and a held receive owns its
buffer, so the kernel cannot post a receive that would not fit.
