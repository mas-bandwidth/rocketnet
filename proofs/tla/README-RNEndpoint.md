# RNEndpoint: two endpoints end to end, with CONFIG

`RNEndpoint` is the model `docs/CHANNELS.md` section 15 plans for layer 7:
two message endpoints, one at each end of a connection, joined by two
instances of `proofs/tla/Link.tla` (`AB` carries a's packets to b, `BA` carries b's
to a), with the transport's ack semantics and the CONFIG announcement of
sections 10.3 and 13.4. It is written before the code: it is the spec the
CONFIG card (`rocketnet-config-announcement`) and the endpoint card
(`rocketnet-endpoint-pure`) implement, and its actions carry the names of
the planned functions of section 10.1. No code implements it yet, so
`proofs/tla/COVERAGE.tsv` holds its row as owed by `rocketnet-endpoint-pure`.

## What is modelled

- **Reset** (`rn_endpoint_reset`): both ends start a connection. Each queues
  CONFIG (record 0) at the head of IMMEDIATE with the first reliable number,
  so the first packet an end writes carries it.
- **Send** (`rn_endpoint_send`): the application sends messages 1..N in index
  order. A message on a channel outside the sender's own `num_channels` is
  refused. A reliable or reliable-ordered message takes the next reliable
  number; an unreliable one waits for its single write.
- **WritePacket** (`rn_endpoint_write_packet`): a packet carries reliable
  records whose numbers are inside the window `U .. U + W - 1` (first sends
  and resends alike), waiting unreliable records, and the transport's pending
  acks. An unreliable record is written once and is gone after.
- **PacketFailed** (`rn_endpoint_packet_failed`): the packet was written and
  the transport never sent it (a burned id). Its reliable records stay due;
  its unreliable records are gone.
- **OnAcked** (`rn_endpoint_on_acked`): the records of the end's own packets
  that the remote endpoint opened are acked.
- **OnPayload** (`rn_endpoint_on_payload`): an end opens a packet from its
  remote endpoint. The transport acks the packet's reliable records whatever follows
  (P2). CONFIG, first in the packet, goes through **ConfigCompare**; a
  mismatch is `CONFIG_MISMATCH` and parsing stops. Data records are checked
  against the receiver's own `num_channels`: a channel outside it is a
  `VIOLATION` and parsing stops (R6). Otherwise a fresh reliable message is
  received once, an ordered one is held until its channel predecessors are
  received and then delivered with every hold it releases, and an unreliable
  one is delivered at once. An end in error opens and discards.
- **Tick** (`rn_endpoint_tick`): the reliable timeout. An end still waiting
  for the remote endpoint's CONFIG, or holding an unacked reliable record, enters
  `RELIABLE_TIMEOUT`.
- The transport's own steps: **WriteAck** sends pending acks with no message
  records (a keepalive); **AckSilence** drops pending acks unsent (ack history
  overflow, P5).

An end in error does nothing more: the object disconnects it (section 11).

## What is abstracted, and why

- **Packet ids.** An ack names the records of the opened packet, not a packet
  id. The map from an acked id to the records it carried, its eviction and
  the never-repeating id are `RNWindow`'s sent map, checked there. Taking
  the records directly keeps "acked implies opened" (P2) exact at record
  level: every acked record was in a packet the remote endpoint opened.
- **Numbering, the window and the receiver's slots** are taken at the level
  `RNWindow` and `RNChannelTypes` check: the count bound `U + W`, a received
  set standing for `R` and the bitset, duplicates dropped, ordered messages
  held on their channel. The 24-bit wire number, `U'` inside blocks and the
  descriptor slots are not restated.
- **The resend delay** is abstract, as in `RNWindow`: an unacked record inside the
  window may be resent at any step. `ResendDelayAboveRtt = TRUE` (the liveness cases)
  narrows this to the real ordering of the resend delay above the RTT: a record is due
  again only once no copy of it is in flight and no ack of it is pending or in
  flight back. The timing that makes this so is `RNResendTiming`'s.
- **The reliable timeout.** Under `TimeoutAnyTime = TRUE` (the safety cases)
  `Tick` may fire at any moment, so safety holds wherever the timeout lands.
  Under `FALSE` (the liveness cases) it fires only once the remote endpoint is in error
  and silent: that a live remote endpoint's resends land before `reliable_timeout` is
  `RNResendTiming`'s `AttemptsBeforeTimeout`.
- **A configuration** is its `num_channels`, the field the instances vary.
  ConfigCompare compares the whole configuration value, which stands for the
  field-by-field comparison of section 13.4 (version, enabled mask,
  `reliable_messages_in_flight`, `receive_message_buffer_bytes`, `max_message_bytes`,
  `fragment_bytes`): any differing field is the same mismatch.
