# RNSlots: connection tags and message views

`RNSlots` extends the draft in `docs/CHANNELS.md` section 15.1. The object
observes the transport's final slot owners, resets changed endpoints, and
admits each payload or acknowledged packet id against its captured connection
tag. A slot index never identifies a connection by itself.

## Release lifetime

A delivered message is an opaque byte view into the connection's region.
The application releases every view before its next object update. An update
called while a view remains held is the application's misuse:
`rn_message_object_update_begin` counts `MESSAGE_NOT_RELEASED`, asserts in a
debug build, takes every view back (`held = 0`), runs the update and returns
`ROCKETNET_UPDATE_MESSAGE_NOT_RELEASED`. Views cannot survive an update. The
model has both branches: `BeginUpdate` with every held set empty (result
NONE), and `RejectUnreleasedUpdate` with a view held, which takes the views
back, records the named result and enters the transport phase like
`BeginUpdate`; the model continues. The `updatewithheld` mutation carries the
views into the transport phase instead, violating
`UpdateRequiresReleasedViews` (a name kept from when the model ended at the
misuse: it now says no view is carried into an update).

`Release` returns the view only (`rn_message_view_release`: the view marked
released, the held count down); `ReleaseReturnsView` says a released record is
no longer held. The region's charge for every delivery of the last update,
released or taken back, returns when the next update begins
(`rn_endpoint_begin_update` in `rn_message_object_update_begin`), before the
transport runs; `ReleasedRecordsReusable` says the region is empty in the
transport phase. A record that cannot fit during an update enters `Exhausted`
without partial delivery.
The next nonstuttering step is `Kick`, with reason
`MESSAGE_MEMORY_EXHAUSTED`, followed by region reset before reuse. No API
error lets the application continue an exhausted connection. Messages already
returned to the application still require release before another update;
multiple messages received in the same update can exhaust the region.

## Transport and finite bounds

There is one unchanged `Link` instance. Packets carry slot, connection,
message and packet ticket. `TransportOpen` consumes a data packet and creates
a tagged payload entry. `TransportAck` represents the existing transport's
acked-packet-id notification for an opened packet, including deferred
publication. There is no message acknowledgement protocol or second ack link.
The packet map translates that transport id back to its record.

The model treats a record as one unit of region storage and a held set member
as a view of that same record. It does not model a byte copy, allocator call,
wire encoding, fragmentation, channel order or bandwidth scheduler. These
abstractions establish lifetime and ownership rules; implementation tests
must establish pointer identity and absence of allocation.

Connection ids are unique within the finite run. Packet tickets are reusable
only after every old wire, opened, batched, deferred or pending reference has
drained. Reuse atomically evicts the old map entry. This finite abstraction
permits repeated retransmission without claiming uint64 wrap safety.
Batch sets collapse duplicate entries with the same packet identity; different
tickets and different connection tags remain distinct. Admission order is
nondeterministic, covering both payload and ack orderings.

The safety specification permits arbitrary connection churn, application
requests, deferred notification subsets and invalid update calls (an update
with views held takes them back and continues, as the code does);
configurations ignore terminal deadlock. The two larger controls
(`MCRNSlotsTwoSlots.cfg`, `MCRNSlotsExhaustion.cfg`) override `Misuse` to
FALSE (`MCNoMisuse`): with the continued update after the misuse their state
spaces ran past the 110-second budget (`MCRNSlotsExhaustion.cfg` timed out at
95 s of a shard), so their application releases before every update.
`MCRNSlots.cfg` explores the misuse. `NoOwnEntryDropped` is a local consistency invariant,
not a separately declared mutation witness. Safety
never assumes fairness. The live specification keeps a connection stable,
submits the next message after previous views are released, publishes deferred
notifications at the next update, and assumes fair retries, update processing,
application release and `Link`. The release-paced workload prevents exhaustion.
Fair release alone cannot ensure delivery if earlier arrivals already kick a
connection; liveness makes that admissibility condition explicit.

## Action mapping for the objects implementation

The endpoint and slots names below are the planned functions in
`docs/CHANNELS.md` sections 10 and 11. The release and error-check rows name
required object responsibilities; their final C API spelling belongs to the
objects/API cards.

