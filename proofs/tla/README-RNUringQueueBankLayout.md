# RNUringQueueBankLayout: the per-queue send claim at the code's layout

`RNUringQueueBankLayout.tla` models layer L0 of the io_uring per-queue send. It covers
the arithmetic that `start_packet`, `finish_packet` and `abort_packet` share:
- the destination's queue;
- the packet id that carries the queue;
- one fetch_add claim on the queue's producer bank, refused at `S`;
- the slot computed from a pointer, refused unless the offset is non-negative, a
  whole number of strides, and inside the bank. All three tests come before the
  division.

The banks of every queue are laid out end to end. That is the worst case for a stray
pointer: one byte before a bank lands inside its neighbour.

Status: built. sdk/rocketnet_uring_send_queue.cpp implements it and cites each action:
start is Start(t, d, ptr, pid) with the pointer and id it gives back, or StartFull(t, d,
pid) when it returns NULL; finish and abort are Finish(t, k, w) when they return true,
FinishRefused(t, k) when false; turn is Flush(n) with the queues it returns. The shared
arithmetic is sdk/rocketnet_send_queue.h, the same arithmetic the XDP transport's own send staging carries. The code is
bound to the model by the trace check (TraceRNUringQueueBankLayout.tla,
tests/tla_trace_bank.cpp, `tools/tla_trace_check.py --layer bank`, README-TraceCheck.md):
its plant, the turn without its reset, is rejected at the turn onto the reused bank.

`QueueOf(d)` is `d % Q`: destinations are numbered by the queue the code's hash gives
them, so any assignment of destinations to queues is a choice of `Dests`. The finish
pointer's offsets are `-MaxSkew..MaxSkew`. Both are plain constants, as the trace
check's cfg must be.

## The small instance (MCRNUringQueueBankLayout.tla)

- 2 queues, each with 2 banks of 3 slots; stride 2.
- 2 fill threads, each making 3 starts per fill, so claims run past a bank and are
  refused.
- 3 destinations, 2 and 4 on queue 0 and 3 on queue 1 (`Dests = {2, 3, 4}`).
- A caller may hand finish a pointer 1 byte early, exact, or 1 byte late (`MaxSkew = 1`).
- 2 flushes.

## Results (bench-nyc-5, pinned jar 936a2620..., pinned tlacheck 0c97d595, 2 workers, 2026-11-13; RUNS.tsv)

| cfg | expected | result | states (distinct) |
|---|---|---|---|
| MCRNUringQueueBankLayout.cfg | pass | pass | 4,578,934 (1,008,425), 28 s |
| MCRNUringQueueBankLayoutBrokenClaimPastEnd.cfg | InBounds | InBounds violated | 1,052 (383) |
| MCRNUringQueueBankLayoutBrokenNarrowCounter.cfg | NoSharedSlot | NoSharedSlot violated | 2,363 (847) |
| MCRNUringQueueBankLayoutBrokenDivideFirst.cfg | NoForeignWrite | NoForeignWrite violated | 41 (25) |
| MCRNUringQueueBankLayoutBrokenIdAdd.cfg | RouteMatches | RouteMatches violated | 42 (25) |

The control's counts are those of the instance before the trace check's v2 changes
(returned values as action arguments, `QueueOf` as `d % Q`, `MaxSkew`): the same
states. Two witnesses moved by one state, the order TLC meets the violation in.

The instance went from 4 starts per thread to 3 so the control fits tlacheck's
110 s budget; every witness still fires (NarrowCounter needs 5 claims on one bank in a
fill, and two threads make 6).

## The witnesses, as TLC printed them

Each was checked by hand against the broken rule.

- **ClaimPastEnd.** The claim refuses only an index past `S`, so the fourth claim on a
  bank gets slot `S`, one past the array.
- **NarrowCounter.** The counter wraps at 4 with `S` = 3.
  - Three starts take slots 0, 1 and 2.
  - The fourth is refused, and its add carries the counter round to 0.
  - The fifth start is handed slot 0 while thread 1 still holds it.
  - The code's counter is 64 bits wide and a fill cannot wrap it. The witness is
    there to show that the counter's width is part of the claim.
- **DivideFirst.** Thread 2 holds slot 1 (pointer 2) and finishes with the pointer one
  byte early. The offset is 1, which truncates to slot 0: thread 1's slot. The
  check-first rule refuses offset 1 because it is not a whole stride.
- **IdAdd.** The id is `gid + q`, not `gid * Q + q`. Thread 2's start on queue 0 gets
  id 1, and finish takes `1 mod 2 = 1`, the wrong queue.