- **Duplicated packets** are the transport replay window's to drop
  (`ReplayWindow`), so the instances run Link with `Duplication = FALSE`.
  Resends still put several copies of one reliable record in flight in the
  safety cases, and the receiver's dedupe is what keeps them exactly once.
- **Inside one packet** the records are processed CONFIG first, then data; a
  data record outside the receiver's bounds refuses the packet's data as a
  whole. The order of records inside a packet beyond that is section 9.3's.
- **Bytes, the ring, blocks, receipts and priorities below IMMEDIATE** are
  other modules' (`RNRing`, `RNSendSpans`, `RNReceipts`, `RNScheduler`).
- `WriteAll = TRUE` makes a packet carry every due record and every waiting
  unreliable record, a model-size knob: which subset a packet carries is
  `RNScheduler`'s and `RNSendBudget`'s. On an in-order link it also leaves
  ordering unexercised: every packet carries every unacked record inside the
  window, so a later ordered message never arrives without the one before
  it, and `OrderedInOrder` holds whatever the receiver does.
  `MCRNEndpointPartial.cfg` runs `WriteAll = FALSE` (a packet carries any
  subset of the due records) so the hold is what keeps the order, and
  `MCRNEndpointBrokenOrderIgnored.cfg` shows it.

## Properties

Safety (every case lists them):

| Invariant | Meaning |
|---|---|
| `TypeOK` | variable domains; each link within its capacity |
| `ConfigFirstReliable` | CONFIG takes the first reliable number (10.3) |
| `WindowBounded` | no more than `W` reliable records are ever writable (6.2) |
| `AckedImpliesOpened` | an acked record was in a packet the remote endpoint opened (P2) |
| `ReliableExactlyOnce` | a reliable message is delivered at most once, and only if the remote endpoint's application sent it and Send accepted it; with `EveryReliableDelivered`, exactly once |
| `UnreliableAtMostOnce` | an unreliable message is delivered at most once, and only if sent |
| `OrderedInOrder` | an ordered message is delivered only after every ordered message before it on its channel |
| `MismatchErrorsBothEnds` | an end that takes the remote endpoint's CONFIG takes an equal one, and `CONFIG_MISMATCH` means a real difference: every end that opens a differing CONFIG errors |
| `NoViolationWhenEqual` | with equal configurations, data records before CONFIG never trip the receiver's bounds (R6) |

Liveness (`MCRNEndpointLive.cfg`, under `LiveSpec`):

| Property | Meaning |
|---|---|
| `ConfigVerifiedDespiteFirstLoss` | with equal configurations and each link losing its first packet, both ends take the remote endpoint's CONFIG |
| `EveryReliableDelivered` | with equal configurations, every reliable message the remote endpoint sent is delivered (no hang) |
| `MismatchBothEndsError` | with differing configurations, both ends end in error (no hang) |

Fairness is for liveness only: the application sends; the oldest unacked
reliable record is written again while it can be (the resend FIFO's
guarantee, as in `RNWindow`); a waiting unreliable record is written; acks go
out; the head of each wire is opened; a due timeout fires. Strong where a
lost send or a full link disables the step for a while.

## Cases

The instance: N messages per side, the first N - 1 reliable ordered on
channel 0 and the last unreliable on channel 1; `W = 2`; two
configurations, equal (`num_channels` 2 at both ends) and differing (2 at
a, 1 at b), both explored from `Init`. The passing cases and the order
witness run Link capacity 1; the two CONFIG witnesses run capacity 2.

| Case | Spec and terrain | Expected |
|---|---|---|
| `MCRNEndpoint.cfg` | `Spec`, N = 3, capacity 1, `"arbitrary"` in-order loss both ways, failed sends, ack silence, timeouts at any time, abstract resend delay; constraints `MCOneTimeout` and `MCOneWay` | pass |
| `MCRNEndpointLive.cfg` | `LiveSpec`, N = 2, capacity 1, `"first"` loss both ways, `ResendDelayAboveRtt` | pass |
| `MCRNEndpointBrokenConfigUnreliable.cfg` | as Live with `ConfigReliable = FALSE`, capacity 2 | temporal `ConfigVerifiedDespiteFirstLoss` |
| `MCRNEndpointBrokenConfigNotCompared.cfg` | as the safety case with `ConfigCompared = FALSE`, capacity 2, no constraints | invariant `MismatchErrorsBothEnds` |
| `MCRNEndpointTwoWay.cfg` | `Spec`, N = 2, capacity 1, both ends sending, the safety terrain; constraint `MCOneTimeout` only (bench) | pass |
| `MCRNEndpointPartial.cfg` | the safety case with `WriteAll = FALSE` (bench) | pass |
| `MCRNEndpointBrokenOrderIgnored.cfg` | the Partial case with `OrderChecked = FALSE` | invariant `OrderedInOrder` |