| Model action | Implementation responsibility |
|---|---|
| `Connect`, `Disconnect` | Transport connection changes during `rocketnet_server_transport_update`; `Connect` clears the slot's transport end reason (`rocketnet_server_protocol_t.end_reasons`) and `Disconnect(s, why)` records it in `rocketnet_server_connection_reset` (TIMED_OUT from the idle sweep, CLIENT_DISCONNECTED from the client's disconnect packet, PROTOCOL_VIOLATION from the integrity limit) |
| `Request` | Message submission to `rn_endpoint_send` |
| `Send` | `rn_endpoint_write_packet` and transport packet submission; retain packet-to-record mapping |
| `TransportOpen` | Transport payload batch tagged with the connection at receive time |
| `TransportAck` | `rocketnet_server_transport_acked_packets` feed, including deferred entries |
| `BeginUpdate` | `rn_message_object_update_begin` with no view held: every connection's deliveries of the last update expire and their region charge returns (`rn_endpoint_begin_update`), before the transport runs |
| `Observe` | `rn_slots_observe`, followed by `rn_endpoint_reset` when the slot is bound to a new connection (`RN_SLOTS_RESET`); a slot going free (`RN_SLOTS_DROP`) is released in O(1) by `rn_message_connection_release`, its region left unreachable until that reset (the lazy reset) |
| `Admit` | `rn_slots_admit` at apply time, then `rn_endpoint_on_acked` or `rn_endpoint_on_payload` |
| `Kick` | Object error handling calls `rocketnet_server_transport_disconnect_client` with the exhaustion reason; the transport records SERVER_DISCONNECTED, which no later `Observe` reads (the kicked slot is observed free at reset) |
| `ResetFree` | `rn_message_connection_release`: the kicked slot freed in O(1), its views released; its region is carved by the `Observe` that binds the slot again |
| `Release` | `rn_message_object_release` / `rn_message_view_release`: the held view is marked released and the held count drops; the region is untouched |
| `RejectUnreleasedUpdate` | `rn_message_object_update_begin` with a view held: `MESSAGE_NOT_RELEASED` counted, a debug assert, every view taken back (`held = 0`), the same expiry as `BeginUpdate`, and the update returns `ROCKETNET_UPDATE_MESSAGE_NOT_RELEASED` |
| `StartPending`, `FinishPending` | `rn_message_object_start`, finish and abort: one start pending per connection (`open.pending`); a second start on the same connection is refused, starts on other connections are independent |
| `EndUpdate` | Return deliveries to the application after processing the current batch |

## Disconnect reason

`rocketnet_server_disconnect_reason` reads the slot's `reason`. A message-layer
end writes it directly: `Kick` the exhaustion reason, `PublicDisconnect`
SERVER_DISCONNECTED (`rocketnet_server_disconnect_client`,
`rocketnet_server_disconnect_all_clients`). A transport end is carried through
the link: the transport records why in `ended` (`end_reasons`), and the
`Observe` that finds a slot it held free copies it into `reason`
(`RN_SLOTS_DROP` in `rn_message_object_update`, reading
`rn_message_link_t.end_reason`). `Observe` writes NONE for a slot it binds to
a new connection and keeps every other slot's reason, so a reason lasts until
the slot's next connection. `ConnectedReadsNone` holds in every control;
`DropReadsTransportEnd` and `ReasonKeptUntilNextConnection` are action
properties of `MCRNSlots.cfg`, `MCRNSlotsTwoSlots.cfg` and
`MCRNSlotsExhaustion.cfg`. The two larger controls take one transport ending
(`TransportEnds <- MCOneTransportEnd`): with all three their state space ran
past the 110-second budget, and `MCRNSlots.cfg` explores all three.

- **Drop reads NONE** (`MCRNSlotsBrokenDropReadsNone.cfg`): connect 1,
  `Observe` binds it, then in the next update the transport times it out
  (`ended` TIMED_OUT). The mutant's `Observe` drops the slot with NONE, the
  code before the link carried the transport's reason, and
  `DropReadsTransportEnd` fails at state 8.
- **Observe clears the reason** (`MCRNSlotsBrokenObserveClearsReason.cfg`):
  connect 1 and bind it, the application disconnects it (SERVER_DISCONNECTED)
  and `ResetFree` frees it; the next update's `Observe`, with the slot still
  free, clears the reason to NONE. This was this model's rule before; the code
  keeps the reason (`docs/CONNECTION-RESET.md`, `disconnect_reason` KEEP).
  `ReasonKeptUntilNextConnection` fails at state 9.

## Measured cases and witnesses

The controls separate three-connection churn, two-slot interaction,
release-paced liveness, and two-record exhaustion. The two-slot control has
two packet tickets and one in-flight copy. The exhaustion control has one
slot, two tickets and two in-flight copies, so duplication and reordering
are enabled and reachable. Their conjunction is not one combined proof:
two-slot interaction with two wire copies exceeds the 110-second budget and
remains an unexplored larger bound. The failed measurement is preserved in
the job evidence; it is not reported as a pass.

