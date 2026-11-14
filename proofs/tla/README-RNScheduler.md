# RNScheduler: the order items are first written in

`proofs/tla/RNScheduler.tla` is layer 6's scheduler (docs/CHANNELS.md 9.2): the order
in which queued items go into packets. Two pieces of code make it:
`rn_ordered_write` in `sdk/rocketnet_channel.cpp` numbers new messages, the
ready channels taking turns, and plans each packet's items (action `Number`);
`rn_packet_builder_write` in `sdk/rocketnet_packet_builder.cpp` orders the
planned items (action `Write`). Each scheduler function names the action it
implements.

`proofs/tla/MCRNScheduler.tla` holds three instances and the `proofs/tla/MCRNScheduler*.cfg`
files are the cases, rows of `proofs/tla/CASES.tsv` (group `rnscheduler`) with
measured records in `proofs/tla/RUNS.tsv`.

## The rule

**The turns across packets** (`rn_ordered_write`). A channel joins the back
of the ready list when its FIFO goes from empty to non-empty
(`rn_channels_send`: `was_empty` -> `rn_ready_push`). Each numbering turn takes
the ready list's head channel, numbers its FIFO head
(`rn_window_send_first_write`), pops it (`rn_channel_queue_pop`), and the
channel leaves the turns and rejoins the back if it has more
(`rn_ready_remove_named`, `rn_ready_push`). Numbering stops when the window is
closed or the call's room is planned; the due retransmits are planned first
(`rn_ordered_walk`), so they take room before any new number.

**The builder's order over the planned items**, three rules nested,
outermost first:

1. **The budget's class order** (owner rule R6): retransmits and ordered
   progress, then new reliable items, then unreliable items. A lower class is
   taken only when no higher class has an item it may write now, whatever
   the priorities.
2. **Four priorities inside a class.** Each level has a clock. The
   level with the smallest clock goes next and its clock advances by its
   stride: 1, 5, 14 and 35 for IMMEDIATE, HIGH, MEDIUM and LOW
   (`(1 << p) * (p + 1) + p`). A clock behind
   the last level taken counts as that level's clock (the pull-forward).
   A tie goes to the more urgent level. The clocks reset to their seeds 0,
   3, 10 and 27 at the end of each builder call whose plan was all taken (no
   packet refused). The plan holds only the lost and the numbered items, so
   the reset comes while messages still wait queued behind a stalled window
   or the room, and, the endpoint calling `rn_ordered_write` once per packet,
   in the middle of an update. In the model a reliable `Write` that leaves no
   item numbered or lost resets the clocks.
3. **Round robin across channels inside a level**, one item per turn,
   starting after the channel the last item came from.

**Channel FIFO.** Within each reliable class, only a channel's head is offered,
at the best priority among the channel's items in that class. For new reliable
items the head is the channel's earliest numbered item. For retransmits it is
the lost item earliest in the resend FIFO (`resendQ`): the builder's caller
order is `rn_ordered_walk`'s walk of that FIFO, and every send, first or
resent, moves the number to the FIFO's tail (`rn_resend_fifo_push_tail` from
`rn_window_ack_commit`, `sdk/rocketnet_window_ack.cpp`), so the head is the
least recently sent lost item, not the one with the lowest id. A priority
boosts the whole channel group and never reorders it.

**The window stall.** A new item takes a message number when it is numbered,
and at most W numbers are outstanding. A full window stops numbering. It never
holds back retransmits (already numbered) or unreliable items (never numbered,
I13). In the endpoint a stalled window with nothing due ends the reliable
phase (`rn_endpoint_ordered_due` is false: the ready head is stalled and no
resend is due) and `rn_endpoint_end_reliable` starts the unreliable phase,
unless the budget refused a reliable packet (`reliable_waiting`): the
unreliable phase runs while budget remains (`sdk/rocketnet_endpoint.cpp`,
`rn_endpoint_next_packet_bytes`).

## Priorities in v1.1.x: the levels are reserved