### What makes the controls finish, and what each reduction gives up

- `OpenAny(e)` opens the head of the wire into `e` rather than quantifying
  over every value of `Pkts` (4,096 records at N = 3). Link's `Deliver`
  hands over only the head, so the steps are the same; only the
  enumeration per state is gone.
- Capacity 1 on the controls. With in-order loss, a second copy in flight
  adds interleavings of the same records, not a new delivery rule; the
  witnesses keep capacity 2.
- `MCOneTimeout` (safety only): the timeout fires at any moment, at one end
  per behaviour. An end in error is inert: it opens and discards and writes
  nothing, which under `"arbitrary"` loss is a live end that stays silent
  while the link drops every packet into it. A second timed-out end adds
  no delivery state, only its `err` value, which no invariant reads for
  `RELIABLE_TIMEOUT`.
- `MCOneWay` (the one-way safety case only): only a's application sends;
  b resets, writes CONFIG and acks, and opens a's packets. The model is
  symmetric in the two ends except for `cfg`, so b's data to a is a's data
  to b renamed, and each direction on its own is covered. The renaming
  does not cover interference between the directions: it assumes an end's
  sent records and its received records never meet in one rule. A
  receiver that confuses them (a planted bug that takes as fresh any record
  not in `rcvd[e] \ acked[e]`, so a message the end has received is taken
  again once its own send of the same number is acked) passes the one-way
  case at its full 736909 distinct states and fails `ReliableExactlyOnce`
  in the two-way case within seconds. `MCRNEndpointTwoWay.cfg` checks both
  ends sending at N = 2 under `MCOneTimeout` alone. The differing pair puts
  the larger `num_channels` at a, so a's channel-1 message is the data
  record outside b's bounds (the `VIOLATION` path); b's own refusal of that
  channel is `Send`'s guard. The full two-way product at N = 3 did not
  finish in 280 s with 4 workers (over 3.5 million distinct states, queue
  growing), with or without timeouts at any time.
- `WriteAll = TRUE` on the one-way, two-way and liveness cases leaves
  `OrderedInOrder` unexercised (see above): `OrderChecked = FALSE` under
  `MCRNEndpoint.cfg` passes at its full 736909 distinct states, and the
  same argument holds for every `WriteAll = TRUE` case. The Partial case
  is where the ordering rule is checked.
- Liveness stays at N = 2: at N = 3 and capacity 1 it completes (86,113
  distinct states, no violation) but in 188 s, over the budget.

### Measured records

Measured with the pinned runner and JAR on a 16-CPU Linux bench under a
one-minute load of 5 to 20, each case its own 110-second shard on two
workers, merged into `proofs/tla/RUNS.tsv` with `tlacheck merge --keep proofs/tla/RUNS.tsv` (`groups --stale` prints `[]`):

| Case | Gate | Exit | Result | Generated | Distinct | Seconds |
|---|---|---:|---|---:|---:|---:|
| `MCRNEndpoint.cfg` | required | 0 | PASS | 5708001 | 736909 | 44.018 |
| `MCRNEndpointLive.cfg` | required | 0 | PASS | 61813 | 24370 | 35.673 |
| `MCRNEndpointBrokenConfigUnreliable.cfg` | required | 13 | PASS (temporal `ConfigVerifiedDespiteFirstLoss`) | 24935 | 11165 | 7.317 |
| `MCRNEndpointBrokenConfigNotCompared.cfg` | required | 12 | PASS (invariant `MismatchErrorsBothEnds`) | 41 | 33 | 1.605 |
| `MCRNEndpointBrokenOrderIgnored.cfg` | required | 12 | PASS (invariant `OrderedInOrder`) | 47823 | 10350 | 3.283 |
| `MCRNEndpointTwoWay.cfg` | bench | 124 | FAIL (debt `endpoint-two-way-safety-over-budget`) | - | - | 110.332 |
| `MCRNEndpointPartial.cfg` | bench | 124 | FAIL (debt `endpoint-partial-write-safety-over-budget`) | - | - | 110.335 |

The two bench cases complete outside the budget. Run on the same bench
with the same JAR, TLC on two workers with `-deadlock` and no budget:
`MCRNEndpointTwoWay.cfg` completes with no error, 37543692 states
generated, 5082866 distinct, in 4 min 12 s; `MCRNEndpointPartial.cfg`
completes with no error, 30828410 generated, 3326097 distinct, in
2 min 56 s. Those runs are evidence for this document, not records: the
records stay the 110-second debt rows. The one-way safety control took
89 s of 110 under a one-minute load of 35 to 55; a refresh on a heavily
loaded bench may need a quieter one.

