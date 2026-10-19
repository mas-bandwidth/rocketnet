# RNFlows

`RNFlows.tla` models flows on the message server: a fixed flow table whose
ids never repeat, subscriptions tagged with the connection id of their slot,
a send that queues one copy to each subscriber slot's endpoint the way
`rocketnet_message_server_broadcast` does, and the transport's slot churn
between object updates. The SDK does not ship flows, so the `rn-flows` row of
`tla/COVERAGE.tsv` is `owed`: the module is the specification an
implementation of flows is checked against.

## Abstraction

- **Phases.** `application` is the main thread between updates: Create,
  Destroy, Subscribe, Unsubscribe and Send run only there.
  `BeginUpdate` hands over to `transport`, where any number of `Connect`,
  `Disconnect` and `Deliver` steps run; `Observe` ends the update. `Kick`
  runs in the application phase, as `rn_message_object_kick` does from the
  send path: the transport frees the slot at once, the endpoint takes
  nothing more, and the flow lists stay as they are until `Observe`. A kick
  in the middle of a flow send is the kick before or after the atomic
  `Send`: the kicked slot gets no copy either way and no other slot is
  touched.
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
  `Observe` leaves the pairs of ended connections in `wants`; that is safe
  because connection ids never repeat, so no later connection can match
  them.
- **Send.** `Send` queues to every slot that appears in the flow's
  subscriber list and has a bound endpoint, with no connection-tag check.
  A connection-tag check at the send would be a second guard, and it is not
  what keeps the model safe: the drop at `Observe` alone does.
- **Refusals** (`Refuse(r)`: `FLOW_TABLE_FULL`, `FLOW_UNKNOWN`,
  `FLOW_NOT_CONNECTED`, `FLOW_ALREADY_SUBSCRIBED`, `FLOW_SUBSCRIBERS_FULL`,
  `FLOW_CLIENT_FLOWS_FULL` and `FLOW_NOT_SUBSCRIBED`, the model's reason
  names) are taken in a fixed order and change no state, so each is a stutter
  step; their counters are not modelled.
- **Bounds.** `MaxFlows`, `MaxSubsPerFlow` and `MaxFlowsPerClient` are the
  flow table's three bounds. `MaxIds` bounds the ids issued in one run
  (standing for 2^32 - 1 ids, past which create refuses), `MaxConn` the
  connections and `MaxMessages` the sends.

| Action | What it stands for |
|---|---|
| `Create` | an application call that takes a free table index and issues a fresh flow id |
| `Destroy` | an application call that frees the flow's index and every subscription on it |
| `Subscribe` | an application call that links a slot into the flow's subscriber list and the flow into the slot's row |
| `Unsubscribe` | an application call that unlinks the slot from the flow's list and the flow from the slot's row in one step |
| `Send` | one queued copy per subscriber slot with a bound endpoint, as `rocketnet_message_server_broadcast` queues per slot |
| `Observe` | the drop of every subscription of a changed slot, in the RNSlots Observe loop of `rocketnet_message_server_update` |
| `Kick` | `rn_message_object_kick` (no flow change; the drop is the next Observe) |
| `Connect`, `Disconnect`, `Deliver`, `BeginUpdate` | the transport and the object update of `rocketnet_message_server_update` |

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

Both cases are required, group `rnflows`, measured with the pinned `tlacheck`
and JAR of `README.md`, one shard each, 110-second budget. The records are
in `RUNS.tsv`.

| Configuration | Generated | Distinct | Seconds | Expected result |
|---|---:|---:|---:|---|
| `MCRNFlows.cfg` | 4,734,741 | 629,876 | 22.254 | pass - |
| `MCRNFlowsBrokenSendAfterUnsubscribe.cfg` | 230 | 90 | 1.155 | invariant DeliveredOnlyToSubscribers |

The control has two slots, three connections, two flow indices, three ids,
one send, two subscribers per flow and one flow per slot. In reach: a send
fanned out to both slots, the table-full and client-flows-full refusals,
destroy and re-create into the freed index, a send to a destroyed id, a
kick before or after the send, and churn of both slots. Not in reach: the
subscribers-full refusal (two slots never exceed two subscribers), and a
second send, so unsubscribe between two sends is checked only by the
reversed witness's single send after it. In uncommitted runs, two sends with
two ids and two subscribers per flow passed in 92.3 seconds (21,317,073
generated, 3,695,292 distinct), too near the 110-second budget for a
required case, and two sends with one subscriber per flow passed in 25.9
seconds before the Kick action was added. Neither is a measured record or
claimed here.

## The reversed witness

**Send after unsubscribe** (`Broken = {"sendafterunsubscribe"}`): unsubscribe
clears the slot's row but leaves the slot in the flow's subscriber list. The
measured trace: create flow 1; begin an update and connect connection 1;
`Observe` binds slot 1 to it; subscribe it to flow 1; unsubscribe it, which
removes the pair from `wants` while the flow list keeps the slot; send to
flow 1, which queues a copy to slot 1 with nobody entitled; begin an update
and `Deliver` it to connection 1. `DeliveredOnlyToSubscribers` fails at
state 10. Checked by hand against the rule: an unsubscribe unlinks the slot
from the flow's list in the same step that frees the flow from the slot's
row, which is the step the mutant skips. An implementation of flows carries a
unit test that replays this trace.

## Uncommitted checks of the reuse rule

`NoSubscriptionSurvivesReuse` has no committed reversed witness. In a scratch copy, `Broken = {"keepatreuse"}` (Observe skips the
drop) with one slot, two connections, one flow, one id and one send was run
twice with TLC and the pinned JAR:

- checking `NoSubscriptionSurvivesReuse`: violated at state 9 (create,
  connect 1, observe, subscribe, begin an update, disconnect 1, observe: the
  free slot still holds connection 1's subscription), 189 states generated;
- checking `DeliveredOnlyToSubscribers`: violated at state 13 (as above,
  then connect 2 into the slot before observe, send, deliver to 2, which
  never subscribed), 565 states generated.

These are evidence that the invariant is not vacuous and that the drop at
Observe carries delivery safety; they are not measured records.
