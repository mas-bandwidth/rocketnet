# RNScheduler: the order items are first written in

`tla/RNScheduler.tla` is layer 6's scheduler (docs/CHANNELS.md 9.2): the order
in which the packet builder takes queued items into packets. The code is
`rn_packet_builder_write` in `sdk/rocketnet_packet_builder.cpp`; each of its
scheduler functions names the action it implements. The model was written
before the code.

`tla/MCRNScheduler.tla` and `tla/MCRNScheduler.cfg` are the small instance.
The two `tla/MCRNSchedulerBroken*.cfg` files are the reversed witnesses. The
cases are rows of `tla/CASES.tsv` (group `rnscheduler`) with measured records
in `tla/RUNS.tsv`.

## The rule

Three orders nest, outermost first.

1. **The budget's class order** (owner rule R6): retransmits and ordered
   progress, then new reliable items, then unreliable items. A lower class is
   taken only when no higher class has an item it may write now, whatever
   the priorities. A LOW retransmit goes before an IMMEDIATE new reliable
   item, and an IMMEDIATE unreliable item goes after both.
2. **Four priorities inside a class.** Each level has a clock. The
   level with the smallest clock goes next and its clock advances by its
   stride: 1, 5, 14 and 35 for IMMEDIATE, HIGH, MEDIUM and LOW
   (`(1 << p) * (p + 1) + p`). A clock behind
   the last level taken counts as that level's clock (the pull-forward).
   A tie goes to the more urgent level. When nothing is queued the clocks
   reset to their seeds 0, 3, 10 and 27. Under a backlog adjacent levels
   interleave at about 5:1, 2.8:1 and 2.5:1, and no level starves.
3. **Round robin across channels inside a level**, one item per turn,
   starting after the channel the last item came from.

**Channel FIFO.** Within a class only a channel's head (its earliest queued
item) is offered, at the best priority among the channel's queued items: a
priority boosts the whole channel and never reorders it.

**The window stall.** A new reliable item takes a message number when it is
first written, and at most W numbers are outstanding. A full window holds the
new-reliable class back. It never holds back retransmits (already numbered)
or unreliable items (never numbered, I13). In the code the window is layer
3's and the caller offers only items the window admits; the builder has no
window gate, so it cannot block unreliable items on one.

## What the module covers

| Variable | Meaning |
|---|---|
| `sent` | how many of the scenario's reliable items the application has queued |
| `st` | per reliable item: `none`, `queued`, `inflight`, `lost`, `acked` |
| `clock` | per level, the clock measured from the last level taken (bounded by the largest stride) |
| `rr` | the channel the last item was taken from |
| `unrelQ` | unreliable items queued for this update |
| `budget` | items this update may still write |
| `losses` | transport packet losses so far |
| `drops` | unreliable items dropped at the end of an update (counted, capped) |
| `written` | ghost: per channel, the orderings first-written, in order |
| `classBad` | ghost: a write took a class while a higher class had an item |

Actions: `Queue` (the application queues the next item of the scenario
`Msgs`), `Write` (the builder takes `Pick`), `Tick` (the update ends when
nothing more can be written: leftover unreliable items are dropped and
counted, the budget refills, the next update's unreliable items arrive, and
with nothing queued the clocks reset), `Ack(i)` and `Lose(i)` (the
transport's packet ack or loss for the packet that carried item `i`, owner
rule R1; a loss queues the item again as a retransmit).

Properties:

| Property | Kind | Meaning |
|---|---|---|
| `TypeOK` | invariant | every variable in its range |
| `FirstWriteInOrderPerChannel` | invariant | within each channel, first writes follow the ordering |
| `UnreliableNotBlockedByWindow` | invariant | a closed window never stops an unreliable item that has budget |
| `ClassOrderBeforePriority` | invariant | no write took a class while a higher class had an item it could write |
| `NoStarvation` | liveness | every queued reliable item is eventually first-written |

Fairness: weak fairness on `Queue`, `Write`, `Tick` and every `Ack(i)`.
Losses are bounded by `MaxLoss` and not fair.

## The instance

