# RNSendBudget: the per-connection send budget

`proofs/tla/RNSendBudget.tla` is the send budget every connection keeps (the
client's one connection and each server client slot): a rate in kilobits per
second, counted in the transport's wire bytes, enforced as a token bucket the
packet writer asks before it writes each item. The code is
`sdk/rocketnet_send_budget.h`; each of its functions names the action it
implements. The model was written before the code.

`proofs/tla/MCRNSendBudget.tla` and `proofs/tla/MCRNSendBudget.cfg` are the small instance.
The four `proofs/tla/MCRNSendBudgetBroken*.cfg` files are the reversed witnesses. The
cases are rows of `proofs/tla/CASES.tsv` (group `rnsendbudget`) with measured
records in `proofs/tla/RUNS.tsv`.

## The rule

- **The unit.** 1 kbps = 1000 bit/s, the unit of the transport's
  `bandwidth_sent_kbps`. K kbps is K x 125 bytes per second.
- **The bytes.** A packet's wire bytes as the transport records them
  (`rocketnet_ack_endpoint_record_sent`'s `wire_bytes`):
  `ROCKETNET_PACKET_OVERHEAD_BYTES + ack_block_bytes + payload_bytes`, the
  value `rocketnet_write_sealed_payload_in_place` returns.
- **The bucket.** Refilled at each update with rate x elapsed time (time is an
  argument), capped at the depth: the larger of one maximum packet
  (`ROCKETNET_SEND_BUDGET` wire bytes) and 100 ms of budget. Every packet
  spends its wire bytes, and only what fits is spent.
- **The order.** Within an update the writer offers retransmits and ordered
  progress first, then new reliable items, then unreliable items. A reliable
  item that does not fit waits for a later update and is never dropped; once
  one waits, no lower class spends the bucket this update. An unreliable item
  that does not fit the bytes left to its class is dropped and counted.
- **Defaults.** 1000 kbps for the client, 10000 kbps for each server client
  slot; configuration fields in the message config, overridable at create and
  at runtime. A capped budget is 1 kbps to 10000000 kbps.
- **Uncapped.** Inside the message endpoint, a value of 0 or lower to
  `rn_endpoint_set_send_kbps` disables that connection's budget (the public
  calls never pass one: there 0 is the default rate and a negative value is
  refused with `ROCKETNET_STATUS_INVALID_ARGUMENT`): it sends every item, nothing is dropped
  or waits by the budget, and `NeverOverspend` claims nothing about it. A
  positive value caps it again with a fresh full bucket; a value above the
  maximum is refused and changes nothing. Disabling the budget is not
  recommended: without one there is no defined behaviour when a sender
  outruns its path or its receiver.
- **A failed send costs nothing.** A packet is admitted at its maximum wire
  bytes before the transport's `finish_packet`; when the finish returns no
  wire bytes (nothing left), the whole charge is settled back
  (`rocketnet_send_budget_settle( budget, admitted, 0 )`, which restores both
  `tokens` and `bytes_spent`). A reliable item is due again as a retransmit and
  is charged once, when it is resent; an unreliable one is counted failed.

## What the module covers

State:

| Variable | Meaning |
|---|---|
| `tokens` | the bucket, in bytes, `0..Depth` |
| `hist`, `cur` | bytes sent in each of the previous `W - 1` ticks, and in this one: the sliding window |
| `phase` | `"idle"` between updates, `"send"` while the writer runs |
| `rtx`, `rel`, `unrel` | the queued retransmits (and ordered progress), new reliable ids, unreliable items for this update |
| `nextRel`, `inflight`, `acked` | reliable arrivals, ids whose carrying packet is in flight, ids the transport acked |
| `losses`, `drops` | transport losses so far (bounded), unreliable drops counted |
| `orderBad`, `dropBad` | ghosts: a class sent while a higher class waited; an unreliable drop that fit |
| `uncapped` | TRUE when the budget does not apply to this connection |
| `uncapBad` | ghost: an uncapped connection dropped or waited by the budget |
| `fails` | sends the transport refused at finish so far (bounded by `MaxFail`) |
| `failBad` | ghost: a failed send left `tokens` or `cur` other than they were before it |

Actions:

| Action | Code | What |
|---|---|---|
| `Init` | `rocketnet_send_budget_init` | a full bucket |
| `Tick(run)` | `rocketnet_send_budget_refill`, `rocketnet_send_budget_begin_update` | one update: refill capped at the depth, slide the window; with `run`, arrivals and a send phase; without, a stalled update |
| `Send` | `rocketnet_send_budget_admit` returning SEND (`can_send`, then `spend`) | the next class's head fits: spent and written |
| `SendFailed` | `rn_endpoint_packet_failed`, `rn_endpoint_in_place_failed`, `rn_message_flush_unreliable`'s failure branch | the next class's head is charged as `Send` charges it, then the transport refuses the finish: with `RefundOnFail` the charge is settled back whole; a reliable item is queued again as a retransmit, first; an unreliable one is counted failed |
| `Drop` | `admit` returning DROP | the next class is unreliable and its head does not fit: dropped, counted |
| `EndUpdate` | `admit` returning WAIT | nothing more fits: every queue empty, or a reliable head waits; unreliable items behind it are dropped, counted |
| `SetKbps(k)` | `rn_endpoint_set_send_kbps` | 0 or lower uncaps (no bucket, no drops or waits); a positive value up to `MaxKbps` caps it, a fresh full bucket when it was uncapped; above the maximum is refused, unchanged |
| `Ack(r)` | the transport's packet ack (R1) | the packet that carried `r` was acked |
| `Lose(r)` | the transport's loss | that packet was lost: `r` queues again as a retransmit |

Properties:

- `NeverOverspend`: bytes sent in any window of `W` ticks are at most
  `Budget + Depth`, where `Budget = Rate * W` is one window's budget.
- `ReliableNeverDropped`: every reliable id that arrived is queued, in flight
  or acked.
- `UnreliableDroppedOnlyOverBudget`: an unreliable item is dropped only when it
  does not fit the bytes its class may spend (none while a reliable item
  waits).
- `ClassOrder`: no class is sent while a class ahead of it is queued.
- `UncappedNeverDropsOrWaits`: an uncapped connection never drops an item or
  ends a send phase with work queued by the budget.
- `FailedSendCostsNothing`: a send the transport refused at finish leaves
  `tokens` and the window's bytes (`cur`) as they were before it.
- `TypeOK`.
- LIVE `EveryReliableEventuallySent`: with a positive rate and finite reliable
  arrivals, every reliable id that arrives is eventually acked. Fairness: weak
  on `Tick(TRUE)` (the writer runs infinitely often), on `Send`, `Drop` and
  `EndUpdate`, and on each `Ack(r)`. Unreliable arrivals are unbounded and
  chosen by the environment, so a steady unreliable load is among the checked
  behaviours.

## Abstractions

- Time is in ticks and bytes are abstract units; one tick is one update. The
  code refills by rate x elapsed time for any spacing; the model's `Tick(FALSE)`
  covers uneven spacing and stalls (time passing while the writer does not run).
- All reliable items have one size and all unreliable items another. The code
  checks each item on its own size; the tests drive mixed sizes.
- The runtime rate change is modelled as `SetKbps(kb)`: uncapping and capping
  transition as the code does. Changing one capped rate to another positive
  rate keeps the bucket in the model (the code refills at the old rate and
  caps at the new depth); the unit test
  `test_runtime_change_takes_effect_next_window` checks that arithmetic.
- The admit-at-bound-then-settle step of a sent packet
  (`rocketnet_send_budget_max_wire_bytes`, `rocketnet_send_budget_settle`) is
  not modelled: settling only returns bytes the sealed packet did not use, so
  what was sent never exceeds what was spent. The settle of a failed send is:
  `SendFailed` charges the item's size and settles it back with actual 0 in
  one step, as the code charges at write and settles in the failure path of
  the same update, at one update time.
- `SendFailed` keeps the send phase open; the code ends an ordered failure's
  reliable phase for that update. The bucket is the same either way, since the
  charge is refunded whole.

## The small instance

`W = 2`, `Rate = 1` (so `Budget = 2`), `Depth = 2 = MaxPkt`, `RelSize = 2`
(larger than one tick's refill, so a reliable item can be starved),
`UnrelSize = 1`, `UPerTick = 2`, `MaxRel = 2`, `MaxLoss = 1`,
`RefundOnFail = TRUE` and `MaxFail = 1` (one refused finish; the reversed
witnesses keep both, so their verdicts are unchanged); the module
defines `MaxKbps = 2` (so `SetKbps` ranges over 0, 1, 2 and the refused 3)
and `UncapHonoured = TRUE`. Definitions, not constants: a constant would have
forced a new line into the other reversal configurations, and the uncapped
witness replaces `UncapHonoured` with `<-`.

## Reversed witnesses

| Case | Flip | Must fail | The counterexample, checked by hand |
|---|---|---|---|
| `MCRNSendBudgetBrokenOverspend.cfg` | `CheckBudget = FALSE`: spend without the check, the bucket saturating at zero | `NeverOverspend` (exit 12) | In the first update a reliable item (2) and an unreliable item (1) arrive; the reliable item spends the bucket to 0 and the unreliable one is sent anyway; the reliable packet is lost and retransmitted in the same update (2 more): 5 bytes in one tick, above `Budget + Depth = 4`. In the code this is `spend` without `can_send`; the unit test `test_never_exceeds_budget_per_window` fails under that mutation. |
| `MCRNSendBudgetBrokenUnreliableFirst.cfg` | `ReliableFirst = FALSE`: the writer offers unreliable items first | `EveryReliableEventuallySent` (exit 13) | After the first reliable item is acked, the second waits with the bucket at 0. Each update refills 1, an unreliable item arrives and is sent first, spending it; the reliable item (2) never fits. The lasso repeats forever. `ClassOrder` is the safety face of the same flaw and is left out of this cfg so the liveness failure is the one checked. The prefix TLC prints may open with a refused unreliable send (`SendFailed`), refunded whole, before the starving lasso. |
| `MCRNSendBudgetBrokenUncapped.cfg` | `UncapHonoured <- BrokenUncap` (FALSE): an uncapped connection still consults the bucket | `UncappedNeverDropsOrWaits` (exit 12) | In the first update a reliable item (2) and an unreliable item (1) arrive; `SetKbps(0)` uncaps the connection; the reliable item is sent and still spends the bucket to 0, and the unreliable item, which no longer fits, is dropped while `uncapped` is TRUE (5 states). In the code this is the endpoint charging or settling the bucket on an uncapped connection; the unit test `test_endpoint_uncapped_large_sequenced_skips_the_bucket` fails under that mutation. |
| `MCRNSendBudgetBrokenNoRefund.cfg` | `RefundOnFail = FALSE`: a packet the transport refuses at finish keeps its charge | `FailedSendCostsNothing` (exit 12) | In the first update a reliable item (2) arrives; it is charged, the bucket going from 2 to 0 and `cur` to 2, and the transport refuses the finish; the item is queued again as a retransmit with the bucket still at 0 (3 states). In the code this is a failed-finish path that never settles its admitted charge; the unit tests `test_message_budget_refunded_on_failed_large_sequenced_finish`, `test_message_budget_refunded_on_failed_in_place_finish`, `test_message_budget_refunded_on_failed_write_pass_finish` and `test_message_budget_refunded_on_failed_unreliable_flush` fail under that mutation (each fails at the code before the settles were added). |
