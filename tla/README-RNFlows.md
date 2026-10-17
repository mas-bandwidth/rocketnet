# RNFlows

`RNFlows.tla` models flows on the message server, the design of
`docs/FLOWS.md`: a fixed flow table whose ids never repeat, subscriptions
tagged with the connection id of their slot, a send that queues one copy to
each subscriber slot's endpoint the way `rocketnet_message_server_broadcast`
does, and the transport's slot churn between object updates. No code
implements it yet; `tla/COVERAGE.tsv` holds the `rn-flows` row as owed to
the first implementation card, `rocketnet-flows-table`.

## Abstraction

- **Phases.** `application` is the main thread between updates: Create,
  Destroy, Subscribe, Unsubscribe and Send run only there (FLOWS.md F6).
  `BeginUpdate` hands over to `transport`, where any number of `Connect`,
  `Disconnect` and `Deliver` steps run; `Observe` ends the update.
- **Slots.** `owner` is the transport's connection in each slot, with ids
  that never repeat (as in `RNSlots`); `bound` is the connection the object
  bound the slot's endpoint to at its last `Observe`.
- **Delivery.** `Deliver` moves a queued copy to `delivered` only while the
  slot's endpoint is bound to the slot's current connection. That is the
  guarantee `RNSlots` checks (`NoCrossConnectionApply`); it is assumed here,
  not re-proved. An endpoint's queue is emptied when its connection changes.
- **Intent.** The ghost `wants` holds the (connection, flow id) pairs the
  application subscribed and has not unsubscribed or destroyed. Each `Send`
  adds to `entitled` one entry per connection in `wants` for that id.
- **Send.** `Send` queues to every slot that appears in the flow's
  subscriber list and has a bound endpoint, with no connection-tag check.
  The design's `FLOW_STALE_SUBSCRIPTION` check is therefore not what keeps
  the model safe: the drop at `Observe` alone does.
- **Refusals** (FLOWS.md section 4) are taken in the design's order and
  change no state, so each is a stutter step; their counters are not
  modelled.
- **Bounds.** `MaxFlows`, `MaxSubsPerFlow` and `MaxFlowsPerClient` are the
  design's three bounds. `MaxIds` bounds the ids issued in one run (the
  design's 2^32 - 1, `FLOW_IDS_EXHAUSTED`), `MaxConn` the connections and
  `MaxMessages` the sends.

| Action | Planned code (docs/FLOWS.md section 3) |
|---|---|
| `Create` | `rn_flows_create` under `rocketnet_message_server_flow_create` |
| `Destroy` | `rn_flows_destroy` under `rocketnet_message_server_flow_destroy` |
| `Subscribe` | `rn_flows_subscribe` under `rocketnet_message_server_flow_subscribe` |
| `Unsubscribe` | `rn_flows_unsubscribe` under `rocketnet_message_server_flow_unsubscribe` |
| `Send` | `rn_flows_first`/`rn_flows_next` and the per-slot send under `rocketnet_message_server_flow_send` |
| `Observe` | `rn_flows_drop_slot` for each changed slot in `rocketnet_message_server_update`'s Observe loop |
| `Connect`, `Disconnect`, `Deliver`, `BeginUpdate` | the transport and the existing object update |

## Properties

- `DeliveredOnlyToSubscribers`: `delivered \subseteq entitled`. No message
  sent to a flow reaches a connection that was not subscribed to it when it
  was sent.
- `NoSubscriptionSurvivesReuse`: every subscription names the connection its
  slot is bound to now. None outlives its connection into a reuse of the
  slot, and none sits on a free slot.
- `BoundsHeld`: no flow over `MaxSubsPerFlow`, no slot over
  `MaxFlowsPerClient`, a free table index holds no subscribers, and the
  flow lists and the per-slot rows agree.
- `TypeOK`.

## Measured cases

Both cases are required, group `rnflows`, measured with the pinned runner
and JAR of `README.md`, one shard each, 110-second budget. The records are
in `RUNS.tsv`.

| Configuration | Generated | Distinct | Seconds | Expected result |
|---|---:|---:|---:|---|
| `MCRNFlows.cfg` | 2,749,692 | 615,196 | 25.943 | pass - |
| `MCRNFlowsBrokenSendAfterUnsubscribe.cfg` | 166 | 75 | 1.185 | invariant DeliveredOnlyToSubscribers |

The control has two slots, three connections, two flow indices, two ids, two
sends, one subscriber per flow and one flow per slot, so the
subscribers-full and client-flows-full refusals, destroy and re-create into
the freed index, a send to a destroyed id, and churn of both slots are in
reach. With two ids and two indices `Create` is never refused for a full
table (the third create has no id left); the uncommitted bound with three
ids and one send reaches it and passed (1,070,282 generated, 200,912
distinct), and three ids with two sends did not finish within 120 seconds on
a loaded machine. Neither is a measured record or claimed here.

## The reversed witness

**Send after unsubscribe** (`Broken = {"sendafterunsubscribe"}`): unsubscribe
clears the slot's row but leaves the slot in the flow's subscriber list. The
measured trace: create flow 1; begin an update and connect connection 1;
`Observe` binds slot 1 to it; subscribe it to flow 1; unsubscribe it, which
removes the pair from `wants` while the flow list keeps the slot; send to
flow 1, which queues a copy to slot 1 with nobody entitled; begin an update
and `Deliver` it to connection 1. `DeliveredOnlyToSubscribers` fails at
state 10. Checked by hand against FLOWS.md: the design's unsubscribe
unlinks the node from the flow's list in the same call that frees it from
the slot's row (sections 2 and 3), which is the step the mutant skips; card
1's test `flows_unsubscribe_stops_later_sends` replays this trace.

## Uncommitted checks of the reuse rule

`NoSubscriptionSurvivesReuse` has no committed reversed witness (the card
names one). In a scratch copy, `Broken = {"keepatreuse"}` (Observe skips the
drop) with one slot, two connections, one flow, one id and one send was run
twice with TLC and the pinned JAR:

- checking `NoSubscriptionSurvivesReuse`: violated at state 9 (create,
  connect 1, observe, subscribe, begin an update, disconnect 1, observe: the
  free slot still holds connection 1's subscription), 148 states generated;
- checking `DeliveredOnlyToSubscribers`: violated at state 13 (as above,
  then connect 2 into the slot before observe, send, deliver to 2, which
  never subscribed), 427 states generated.

These are evidence that the invariant is not vacuous and that the drop at
Observe carries delivery safety; they are not measured records.
