# RNEndpoint: two endpoints end to end, with CONFIG

`RNEndpoint` is the model `docs/CHANNELS.md` section 15 plans for layer 7:
two message endpoints, one at each end of a connection, joined by two
instances of `proofs/tla/Link.tla` (`AB` carries a's packets to b, `BA` carries b's
to a), with the transport's ack semantics and the CONFIG announcement of
sections 10.3 and 13.4. The model preceded the implementation;
`sdk/rocketnet_endpoint.cpp` now implements the endpoint and CONFIG paths
and cites the corresponding actions. The `rn-endpoint` row in
`proofs/tla/COVERAGE.tsv` records that source mapping. The abstraction and
bounded evidence below do not establish a refinement proof of the C++ code.

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
- **Elapse** (revision 1.1.2, `Clocked` only): one update passes; the
  silence clocks count it, and the transport ends an end that has heard
  nothing for `SilenceT` updates (`TIMED_OUT`).
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
  window may be resent at any step. `RtoAboveRtt = TRUE` (the liveness cases)
  narrows this to a record due again only once no copy of it is in flight and
  no ack of it is pending or in flight back. **This is an assumption of the
  liveness cases, not a proved property.** No model proves it: in
  `RNResendTiming` a sample may set the resend delay to any value of its
  range, every send may be lost, and nothing there relates the delay to the
  path's round trip, so the code can resend a record whose earlier copy or
  ack is still in flight (rn_window_ack_resend_delay and the walk of
  rn_ordered_write resend on the delay alone). Under the assumption
  `RNEndpoint`'s liveness cases do not explore those resends.
- **The reliable timeout.** Under `TimeoutAnyTime = TRUE` (the safety cases)
  `Tick` may fire at any moment, so safety holds wherever the timeout lands.
  Under `FALSE` (the liveness cases) it fires only once the remote endpoint is in error
  and silent. **This is also an assumption of the liveness cases:** that a
  live remote endpoint's resends land before `reliable_timeout` is not
  proved. `RNResendTiming`'s `AttemptsBeforeTimeout` counts sends (at least
  four before the deadline), not deliveries, and lets every send be lost;
  with loss or a round trip near the timeout the code ends a connection
  whose remote endpoint is live (rn_window_ack_check_timeout), which the
  liveness cases leave out. A composed proof over a bounded network
  (bounded loss and delay, the resend delay above the round trip, the
  attempts landing before the timeout) is a proposed roadmap entry
  (`endpoint-bounded-network-liveness`, in
  `proofs/tla/notes-for-R/receipts-rtt-resend-window.md` until it lands in
  `docs/roadmap.sexp`), not a claim of this model.
- **A configuration** is its `num_channels`, the field the instances vary.
  ConfigCompare compares the whole configuration value, which stands for the
  field-by-field comparison of section 13.4 (version, enabled mask,
  `reliable_messages_in_flight`, `receive_message_buffer_bytes`, `max_message_bytes`,
  `fragment_bytes`): any differing field is the same mismatch.
- **Duplicated packets** are the transport replay window's to drop
  (`ReplayWindow`), so the instances run Link with `Duplication = FALSE`.
  Resends still put several copies of one reliable record in flight in the
  safety cases, and the receiver's dedupe is what keeps them exactly once.