The six counterexamples below are replayed from the measured TLC traces.
`BeginUpdate` precedes transport actions and `EndUpdate` returns to the
application. Message numbers in a liveness trace are arbitrary record
identifiers, not an ordering protocol.

- **Apply by slot:** begin an update, connect connection 1, request/send/open
  its record, disconnect it and connect connection 2 before `Observe`.
  `Observe` binds the endpoint to 2. Mutated `Admit` applies the payload tagged
  1 to 2, violating `NoCrossConnectionApply` at state 10. The same invariant
  also covers the transport ack entries; the shortest recorded witness uses
  a payload. This is the same-batch stale-entry hazard in section 10.2.
- **Reconnect keeps its client id:** connect connection 1, open its record,
  then disconnect 1 and connect connection 2 with the same client id before
  `Observe`. The `"clientid"` mutant compares client ids, sees no change,
  and leaves the slot bound with 1's residue; entries are tagged by slot
  state and admitted to 2, violating `ReusedSlotStartsClean` at state 13.
  This is the same-client-id-reconnect hazard the transport's per-connection
  id removes (P9a/P9b).
- **Omit reset:** connect connection 1, request/send/open its record;
  `Observe` binds the slot and `Admit` delivers it into the region. The
  application then ends the slot (`PublicDisconnect`) and `ResetFree` frees
  it in O(1), the lazy reset: its views are released and its region still
  holds 1's record, bound to no connection. Begin another update, connect
  connection 2 and observe it: the mutant's `Observe` binds the slot to 2
  without carving the region, so the `Connected` slot holds 1's record,
  violating `ReusedSlotStartsClean` at state 14. The carve at the binding
  `Observe` (`RN_SLOTS_RESET`) is the reset; freeing the slot does not
  replace it.
- **Carry on after exhaustion:** open two different records in one update
  using two transport tickets. `Observe` binds the slot, the first `Admit`
  fills its one-record region, and the second enters `Exhausted` without
  delivering that record. Mutated `CarryOn` returns to `Connected` instead
  of disconnecting with the named reason. The next-step property
  `ExhaustionOnlyLeadsToDisconnect` fails at state 13.