Every reliable message is MEDIUM: `rn_channels_send` and the endpoint's two
routes set `descriptor->priority = ROCKETNET_PRIORITY_MEDIUM`, and the public
API (`sdk/rocketnet.h`) takes no priority. Only CONFIG
(`rn_ordered_announce_config`) is IMMEDIATE, and it is pushed to the head of
channel 0 and of the turns. So on the code's paths the levels decide one
thing: CONFIG before MEDIUM, and CONFIG is already its channel's head. HIGH,
LOW, and IMMEDIATE outside CONFIG are reserved: the builder supports them, no
caller sets them.

The checked instance is the code's (`MCMsgs`, `MCRNScheduler.cfg` and the
witnesses on it). The builder's contract at the reserved levels keeps one
passing control and one witness on `MCMsgsReserved`
(`MCRNSchedulerReservedLevels.cfg`, `MCRNSchedulerBrokenPerMessagePriority.cfg`);
the boost there has no caller in v1.1.x. No property depends on the stride
values: on the code's instance only IMMEDIATE before MEDIUM is observable. The
strides are held by the builder's unit tests instead
(`tests/test_unit_send_priorities.cpp`, a LOW item one LOW stride behind a
backlog; `tests/test_unit_packet_builder.cpp`, the reference scheduler with
strides 1, 5, 14, 35, take for take).

## What the module covers

| Variable | Meaning |
|---|---|
| `sent` | how many of the scenario's reliable items the application has queued |
| `st` | per reliable item: `none`, `queued`, `numbered`, `inflight`, `lost`, `acked` |
| `q` | per channel, its FIFO of queued items not yet numbered |
| `ready` | the ready list: channels with queued items, in turn order |
| `numQ` | numbered items not yet first-written, in numbering order |
| `clock` | per level, the clock measured from the last level taken (bounded by the largest stride) |
| `rr` | the channel the last item was taken from |
| `unrelQ` | unreliable items queued for this update |
| `budget` | items this update may still write |
| `losses` | transport packet losses so far |
| `drops` | unreliable items dropped at the end of an update (counted, capped) |
| `written` | ghost: per channel, the orderings first-written, in order (finite scenarios only) |
| `classBad` | ghost: a write took a class while a higher class had an item |
| `unrelHeld` | ghost: an update ended with unreliable items queued and budget left |
| `resendQ` | the resend FIFO: written, unacked reliable items, least recently sent first; a write moves the item to the tail, an ack removes it |