- **Inside one packet** a record outside the receiver's channel count
  refuses the whole payload first, CONFIG included, as the reader's
  whole-payload check does (`rocketnet_message_check_payload`, check 5,
  before `rn_ordered_receive` hands out any record): VIOLATION, and nothing
  of the packet is taken. Otherwise CONFIG is processed first, then data.
  (Revision 1.2; before it the model compared and took CONFIG from a packet
  it then refused.) The violations the code finds record by record, after
  earlier records of the packet were taken (the window's, in `RNWindow`
  `OpenPacket`'s record-by-record fold, and the dispatch's block checks, in
  `RNBlocks` `Receive`'s refusal, `Refused`),
  are not modelled here. The order
  of records inside a packet beyond that is section 9.3's.
- **The CONFIG wait** starts at `Reset` for both ends. The code starts each
  end's at the first update its connection is connected at
  (`rn_endpoint_config_tick`: the server's Observe reset, the client's first
  update its transport reports CONNECTED), which can be later than the
  other end's. A later start only delays that end's CONFIG timeout; `Tick`
  in the safety cases may fire at any update once the wait is due, so the
  code's later end is among the model's behaviours, and the model also has
  the earlier one.
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
| `MCRNEndpoint.cfg` | `Spec`, N = 3, capacity 1, `"arbitrary"` in-order loss both ways, failed sends, ack silence, timeouts at any time, abstract resend delay; constraints `MCOneTimeout` and `MCOneWay`; `MCSafetyView` | pass |
| `MCRNEndpointLive.cfg` | `LiveSpec`, N = 2, capacity 1, `"first"` loss both ways, `RtoAboveRtt` | pass |
| `MCRNEndpointBrokenConfigUnreliable.cfg` | as Live with `ConfigReliable = FALSE`, capacity 2 | temporal `ConfigVerifiedDespiteFirstLoss` |
| `MCRNEndpointBrokenConfigNotCompared.cfg` | as the safety case with `ConfigCompared = FALSE`, capacity 2, no constraints | invariant `MismatchErrorsBothEnds` |
| `MCRNEndpointTwoWay.cfg` | `Spec`, N = 2, capacity 1, both ends sending, the safety terrain; constraint `MCOneTimeout` only; `MCSafetyView` | pass |
| `MCRNEndpointPartial.cfg` | the safety case with `WriteAll = FALSE`; `MCSafetyView` | pass |
| `MCRNEndpointBrokenOrderIgnored.cfg` | the Partial case with `OrderChecked = FALSE` | invariant `OrderedInOrder` |
| `MCRNEndpointTimeoutOff.cfg` | the safety case with the reliable timeout off, `Clocked`, T = 3; every safety invariant, `NoEndBeforeSilenceT`, `SilenceEndsAtT`; `MCSafetyView` | pass |
| `MCRNEndpointBrokenConfigWaitOnTimeout.cfg` | as TimeoutOff with `ConfigWaitOnTimeout` | invariant `NoEndBeforeSilenceT` |
| `MCRNEndpointBrokenSilenceEarly.cfg` | as TimeoutOff with `SilenceEndEarly` | invariant `NoEndBeforeSilenceT` |
| `MCRNEndpointBrokenOptedInInsideT.cfg` | as TimeoutOff with the timeout configured, `ReliableT` = 2 < T = 3 | invariant `EndsNeedSilenceT` |

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
- `MCSafetyView` (the four passing safety cases, revision 1.1.2): TLC
  identifies states that differ only in the links' send counters and
  delivery pulses. This is exact, not a reduction of coverage: under
  `"arbitrary"` loss Link reads `sent` only to compute `sent'` and in
  `LostAtSend(NextCount)`, which is `FALSE` for `"arbitrary"` whatever the
  count, and reads `pulse` only to compute `pulse'` (it exists so fairness
  can tell a Deliver from a Drop; `Spec` has no fairness). No RNEndpoint
  action and no invariant reads either, so two states equal in the view
  have the same enabled steps, the same successors in the view and the same
  verdict on every invariant. Under any other pattern the view is every
  variable. `RNChannel`'s `SafetyView` is the same quotient. It takes the
  two-way case from 5,082,866 distinct states to 255,524, and the partial
  case from 3,326,097 to 170,452, with every constant and invariant as
  before; the counts the bullets above quote (736909 for the one-way case)
  are without it.

### Measured records

Measured for revision 1.1.2 with the pinned runner and JAR on an 8-CPU
Linux bench under a one-minute load of 3 to 5, each case its own
110-second shard on two workers, merged into `proofs/tla/RUNS.tsv` with
`tlacheck merge --keep proofs/tla/RUNS.tsv` (`groups --stale` prints `[]`):

| Case | Gate | Exit | Result | Generated | Distinct | Seconds |
|---|---|---:|---|---:|---:|---:|
| `MCRNEndpoint.cfg` | required | 0 | PASS | 288930 | 36785 | 6.714 |
| `MCRNEndpointLive.cfg` | required | 0 | PASS | 61813 | 24370 | 30.411 |
| `MCRNEndpointBrokenConfigUnreliable.cfg` | required | 13 | PASS (temporal `ConfigVerifiedDespiteFirstLoss`) | 24935 | 11165 | 6.554 |
| `MCRNEndpointBrokenConfigNotCompared.cfg` | required | 12 | PASS (invariant `MismatchErrorsBothEnds`) | 41 | 33 | 0.934 |
| `MCRNEndpointTwoWay.cfg` | required | 0 | PASS | 1904460 | 255524 | 25.259 |
| `MCRNEndpointPartial.cfg` | required | 0 | PASS | 1581460 | 170452 | 20.226 |
| `MCRNEndpointBrokenOrderIgnored.cfg` | required | 12 | PASS (invariant `OrderedInOrder`) | 47823 | 10350 | 2.820 |
| `MCRNEndpointTimeoutOff.cfg` | required | 0 | PASS | 1153656 | 148324 | 17.020 |
| `MCRNEndpointBrokenConfigWaitOnTimeout.cfg` | required | 12 | PASS (invariant `NoEndBeforeSilenceT`) | 6 | 6 | 0.924 |
| `MCRNEndpointBrokenSilenceEarly.cfg` | required | 12 | PASS (invariant `NoEndBeforeSilenceT`) | 67 | 34 | 0.950 |
| `MCRNEndpointBrokenOptedInInsideT.cfg` | required | 12 | PASS (invariant `EndsNeedSilenceT`) | 295 | 127 | 0.986 |

The two-way and partial cases were timeout debt until revision 1.1.2
(exit 124 at 110 s); `MCSafetyView` brings them in with nothing else
changed. The same model without the view, run once on the same bench with
two workers and no budget: the partial case completes with no error,
30828410 generated, 3326097 distinct, in 5 min 21 s (the count the
revision 1.1 run without the view found, so the unclocked model is
unchanged by the clocks); the two-way case's no-budget run is in the
revision 1.1.2 report.

## Reversed witnesses, each checked by hand

The traces were read by hand against `docs/CHANNELS.md` 10.3 and 13.4.
These historical trace explanations were checked against the design before
implementation. The corresponding endpoint and CONFIG functions now live
in `sdk/rocketnet_endpoint.cpp`; the source mapping is in `COVERAGE.tsv`.
The trace explanations remain bounded model evidence, not native execution.

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
| `Elapse` | the transport silence end (`rocketnet_server_protocol_update`, `rocketnet_client_check_deadlines`) | MESSAGES.md m1.9 rule 1; TRANSPORT.md t13.2 |
| `err` | `rn_endpoint_error` | 10.1; 12 (`ROCKETNET_DISCONNECT_REASON_*`) |
| `WriteAck(e)`, `AckSilence(e)` | the transport (not the endpoint) | section 2, P2 and P5 |

## Revision 1.1: the CONFIG wait and the reliable timeout off

Standard rules: MESSAGES.md m9.3 and m11 rule 4. `reliable_message_timeout_ms`
0 (the default) disables the reliable timeout; with it off the CONFIG wait
and an unacked record have no clock, and only the transport's silence timeout
ends a silent connection. The constant `TimeoutConfigured` gates `Tick`; the
seven existing configurations set it `TRUE` and their records are unchanged.
`ConfigWaitOnTimeout` is the reversed witness, the code before revision 1.1.

New invariant `NoEndBeforeT == ~TimeoutConfigured => \A e : err[e] #
"RELIABLE_TIMEOUT"`: with the timeout off neither end ends with the reliable
timeout, whatever the paths lose. (Vacuous, and replaced in revision 1.1.2 by
`NoEndBeforeSilenceT`; see below. The table and trace here are the revision
1.1 records.)

| Case | Gate | Exit | Distinct | Seconds |
|---|---|---:|---:|---:|
| `MCRNEndpointTimeoutOff.cfg` (one-way control, timeout off, every safety invariant and `NoEndBeforeT`) | required | 0 | 285095 | 50.949 |
| `MCRNEndpointBrokenConfigWaitOnTimeout.cfg` | required | 12 (`NoEndBeforeT`) | 6 | 1.651 |

Witness trace, read by hand (6 states): the endpoint is live, the remote
CONFIG has not arrived (`remoteCfg = NoCfg`), and under the default `Tick`
fires (the model's clock is untimed: `Tick` may fire whenever its guard
holds): `err = "RELIABLE_TIMEOUT"`. This is the code before revision 1.1:
`rn_endpoint_config_tick` ended the connection when `now -
config_wait_start >= reliable_timeout` under the default 10 s, so an outage
starting right after connect ended a connection both ends reported
connected. Applied unchanged to the new default of 0, the same comparison is
true at the first update after the wait starts. The code now tests
`reliable_timeout > 0.0` first.

`MCRNEndpoint.cfg` (the one-way control, `TimeoutConfigured = TRUE`) passed at
736,909 distinct states in 99.6 s in one run on the 2026-11-11 bench, but that
run's shard also held a case that exhausted the shard's budget, so the merged
record is the later run's timeout (exit 124). Its record is owed a re-measure
on a quieter bench.

## Revision 1.1.2: the silence clock, and `NoEndBeforeT` given its T

`NoEndBeforeT` (above) could not fail in its passing case: with
`TimeoutConfigured = FALSE` the one action that writes `RELIABLE_TIMEOUT`
(`Tick`) is disabled by that same constant. Checked: in
`MCRNEndpointTimeoutOff.cfg` at revision 1.1.1, the invariant
`\A e : ~ENABLED Tick(e)` holds over all 285,095 distinct states. And the
module had no clock, so the T in its name was not stated anywhere.

Revision 1.1.2 gives the module time (`Clocked`, a constant: the passing
timing case and the timing witnesses set it; the other cases keep the
untimed model, their state counts unchanged). `Elapse` is one update: each
live end's silence clock `quiet[e]` (updates since that end last heard any
packet from the other side; `OnPayload` resets it, as every accepted packet
sets `last_receive_time`) counts it, and the transport ends an end whose
silence reaches `SilenceT` at that update, `TIMED_OUT`
(`rocketnet_server_protocol_update`'s `current_time >= last_receive_time +
timeout_seconds`; the client's `rocketnet_client_check_deadlines`, state
`CONNECTION_TIMED_OUT`). With the reliable timeout configured the two
reliable clocks run on the same updates: the CONFIG wait (`cwait`, from
`Reset`) and each record's age since its first send (`fs`; `WritePacket`
starts it, `PacketFailed` does not, as `rn_window_ack_packet_failed` keeps
a never-sent number never sent), and `Tick` fires when the CONFIG wait or
the age of the record at U, the oldest unacked number, reaches `ReliableT`
(`rn_window_ack_refresh_oldest`). An end in error keeps its clocks.

The properties, as the design states them:

| Property | Statement | Fails when (checked) |
|---|---|---|
| `NoEndBeforeSilenceT` | (S) timeout off: every timing end (`RELIABLE_TIMEOUT`, `TIMED_OUT`) has `quiet >= SilenceT`: an outage shorter than T ends nothing | the CONFIG wait on a zero timeout (`MCRNEndpointBrokenConfigWaitOnTimeout.cfg`); the silence end at T - 1 (`MCRNEndpointBrokenSilenceEarly.cfg`); the window half on a zero timeout (a probe: an unacked record that has been sent ends at the next check, quiet 0); the bound replaced by `FALSE` (fails at T = 3 by the silence end) |
| `SilenceEndsAtT` | (K) no live end has gone `SilenceT` updates unheard: the update that brings the silence to T ends it | the silence end one update late, or never (probes; both fail at quiet = 3 with both ends live) |
| `EndsNeedSilenceT` | the same bound without the gate; with the timeout configured it fails, the reachability witness that the opted-in clock ends a connection inside T | `MCRNEndpointBrokenOptedInInsideT.cfg`: reliable timeout 2 < T = 3, the CONFIG wait ends at quiet 2 |

Witness traces, read by hand against the code:

- `MCRNEndpointBrokenConfigWaitOnTimeout.cfg` (2 states): from `Init`, `Tick`
  at a: `err[a] = RELIABLE_TIMEOUT` with `quiet[a] = 0`. The code before
  v1.1.0, `rn_endpoint_config_tick` (sdk/rocketnet_endpoint.cpp, the
  comparison `now - endpoint->config_wait_start >= reliable_timeout` without
  the `reliable_timeout > 0.0` test, commit 55daea990), with a timeout of 0
  ends the wait at the first update after it starts.
- `MCRNEndpointBrokenSilenceEarly.cfg` (3 states): two `Elapse` steps with
  nothing opened; at quiet 2 both ends are `TIMED_OUT`. The code's
  comparison is `>=` with `timeout_seconds` (server) and `>=` with
  `(double) client->timeout_seconds` (client); one update short is this
  trace.
- `MCRNEndpointBrokenOptedInInsideT.cfg` (4 states): two `Elapse` steps,
  `cwait = 2 = ReliableT`, `Tick` at a with `quiet[a] = 2 < 3`. Opted in,
  `rn_endpoint_config_tick` ends the CONFIG wait at `reliable_timeout`
  whatever was heard: the design's "opted in carries no tolerance
  guarantee".

`MCRNEndpointTimeoutOff.cfg` now runs clocked (T = 3) and lists every
safety invariant with `NoEndBeforeSilenceT` and `SilenceEndsAtT`; the
record is in the table above. A clocked opted-in passing case was tried and
dropped: at N = 2 the per-record first-send ages did not finish in 150 s.
Safety with the timeout configured is `MCRNEndpoint.cfg`, where `Tick`
fires at any moment, a superset of the clocked firing times.