Two channels. Channel 0 queues a LOW item and then an IMMEDIATE item (the
boost case); channel 1 queues one HIGH item. Unreliable items are IMMEDIATE,
so the class order is checked against the most urgent priority. A window of
2 numbers, a budget of 1 item per update, one unreliable item arriving with
every update, and 1 transport loss.

## Reversed witnesses

| Case | Constant flipped | Expected failure | Counterexample, checked by hand |
|---|---|---|---|
| `MCRNSchedulerBrokenPerMessagePriority.cfg` | `ChannelFIFO = FALSE`: every queued item offered at its own level | invariant `FirstWriteInOrderPerChannel` | 4 states: channel 0 queues LOW (ordering 0) and IMMEDIATE (ordering 1); the first write takes ordering 1. The code offers only channel 0's head, at the boosted level, so it writes ordering 0 first (`test_rnscheduler_witness_replays`, `test_channel_fifo_kept_per_channel`). |
| `MCRNSchedulerBrokenUnreliableStarvesOrdered.cfg` | `UnreliableLast = FALSE`: the unreliable class offered first | temporal `NoStarvation` | a lasso: all three reliable items queued, then every update writes its one arriving unreliable item and ticks, forever. The code takes classes in the budget's order, so the three reliable items go in the first three updates and the unreliable class gets the budget only after (`test_rnscheduler_witness_replays`, `test_budget_class_order_still_rules`). `ClassOrderBeforePriority` is the safety face of the same flaw and is left out of this case, so the failure checked is the liveness one. |

## Action-to-function map

| Action / operator | Function in `sdk/rocketnet_packet_builder.cpp` |
|---|---|
| `Init`, `Seeds` | `rn_packet_builder_init` -> `rn_builder_reset_clocks`; `round_robin = num_channels - 1`, so the first turn is channel 0's |
| `IsHead`, `Boost`, `PickLevel`, `PickIn` | `rn_builder_next_in_class` (with `rn_builder_effective_clock`, the pull-forward) |
| `Write` (class order, clock step, `rr'`) | `rn_packet_builder_write`'s class loop, `rn_builder_take_level` |
| `Tick`'s clock reset | the end of `rn_packet_builder_write`: every item taken -> `rn_builder_reset_clocks` |

A packet the budget refuses is not written, so the builder restores the
clocks and the round-robin cursor to their values at the packet's start.

## Design choices

1. **The class order is outside priority.** Priority orders items only
   inside one budget class: retransmits and ordered progress, then new
   reliable, then unreliable, whatever their priorities (R6). An IMMEDIATE
   unreliable item waits behind a LOW reliable one.
2. **Priority boosts the whole channel; channels stay FIFO.** A channel is
   offered only at its head, at the best priority among its queued items, so
   a later high-priority send on a channel is never pulled before an earlier
   low-priority one (docs/CHANNELS.md 17.3 item 3).
3. **Round robin across channels within a level**, never one global FIFO of
   a level's messages (17.3 item 4).
4. **The clock advances at take, not at push.** A level's clock advances by
   its stride when an item of that level is taken. The weight belongs to the
   level, not to the message, which is what lets a boosted channel and round
   robin share one clock.
5. **The pull-forward is at pick.** A clock behind the last level taken
   counts as that clock when the next level is chosen.
6. **The reset is at drain.** The clocks reset when a write call leaves no
   item untaken (the model: when nothing reliable is queued or awaiting
   resend at the end of an update).
7. **Ties go to the more urgent level.**
8. **IMMEDIATE is not immediate.** Every priority is written at the next
   update, inside the budget; no priority wakes a thread.
9. **Out of range is never taken.** An item whose priority is not 0 to 3 is
   never written by the builder; the C API is to refuse it at send (17.3
   item 10).

## Abstractions in the model

- The budget is counted in items per update, not bytes; byte accounting is
  `RNSendBudget`'s.
- Retransmits and ordered progress are one class, `rtx`; the code keeps them
  as two adjacent classes in the same order.
- Unreliable items are unchanneled and on one level (`ULevel`); in the code
  they take turns and levels like any other class.
- The window is a count of outstanding numbers; ring placement is
  `RNRing`'s.