## Reversed witnesses, each checked by hand

The traces were read by hand against `docs/CHANNELS.md` 10.3 and 13.4.
No code implements CONFIG or the endpoint yet (`sdk/` defines only the
error code), so the check is against the design, which is the spec the
two code cards implement.

**`MCRNEndpointBrokenConfigNotCompared.cfg`** (`ConfigCompared = FALSE`,
invariant `MismatchErrorsBothEnds`, 3 states). Init picks the differing
configurations, `num_channels` 2 at a and 1 at b. a writes its first packet,
`[rel |-> {0}, unrel |-> {}, acks |-> {}]`: CONFIG alone, at the head of
IMMEDIATE with reliable number 0 (10.3). b opens it with no comparison and
records `remoteCfg[b] = 2` while its own `cfg[b] = 1`, with no error. Section
13.4 requires "any difference is DISCONNECT_REASON_CONFIG_MISMATCH"; without the
comparison b goes on as if both ends were configured the same, so the
"identical and verified" model of 10.3 is lost on the first CONFIG.

**`MCRNEndpointBrokenConfigUnreliable.cfg`** (`ConfigReliable = FALSE`,
temporal `ConfigVerifiedDespiteFirstLoss`, 14 states then stuttering).
Equal configurations, each link losing its first packet ("first"). b writes
its first packet carrying CONFIG as an unreliable record; the BA link loses
it. a writes its first packet carrying CONFIG; the AB link loses it. Both
unreliable CONFIG records are gone after their single write. The rest runs
normally: both applications send, the reliable ordered message 1 is
delivered at each end, acks return, the unreliable message 2 travels; then
nothing is enabled that changes `remoteCfg`, and the behaviour stutters with
`remoteCfg = [a |-> 0, b |-> 0]`: neither end ever verifies. Section 10.3 rules
this out by giving CONFIG the first reliable number, "so it is resent until
acked"; with that rule the same first losses are followed by a resend, which
the model's `WriteOldest` fairness carries.

**`MCRNEndpointBrokenOrderIgnored.cfg`** (`OrderChecked = FALSE`,
invariant `OrderedInOrder`, 9 states). Equal configurations, `num_channels`
2 at both ends. a's application sends messages 1 and 2, both reliable
ordered on channel 0, which take reliable numbers 1 and 2 behind CONFIG.
a writes a packet carrying CONFIG alone; b opens it, its comparison passes
and it owes the ack of record 0. b writes its first packet (its CONFIG)
with that ack; a opens it, takes b's CONFIG and marks record 0 acked, so
the window is now numbers 1 and 2. a writes a packet carrying message 2
and not message 1 (with `WriteAll = FALSE` a packet carries any subset of
the due records). b opens it: message 2 is fresh, and with no predecessor
check it is released at once, `deliv[b] = <<2>>` while message 1 has never
arrived. Section 7.2 holds a RELIABLE_ORDERED message until every earlier
message on its channel is received; with that check, message 2 waits in
`held[b]` until message 1 arrives and both are then delivered in order.

## Action-to-section map

| Action / operator | Planned function (10.1) | Design section |
|---|---|---|
| `Reset` | `rn_endpoint_reset` | 10.1; 10.3 (CONFIG queued at the head of IMMEDIATE, first reliable number) |
| `Send(e)` | `rn_endpoint_send` | 10.1; 6.2 (late numbering); 7.1 (channel bound) |
| `WritePacket(e, S, Us)` | `rn_endpoint_write_packet` | 9.3 (packet writing); 6.2 (count bound `U + W`); 6.4 (resend until acked) |
| `PacketFailed(e, S, Us)` | `rn_endpoint_packet_failed` | 6.4 (a failed send marks its records due); P5 |
| `OnAcked(e, as)` | `rn_endpoint_on_acked` | 6.4 (ack marks records); P2, P4 |
| `OnPayload(e, p)` | `rn_endpoint_on_payload` | 13.3 (receiver's checks in order); 6.5 (duplicates); 7.2 (dispatch, holds); R6 |
| `ConfigCompare(e)` | the CONFIG comparison inside `rn_endpoint_on_payload` | 10.3; 13.4 (every field compared, mismatch is `DISCONNECT_REASON_CONFIG_MISMATCH`) |
| `Tick(e)` | `rn_endpoint_tick` | 10.3 (no CONFIG within `reliable_timeout` is `RELIABLE_TIMEOUT`); 6.4 |
| `err` | `rn_endpoint_error` | 10.1; 12 (`ROCKETNET_DISCONNECT_REASON_*`) |
| `WriteAck(e)`, `AckSilence(e)` | the transport (not the endpoint) | section 2, P2 and P5 |