Actions: `Queue` (the application queues the next item of the scenario
`Msgs`; CONFIG, when in it, is first, so its push to the front of channel 0
and of the turns is this append), `Requeue(i)` (an item of `Refill` queued
again once acked: a channel that keeps sending, an endless stream on a bounded
state), `Number` (the ready head's FIFO head numbered, the channel rotated),
`Write` (the builder takes `Pick`, once the call's numbering is done; a
reliable write that spends the call's plan, no item numbered or lost left,
resets the clocks), `Tick` (the update ends when nothing more can be numbered
or written: leftover unreliable items are dropped and counted, the budget
refills, the next update's unreliable items arrive; the clocks are left),
`Ack(i)` and `Lose(i)` (the transport's packet ack or loss for the packet that
carried item `i`, owner rule R1; a loss queues the item again as a
retransmit).

Properties:

| Property | Kind | Meaning |
|---|---|---|
| `TypeOK` | invariant | every variable in its range |
| `ReadyIsQueued` | invariant | the ready list holds each channel with queued items once, and no other |
| `FirstWriteInOrderPerChannel` | invariant | within each channel, first writes follow the ordering |
| `UnreliableNotBlockedByWindow` | invariant | an update never ends with unreliable items queued and budget left: a closed window ends the reliable phase and the unreliable phase still runs. Under the design constants `Tick` needs `~CanWrite`, which unreliable items with budget make false, so it holds by construction; `MCRNSchedulerBrokenStallEndsPass.cfg` is the code change it guards against. |
| `ClassOrderBeforePriority` | invariant | no write took a class while a higher class had an item it could write. Under the design order `Write` takes the first class with an item, so it holds by construction in every passing case; `MCRNSchedulerBrokenUnreliableFirstClassOrder.cfg` is the code change it guards against. |
| `RtxHeadBoost` | invariant | each retransmit channel offers only its lost item earliest in `resendQ` at the best level among its lost items. It is stated from `resendQ` (`FifoHead`), not from `RtxHead`; under the design constants `Cands` is built from the same definitions, so there it holds by construction and guards `Cands` against drift. It bites in the two broken cases below. |
| `NoStarvation` | liveness | every queued reliable item is eventually first-written; per item, so per channel: under `Refill` one channel always has an item queued and the other channel's item still gets its turn |

Fairness: weak fairness on `Queue`, `Number`, `Write`, `Tick` and every
`Ack(i)`. Losses are bounded by `MaxLoss` and not fair; `Requeue` is the
application's choice and not fair.

## The instances

- `MCMsgs`, the code's: CONFIG (IMMEDIATE) at the head of channel 0, one MEDIUM
  message on channel 0 and one on channel 1; MEDIUM unreliable items one per
  update, a window of 2, a budget of 1 item per update and 3 transport losses
  (channel 0's two items can await retransmission together after the earlier
  one was resent, resend FIFO order 2, 1).
- `MCMsgsReserved`, the reserved levels: channel 0 queues LOW then IMMEDIATE,
  channel 1 HIGH; IMMEDIATE unreliable items, a window of 2, a budget of 2 (so
  both of channel 0's items are numbered in one call and the builder sees
  them together), no loss.
- `MCMsgsKeepsSending`, a channel that keeps sending: CONFIG, two MEDIUM
  messages on channel 0 queued again whenever acked (`Refill = {2, 3}`), one
  MEDIUM message on channel 1; a window of 1, so the turns decide which
  channel gets each number; a budget of 1, 1 loss.

## Cases

Measured on an AWS bench (2026-11-13, after the drain reset), 2 workers, 110 s budget.

| Case | Constants | Result |
|---|---|---|
| `MCRNScheduler.cfg` | code instance, every rule on, every invariant and `NoStarvation` | pass 3235 distinct states, 6.7 s |
| `MCRNSchedulerBrokenPerMessagePriority.cfg` | reserved levels, `ChannelFIFO = FALSE` | invariant `FirstWriteInOrderPerChannel`, exit 12, 6-state trace (30 distinct states, 1.0 s) |
| `MCRNSchedulerBrokenUnreliableStarvesOrdered.cfg` | code instance, `UnreliableLast = FALSE` | temporal `NoStarvation`, exit 13, lasso of 12 states (68 distinct, 1.5 s) |
| `MCRNSchedulerBrokenUnreliableFirstClassOrder.cfg` | code instance, `UnreliableLast = FALSE` | invariant `ClassOrderBeforePriority`, exit 12, 4-state trace (10 distinct, 1.1 s) |
| `MCRNSchedulerBrokenRtxPerMessage.cfg` | code instance, `ChannelFIFO = FALSE` | invariant `RtxHeadBoost`, exit 12, 10-state trace (144 distinct, 1.1 s) |
| `MCRNSchedulerBrokenStallEndsPass.cfg` | code instance, `StallEndsPass = TRUE` | invariant `UnreliableNotBlockedByWindow`, exit 12, 11-state trace (105 distinct, 1.1 s) |
| `MCRNSchedulerReservedLevels.cfg` | reserved levels, every rule on | pass 4477 distinct states, 10.9 s |
| `MCRNSchedulerKeepsSending.cfg` | keeps sending, every rule on | pass 1272 distinct states, 3.8 s |
| `MCRNSchedulerBrokenReadyKeepsTurn.cfg` | keeps sending, `ReadyRotate = FALSE` | temporal `NoStarvation`, exit 13, lasso of 25 states (1220 distinct, 5.7 s) |
| `MCRNSchedulerBrokenRtxById.cfg` | code instance, `RtxSendOrder = FALSE` | invariant `RtxHeadBoost`, exit 12, 13-state trace (403 distinct, 1.3 s) |

What the earlier instance could not show, measured with the cases' constants
changed (bench runs, not cases): the earlier `UnreliableNotBlockedByWindow`,
`(budget > 0 /\ unrelQ > 0 /\ ~WindowOpen) => CanWrite`, held with every
guard reversed at once (`ChannelFIFO = FALSE`, `UnreliableLast = FALSE`: 183
distinct states, no error), since the unreliable candidate never read the
window; `ChannelFIFO = FALSE` on the code's instance passes
`FirstWriteInOrderPerChannel` (3740 distinct states, no error), since all
MEDIUM items behind a CONFIG head leave per-message priority nothing to
reorder; and `ReadyRotate = FALSE` on the finite `MCMsgs` passes
`NoStarvation` (2634 distinct states, no error), since a finite scenario
drains whatever the turns do.

## Reversed witnesses, counterexamples checked by hand

- **`MCRNSchedulerBrokenPerMessagePriority.cfg`** (6 states): channel 0 queues LOW (ordering 0) and
  IMMEDIATE (ordering 1); both are numbered in one call (budget 2, window 2,
  the channel rejoining the turns after the first); the first write takes
  ordering 1. The code plans both as NEW_RELIABLE items of channel 0, and
  `rn_builder_plan` groups them with the earlier one as the head at the
  group's boost IMMEDIATE, so it writes ordering 0 first
  (`test_rnscheduler_witness_replays`, `test_channel_fifo_kept_per_channel`).
  Reserved levels: no v1.1.x caller can set LOW or IMMEDIATE on a message.
- **`MCRNSchedulerBrokenUnreliableStarvesOrdered.cfg`**: a lasso: CONFIG
  numbered, then every update writes its one arriving unreliable item and
  ticks, forever; the numbered CONFIG holds the room and nothing else is
  numbered or written. The code takes classes in the budget's order and runs
  the unreliable phase only after the reliable phase ends
  (`rn_endpoint_next_packet_bytes`), so CONFIG and the two messages go in the
  first three updates. `ClassOrderBeforePriority` is the safety face of the
  same flaw and is left out, so the failure checked is the liveness one.
- **`MCRNSchedulerBrokenUnreliableFirstClassOrder.cfg`** (4 states): the
  same constants, checking only `ClassOrderBeforePriority`: CONFIG is queued
  and numbered, then the write takes the update's unreliable item while that
  numbered item could be written. The code's `rn_packet_builder_write` class loop takes the classes
  in the budget's order, and the unreliable phase starts only after the
  reliable phase ends.
- **`MCRNSchedulerBrokenRtxPerMessage.cfg`**: CONFIG and channel 0's message
  written (`resendQ` 1, 2), both lost; the second lost item appears as another
  candidate instead of the channel offering only its resend-FIFO head at the
  boosted level. The code's `rn_builder_plan` makes one rtx group per channel
  with one head.
- **`MCRNSchedulerBrokenRtxById.cfg`**: CONFIG (item 1) and item 2 queued,
  numbered and written in two updates (`resendQ` 1, 2), item 1 lost and
  resent (`resendQ` 2, 1), then items 1 and 2 lost (3 losses); this case's
  head is item 1. The code's resend FIFO is then 2, 1 (each send pushes to the
  tail), `rn_ordered_walk` hands the builder 2 then 1, and `rn_builder_plan`'s
  stable grouping makes 2 the channel's rtx head at the boosted level
  IMMEDIATE. Unreachable with 2 losses.
- **`MCRNSchedulerBrokenStallEndsPass.cfg`**: CONFIG, channel 0's message and
  channel 1's message queued (ready 0, 1); CONFIG numbered and channel 0
  rejoins the back (ready 1, 0); CONFIG written; tick; channel 1's message
  numbered (ready 0) and written; tick: two numbers out, the window of 2
  closed, channel 0's message at the ready head and stalled, one unreliable
  item and one budget item, and the update ends dropping it. The code there:
  `rn_window_send_first_write` refuses (`send->stalled`), no resend is due,
  so `rn_endpoint_ordered_due` is false; `rn_endpoint_end_reliable` finds no
  `reliable_waiting` and sets `RN_ENDPOINT_WRITE_UNRELIABLE`, and the
  unreliable packet is written. The trace also shows the rotation at work:
  channel 1 is numbered before channel 0's second message.
- **`MCRNSchedulerBrokenReadyKeepsTurn.cfg`** (lasso of 25 states, back to
  16): CONFIG numbered and written, lost and resent, items 2 and 3 queued on
  channel 0, item 4 queued on channel 1 (ready 0, 1); CONFIG acked; item 2
  numbered and channel 0 keeps the head (ready 0, 1), written, acked,
  requeued behind 3; item 3 numbered (ready 0, 1), written, acked, requeued;
  item 2 numbered; and so on: every number goes to channel 0 and item 4
  stays queued. The code pops channel 0
  off the turns and rejoins it at the back after each number
  (`rn_ready_remove_named`, then `rn_ready_push`): after item 2 the list is
  1, 0, and once item 2's ack opens the window item 4 is numbered next.

## Action-to-function map

| Action / operator | Function |
|---|---|
| `Init`, `Seeds` | `rn_packet_builder_init` -> `rn_builder_reset_clocks`; `round_robin = num_channels - 1`, so the first turn is channel 0's |
| `Queue`, `Requeue` (`Enqueue`) | `rn_channels_send`: `rn_channel_queue_append`, and `rn_ready_push` when the FIFO was empty; CONFIG: `rn_ordered_announce_config` |
| `Number` | `rn_ordered_write`'s numbering loop (`sdk/rocketnet_channel.cpp`): the ready head, `rn_window_send_first_write`, `rn_ready_remove_named`, `rn_channel_queue_pop`, `rn_ready_push` when the channel has more |
| `IsHead`, `Boost`, `RtxHead`, `RtxBoost` (caller order: `resendQ` is `rn_ordered_walk`'s FIFO walk) | `rn_builder_plan`: once per call, stable radix passes group the full 16-bit channel and class into contiguous ranges while preserving caller order; a reverse pass stores every position's suffix priority minimum, so each range cursor is its class's head and its suffix minimum is its class's boost; `rn_builder_take_head` advances only that cursor |
| `PickLevel`, `PickIn` | `rn_builder_next_in_class`, one selection operation per current channel/class range head (with `rn_builder_effective_clock`, the pull-forward); `test_packet_builder_matches_reference_order` holds it to the quadratic scan it replaced, take for take |
| `Write` (class order, clock step, `rr'`) | `rn_packet_builder_write`'s class loop, `rn_builder_take_level` |
| `Write`'s clock reset | the end of `rn_packet_builder_write`: every group range of the call's plan taken and no packet refused -> `rn_builder_reset_clocks` |
| `Tick` with unreliable items left | `rn_endpoint_end_reliable` and the unreliable phase of `rn_endpoint_next_packet_bytes` (`sdk/rocketnet_endpoint.cpp`) |

A packet the budget refuses is not written, so the builder restores the
clocks, round-robin cursor and each range cursor to their values at the
packet's start.

## Design choices

1. **The class order is outside priority.** Priority orders items only
   inside one budget class: retransmits and ordered progress, then new
   reliable, then unreliable, whatever their priorities (R6).
2. **Priority boosts the whole channel; channels stay FIFO** (17.3 item 3).
3. **Channels take turns, one message each** (17.3 item 4): across packets by
   the ready list's rotation, inside a packet by the builder's round robin.
4. **The clock advances at take, not at push.**
5. **The pull-forward is at pick.**
6. **The reset is at drain**, of each call's plan, not of the queues.
7. **Ties go to the more urgent level.**
8. **IMMEDIATE is not immediate.** Every priority is written at the next
   update, inside the budget; no priority wakes a thread.
9. **Out of range is never taken** by the builder (17.3 item 10).

## Abstractions in the model

- The budget is counted in items per update, not bytes; byte accounting is
  `RNSendBudget`'s. The room of one `rn_ordered_write` call is the budget
  left: the code's room is one packet's bytes, a separate bound.
- Retransmits and ordered progress are one class, `rtx`; the code keeps them
  as two adjacent classes in the same order. A numbered item not written in
  the call that numbered it is ordered progress in the code's next call; the
  model keeps it in `rel` until written.
- The model offers every channel's lost head to the builder; the code's walk
  plans the due numbers oldest first up to the room, a subset of that.
- Unreliable items are unchanneled and on one level (`ULevel`); in the code
  they are a separate builder in the endpoint's unreliable phase.
- The window is a count of outstanding numbers; ring placement is
  `RNRing`'s. Ready-list memory layout is `RNChannelQueuesLayout`'s;
  `RNChannelQueues` checks membership and order but not the turns' fairness
  (its `Rotate` swaps two members freely and it states no liveness).
