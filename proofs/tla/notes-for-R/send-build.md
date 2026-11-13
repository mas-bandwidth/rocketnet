# Notes for package R from package M5a (send-build)

Branch `models/v1.1.x/send-build`. Everything below belongs to R (records,
roadmap, inventory) or to C (comment-only re-citations in `sdk/`); M5a did
not edit those files.

## MM055: two COVERAGE rows for RNOutgoingBuild

`proofs/tla/COVERAGE.tsv` holds two current rows for one model: `outgoing-build`
(line 26) and `rn-outgoing-build` (line 81). Both are on `sdk/rocketnet_endpoint.cpp`
and describe the same build. M5a brought both up to date with the model on this
branch (the buffered spent frames, `WriteNoFrame`, `WriteFailed` and the
`Excused` deadline; `outgoing-build` closes with "environment actions
WriteNoFrame and WriteFailed."). R merges them into one row, keeping
`outgoing-build`'s id (it carries the environment-actions note that
`check_tla_coverage --actions-strict` reads) and the union of the two texts'
function lists.

## Roadmap entries (sexp text, for docs/roadmap.sexp)

```
  (item "send-budget-in-place-reserve-model" :group "models-v1-1" :area "rocketnet"
   :title "Model the fill phase's in-place admit against the send budget's reserve"
   :text "RNSendBudget's ClassOrder holds within the write pass only. An in-place unreliable send of the fill phase (rn_message_object_start, rn_endpoint_in_place_affordable in sdk/rocketnet_endpoint.cpp) is admitted against the same bucket before the pass, and rocketnet_send_budget_begin_update then resets rank_reached; what keeps it from taking the bytes due reliable work needs is the one-maximum-packet reserve it keeps while ordered work is due. Add an InPlace action to RNSendBudget before the send phase, with the reserve, a property that a due reliable item still fits after the in-place admits of the update, and a reversed witness with the reserve removed."
   :date "2026-11-13" :origin "Models review merged table row MM064")

  (item "ordered-due-ended-connection" :group "models-v1-1" :area "rocketnet"
   :title "Answer not due for an ended connection's ordered work"
   :text "rn_endpoint_ordered_due (sdk/rocketnet_endpoint.cpp) does not ask whether the connection has ended (rn_region_exhausted, a channel error, a reliable message past its timeout), while rn_ordered_write returns 0 for one. A connection that ends between the update's rn_endpoint_error check and send_packets plans an ordered packet, starts a frame and aborts it (message_server.cpp about 2157-2163); on the ring transport the slot is not returned until the transport sends. It costs one frame of the flush's capacity per such connection until the next update kicks it. RNSendRotation models it (EnvVain, NoRefusalAbort, AbortsBounded). Asking the same three conditions in the due test removes the abort; the model's vain flag then goes away with a witness that restores it."
   :date "2026-11-13" :origin "Models review merged table row MM056")
```

## docs/TLA-INVENTORY.md

- Lines 16-17: replace "its six cases were run on 2026-10-24" with "its eight
  cases were run on 2026-11-13".
- Line 40 (server send loop and budgets), new text of the third column:

  > a frame is started only for a payload that will be written: the due test
  > agrees with the walk (a slot in probe hold starts a frame only for its
  > probe) and a budget refusal is decided before a frame, so no unit of the
  > shared capacity goes to an aborted frame for a refusal (`NoUnitWasted`,
  > `NoRefusalAbort`, `OtherClientUnaffected`, `DarkTakesOnlyItsProbes`); a
  > connection that ended before the pass aborts at most one frame a flush
  > (`AbortsBounded`); every slot with writable work served within
  > ceil(connected / K) flushes that had the full capacity, the capacity read
  > live at each visit (`FairService`); a flush is rounds over the last round's
  > writers in the first round's order, the cursor as a walk over every
  > connected slot would leave it, at most N + frames visits (`SameOrder`,
  > `OnlyLastWriters`, `EndedWritesNothing`, `CursorAsFullWalk`,
  > `VisitsBounded`); the reliable message an update owes a packet is with
  > the transport when the flush ends, unless no frame or a failed finish cut
  > the pass (`ReliableSentWithinOneUpdate`); the bytes sent in a window stay within
  > the refill at the rates in force plus one bucket across runtime rate
  > changes (`NeverOverspend`)

## proofs/tla/README.md

- Line 167, `rnoutgoingbuild`: "The outgoing build (docs/OUTGOING-BUILD.md), on
  the ring and staged transports (README-RNOutgoingBuild.md)."
- Line 171, `rnserversendfair`: "The server send pass under the transport's
  live send capacity (README-RNSendRotation.md, revisions 1.1 and 1.2)."

## Comment re-citations for package C (comment-only, sdk/)

1. `sdk/rocketnet_message_server.cpp:1459` (MM059). Now: "proofs/tla/RNWriterFinish.tla
   StartSmall, StartLarge and StartReliable: in place when the update has
   capacity, otherwise marked as having none." New text: "proofs/tla/RNWriterFinish.tla
   StartSmall, StartLarge and StartReliable: in place when the update has
   capacity; held behind an unreliable message already queued on the client,
   queued for send_packets; with no capacity, queued on a buffered
   transport's ring or marked as having none."
2. `sdk/rocketnet_message_server.cpp:2159-2160` (MM056). Now: "only an ended
   connection gets here: exhausted, a channel error or a reliable message past
   its timeout, once". New text: "only an ended connection gets here:
   exhausted, a channel error or a reliable message past its timeout; the
   reliable path ends, so once a flush, until the update kicks it
   (proofs/tla/RNSendRotation.tla vain, AbortsBounded)".
3. `sdk/rocketnet_endpoint.cpp:1139` (MM056). Now: "a long acked prefix: let the
   write walk it". New text: "not reached: an ack unlinks its number from the
   resend FIFO before marking it acked, so the walk meets no acked number
   (rocketnet_channel.cpp, the write walk's assert); kept as a bound".
4. `sdk/rocketnet_message_server.cpp:2127-2134` (MM052), add above the
   `if ( !frame )` branch: "proofs/tla/RNOutgoingBuild.tla WriteNoFrame: no
   frame cuts the pass (the reliable phase, or the whole pass in the
   unreliable phase)."
5. `sdk/rocketnet_endpoint.cpp:1798` (MM052), add above `rn_endpoint_packet_failed`:
   "proofs/tla/RNOutgoingBuild.tla WriteFailed: a finish that sent nothing; an
   ordered packet is due again and cuts the reliable phase, unreliable records
   are dropped and counted, and the allowance is spent either way."
6. `sdk/rocketnet_server_transport.cpp:1554` (MM057), add above
   `rocketnet_server_transport_send_capacity_available`: "proofs/tla/RNServerSendFair.tla
   room and Grow: the capacity the send pass reads live at each visit."
7. MM059's citation at `message_server.cpp:2130` named in the table is now at
   `message_server.cpp:2206` on this base and was already corrected by
   writer-finish e6f6ec241 ("RNWriterFinish.tla SendPackets: the main thread's
   walk finds no packet a writer left open, and takes what the writers queued
   or held as far as the capacity carries."): nothing to do.
