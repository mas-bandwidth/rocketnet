# RNSlots: connection tags and message views

`RNSlots` models the server message object's slots. The object
observes the transport's final slot owners, resets changed endpoints, and
admits each payload or acknowledged packet id against its captured connection
tag. A slot index never identifies a connection by itself.

## Release lifetime

A delivered message is an opaque byte view into the connection's region.
The application explicitly releases every view before its next object update.
Calling update while a view remains held enters
`ROCKETNET_ERROR_MESSAGE_NOT_RELEASED`; the implementation asserts in debug
and returns this named error in release. Views cannot survive an update.
This refines section 11.1's batch lifetime and supersedes the earlier open
choice between automatic expiry and arbitrary explicit lifetime. `BeginUpdate`
checks for empty held sets before running the transport; `Observe` retains
that condition; `RejectUnreleasedUpdate` models the invalid call.
The `updatewithheld` mutation proceeds anyway, violating
`UpdateRequiresReleasedViews`.

`Release` removes the region charge as well as the application view. A record
that cannot fit during an update enters `Exhausted` without partial delivery.
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
requests, deferred notification subsets and invalid update calls. Terminal
API errors are deliberate; configurations ignore terminal deadlock. Safety
never assumes fairness. The live specification keeps a connection stable,
submits the next message after previous views are released, publishes deferred
notifications at the next update, and assumes fair retries, update processing,
application release and `Link`. The release-paced workload prevents exhaustion.
Fair release alone cannot ensure delivery if earlier arrivals already kick a
connection; liveness makes that admissibility condition explicit.

## Action mapping for the objects implementation

The endpoint and slots names below are the planned functions. The release and error-check rows name
required object responsibilities; their final C API spelling belongs to the
objects/API cards.

| Model action | Implementation responsibility |
|---|---|
| `Connect`, `Disconnect` | Transport connection changes during `rocketnet_server_transport_update` |
| `Request` | Message submission to `rn_endpoint_send` |
| `Send` | `rn_endpoint_write_packet` and transport packet submission; retain packet-to-record mapping |
| `TransportOpen` | Transport payload batch tagged with the connection at receive time |
| `TransportAck` | `rocketnet_server_transport_acked_packets` feed, including deferred entries |
| `BeginUpdate` | Entry check in `rocketnet_server_update`, before the transport mutates prior views |
| `Observe` | `rn_slots_observe`, followed by `rn_endpoint_reset` on identity change |
| `Admit` | `rn_slots_admit` at apply time, then `rn_endpoint_on_acked` or `rn_endpoint_on_payload` |
| `Kick` | Object error handling calls `rocketnet_server_transport_disconnect_client` with the exhaustion reason |
| `ResetFree` | Reset the disconnected endpoint's region before the slot can be reassigned |
| `Release` | Object message-release operation returns the view's region charge |
| `RejectUnreleasedUpdate` | Entry check in `rocketnet_server_update` before touching the transport or old views |
| `EndUpdate` | Return deliveries to the application after processing the current batch |

## Measured cases and witnesses

The controls separate three-connection churn, two-slot interaction,
release-paced liveness, and two-record exhaustion. The two-slot control has
two packet tickets and one in-flight copy. The exhaustion control has one
slot, two tickets and two in-flight copies, so duplication and reordering
are enabled and reachable. Their conjunction is not one combined proof:
two-slot interaction with two wire copies exceeds the 110-second budget and
remains an unexplored larger bound. The failed measurement is preserved in
the job evidence; it is not reported as a pass.

The five counterexamples below are replayed from the measured TLC traces.
`BeginUpdate` precedes transport actions and `EndUpdate` returns to the
application. Message numbers in a liveness trace are arbitrary record
identifiers, not an ordering protocol.

- **Apply by slot:** begin an update, connect connection 1, request/send/open
  its record, disconnect it and connect connection 2 before `Observe`.
  `Observe` binds the endpoint to 2. Mutated `Admit` applies the payload tagged
  1 to 2, violating `NoCrossConnectionApply` at state 10. The same invariant
  also covers the transport ack entries; the shortest recorded witness uses
  a payload. This is the same-batch stale-entry hazard in section 10.2.
- **Omit reset:** deliver connection 1's record, return its view, then release
  it. Region storage is empty, but receive/dedup state still belongs to 1.
  Begin another update, disconnect 1, and observe the free slot. The mutant
  leaves that receive state in a `Free` slot, violating
  `ReusedSlotStartsClean` at state 13, before the next client can inherit it.
  Releasing application views does not replace endpoint reset.
- **Carry on after exhaustion:** open two different records in one update
  using two transport tickets. `Observe` binds the slot, the first `Admit`
  fills its one-record region, and the second enters `Exhausted` without
  delivering that record. Mutated `CarryOn` returns to `Connected` instead
  of disconnecting with the named reason. The next-step property
  `ExhaustionOnlyLeadsToDisconnect` fails at state 13.
- **Ignore release:** in the stable workload, deliver and transport-ack
  message 2, return its view and release it. The mutant removes the view but
  retains its region charge. Only after that release is message 1 requested.
  Its arrival exhausts the supposedly reusable region, triggering `Kick`.
  The connection cannot deliver message 1; the recorded lasso continues
  empty updates after reset. `EveryReliableDelivered` fails despite the
  application obeying release-before-update and release-paced submission.
- **Update with held views:** deliver one record and return to the
  application with its view held. Mutated `BeginUpdate` proceeds without a
  release and sets `badLifetime`, violating `UpdateRequiresReleasedViews`
  at state 10. The normal action is disabled and the invalid call takes
  `RejectUnreleasedUpdate` with the canonical named error.

Each reversed witness declares only its intended failing property. Positive
controls check the full safety set; `Live` also checks
`EveryReliableDelivered`. Current measured counts and fingerprints are in
`RUNS.tsv`. Earlier semantic-error and timeout attempts are retained in the
job evidence, not substituted for final completed measurements.

All nine cases match their expected result with up to two workers, a 110-second
per-case budget and the pinned runner/JAR from `README.md`. The measured
totals across separate configurations are 8,913,585 generated and 2,328,289
distinct states in 38.831 seconds; these totals are not one combined state
space. The exact table follows.

| Configuration | Generated | Distinct | Seconds | Expected result |
|---|---:|---:|---:|---|
| `MCRNSlots.cfg` | 72,386 | 38,716 | 1.333 | pass - |
| `MCRNSlotsBrokenApplyBySlot.cfg` | 287 | 201 | 0.676 | invariant NoCrossConnectionApply |
| `MCRNSlotsBrokenKickCarriesOn.cfg` | 18,459 | 8,016 | 0.996 | action ExhaustionOnlyLeadsToDisconnect |
| `MCRNSlotsBrokenNoResetOnReuse.cfg` | 1,007 | 643 | 0.696 | invariant ReusedSlotStartsClean |
| `MCRNSlotsBrokenReleaseIgnored.cfg` | 1,056 | 689 | 0.998 | temporal EveryReliableDelivered |
| `MCRNSlotsBrokenUpdateWithHeldViews.cfg` | 149 | 81 | 0.665 | invariant UpdateRequiresReleasedViews |
| `MCRNSlotsExhaustion.cfg` | 7,961,361 | 1,918,511 | 25.977 | pass - |
| `MCRNSlotsLive.cfg` | 756 | 491 | 1.593 | pass - |
| `MCRNSlotsTwoSlots.cfg` | 858,124 | 360,941 | 5.897 | pass - |