- **Ignore release** (`MCRNSlotsBrokenReleaseIgnored.cfg`, Broken
  `"releaseignored"`): deliver one record and return to the application; the
  mutant's `Release` records the release but leaves the view held, the
  defect of a `rn_message_view_release` that does not mark the view
  released. `ReleaseReturnsView` fails. (Before this revision the mutant kept
  the region charge in `Release`; the code never returned the charge there,
  so that witness did not reverse the code's release.)
- **No expiry at the update** (`MCRNSlotsBrokenNoExpire.cfg` and
  `MCRNSlotsBrokenNoExpireLive.cfg`, Broken `"noexpire"`): the update does
  not return the last update's region charge, the defect of a
  `rn_message_object_update_begin` without `rn_endpoint_begin_update`. In
  the stable workload one record is delivered and released, and the next
  update enters the transport phase with the region still holding it:
  `ReleasedRecordsReusable` fails. Under `LiveSpec` the next record's arrival
  exhausts the region, triggering `Kick`, and that record is never
  delivered: `EveryReliableDelivered` fails despite the application obeying
  release-before-update and release-paced submission.
- **Update with held views** (`MCRNSlotsBrokenUpdateWithHeldViews.cfg`):
  deliver one record and return to the application with its view held. The
  update takes `RejectUnreleasedUpdate`; the mutant carries the view into
  the transport phase instead of taking it back and sets `badLifetime`,
  violating `UpdateRequiresReleasedViews`.
- **Pending across reset** (`MCRNSlotsBrokenPendingAcrossReset.cfg`): a start
  pending on a slot whose connection the update resets is not cancelled;
  `NoPendingAcrossReset` fails.
- **Two starts pending** (`MCRNSlotsTwoPending.cfg`, no mutation): a
  reachability probe. Two slots are bound and a start is pending on each;
  `NotBothPending` fails, which shows the model allows what the code allows,
  one pending start per connection rather than one per object.

Each reversed witness declares only its intended failing property. Positive
controls check the full safety set; `Live` also checks
`EveryReliableDelivered`. Current measured counts and fingerprints are in
`RUNS.tsv`. Earlier semantic-error and timeout attempts are retained in the
job evidence, not substituted for final completed measurements.

All ten cases match their expected result with up to two workers, a 110-second
per-case budget and the pinned runner/JAR from `README.md`. The measured
totals across separate configurations are 8,915,107 generated and 2,329,260
distinct states in 40.362 seconds; these totals are not one combined state
space. The exact table follows.

| Configuration | Generated | Distinct | Seconds | Expected result |
|---|---:|---:|---:|---|
| `MCRNSlots.cfg` | 72,386 | 38,716 | 1.333 | pass - |
| `MCRNSlotsBrokenApplyBySlot.cfg` | 287 | 201 | 0.676 | invariant NoCrossConnectionApply |
| `MCRNSlotsBrokenClientIdAsConnection.cfg` | 1,522 | 971 | 1.531 | invariant ReusedSlotStartsClean |
| `MCRNSlotsBrokenKickCarriesOn.cfg` | 18,459 | 8,016 | 0.996 | action ExhaustionOnlyLeadsToDisconnect |
| `MCRNSlotsBrokenNoResetOnReuse.cfg` | 1,007 | 643 | 0.696 | invariant ReusedSlotStartsClean |
| `MCRNSlotsBrokenReleaseIgnored.cfg` | 1,056 | 689 | 0.998 | temporal EveryReliableDelivered |
| `MCRNSlotsBrokenUpdateWithHeldViews.cfg` | 149 | 81 | 0.665 | invariant UpdateRequiresReleasedViews |
| `MCRNSlotsExhaustion.cfg` | 7,961,361 | 1,918,511 | 25.977 | pass - |
| `MCRNSlotsLive.cfg` | 756 | 491 | 1.593 | pass - |
| `MCRNSlotsTwoSlots.cfg` | 858,124 | 360,941 | 5.897 | pass - |

## Revision 1.1.2: a freed slot is held until the update ends

Before 1.1.2 the transport could free a slot and give it to a new client in
the same update. `Observe` then saw only the new connection: the old one's end
and its reason were never reported. The transport now holds every slot it
frees (`rocketnet_client_slots_release` writes it above the free stack's top)
until `rocketnet_client_slots_publish` at the end of
`rocketnet_server_protocol_update`, which runs before the message layer's
`Observe`. In the model a slot ended by `Disconnect`, `Kick` or
`PublicDisconnect` is `freed` until the next `Observe`, and `Connect` does not
take it. The action property `EveryEndSeen` (in the three churn controls)
says every slot freed since the last `Observe` is still free when `Observe`
runs.

- **Reuse in the update** (`MCRNSlotsBrokenReuseInUpdate.cfg`, Broken
  `"reuseinupdate"`, the code before the hold): in one update the transport
  connects 1, times it out (`ended` TIMED_OUT) and connects 2 into the same
  slot, which clears `ended` to NONE as `rocketnet_server_connection_id_assigned`
  clears `end_reasons`; the `Observe` that follows finds the slot held by 2,
  and `EveryEndSeen` fails at state 6.

The apply-by-slot and client-id witnesses above need the same-update reuse
to reach their hazard, so their configurations add `"reuseinupdate"`: they
check that the per-connection compare in `Observe` would still catch it.
`RNClientSlotsLayout` checks the hold at the free stack's layout.

## Revision 1.1.2: the integrity limit ends with PROTOCOL_VIOLATION

Before 1.1.2 a connection the server ended at the AEAD integrity limit
recorded NONE as its transport reason, so the game read an ended slot with
no reason. It now records PROTOCOL_VIOLATION, as the client's end already
reads it. `TransportEnds` carries PROTOCOL_VIOLATION for the integrity limit,
and the action property `TransportEndNamed` (in the three churn controls)
says a slot `Observe` finds dropped never reads NONE.

- **Integrity limit reads NONE** (`MCRNSlotsBrokenIntegrityNone.cfg`, Broken
  `"integritynone"`, the code before the change): connect 1 and bind it, the
  transport ends it at the integrity limit with NONE, and the next `Observe`
  drops the slot with NONE; `TransportEndNamed` fails.

## Revision 1.1: no change

MESSAGES.md m1.7 turns a send with no room into a refusal and keeps the
receive side's MESSAGE_MEMORY_EXHAUSTED end. This model's only step to
`"Exhausted"` is `Admit` of a received record into a full region
(`Cardinality(st.region[s]) >= RegionCapacity`), which is the receive side;
no send step reaches it. `MCRNSlotsExhaustion.cfg` and
`MCRNSlotsBrokenKickCarriesOn.cfg` therefore check the end that stays, and
the model, its configurations and its records are unchanged. The send-side
refusal is modelled in RNConnectionMemory, RNSendStoreLayout, RNSendSpans,
RNChannelTypes, RNFlows and ClientSession.
