# RNFlows

`RNFlows.tla` models flows on the message server, the design of
`docs/FLOWS.md`: a fixed flow table whose ids never repeat, subscriptions
tagged with the connection id of their slot, a send that queues one copy to
each subscriber slot's endpoint the way `rocketnet_message_server_broadcast`
does, and the transport's slot churn between object updates. No code
implements it yet; `proofs/tla/COVERAGE.tsv` holds the `rn-flows` row as owed to
the first implementation card, `rocketnet-flows-table`.

## Abstraction

- **Phases.** `application` is the main thread between updates: Create,
  Destroy, Subscribe, Unsubscribe and Send run only there (FLOWS.md F6).
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
| `Kick` | `rn_message_object_kick` (no flow call; the drop is the next Observe) |
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
  free slot still holds connection 1's subscription), 189 states generated;
- checking `DeliveredOnlyToSubscribers`: violated at state 13 (as above,
  then connect 2 into the slot before observe, send, deliver to 2, which
  never subscribed), 565 states generated.

These are evidence that the invariant is not vacuous and that the drop at
Observe carries delivery safety; they are not measured records.

## Revision 1.1: the send path ends no connection

Standard rule: MESSAGES.md m1.7. A flow send to a subscriber whose memory is
full is refused for that slot and counted; the connection goes on. `Kick`
(a connection ended from the application thread, in the middle of a flow
send) is now reachable only under the reversed witness `Broken =
{"sendkick"}`, v1.0's send-side kick; connections end in the transport phase
only (`Disconnect`: a receive-side exhaustion, silence or an explicit
disconnect). The action property `SendNeverEnds` (`phase = "application" =>
owner' = owner`) says no step of the application phase changes a slot's
connection.

| Case | Expected | Exit | Distinct |
|---|---|---:|---:|
| `MCRNFlows.cfg` (now also `SendNeverEnds`) | pass | 0 | 402316 |
| `MCRNFlowsBrokenSendKick.cfg` | `SendNeverEnds` | 13 | 49 |

Witness trace, read by hand: a client connects and subscribes to a flow; in
the application phase `Kick` ends its connection (`owner'` differs). The code
before revision 1.1 did this when a flow send found the subscriber's region
full: `rn_region_send_alloc` exhausted the region and the transport ended the
connection at the next decision. `MCRNFlows.cfg` falls from 629876 distinct
states to 402316: the states where a slot was ended in the middle of the
application phase are no longer reachable. The measured-cases table above is
the revision 1.0 record.
