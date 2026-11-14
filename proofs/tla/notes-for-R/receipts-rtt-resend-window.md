# Notes for package R and package C from M3 (receipts-rtt-resend-window)

Package M3 corrected RNReceipts, RNRttFilter, RNResendTiming,
RNResendGenerations, RNEndpoint, RNWindow and RNBlocks (rows MM019 to MM034,
the MM028 model half and the MM218 README-lag instance for RNReceipts). What
belongs to package R (roadmap, the inventory, the proofs README) and to
package C (comment-only re-citations in `sdk/`) is below. Nothing here was
applied by M3.

## Roadmap entries (for docs/roadmap.sexp, group "review-v1-2")

```
(item "endpoint-bounded-network-liveness" :group "review-v1-2" :area "proofs"
 :title "Prove RNEndpoint's two liveness premises against a bounded network instead of assuming them"
 :text "RNEndpoint's liveness cases (MCRNEndpointLive.cfg, ConfigVerifiedDespiteFirstLoss) rest on two premises no model proves: RtoAboveRtt (a record is due again only once no copy of it is in flight and no ack of it is pending or in flight back) and the Tick guard (the reliable timeout fires only once the remote end is in error and silent, so a live end's resends land before it). The code resends on the resend delay alone: RNResendTiming lets a sample set the delay anywhere in [RtoMin, RtoMax] and relates it to no round trip, AttemptsBeforeTimeout counts sends before the deadline and not deliveries, and every send may be lost. Both premises are labelled ASSUMPTIONS in RNEndpoint.tla and README-RNEndpoint.md since M3 (review v1.2, MM024). The proof composes a network with a bounded delay and a bounded loss run with RNResendTiming's schedule and RNRttFilter's estimate: the resend delay exceeds the round trip once a sample has been taken, and some send of each record lands before reliable_message_timeout_ms; then RNEndpoint's liveness follows with RtoAboveRtt and the Tick guard replaced by those facts. Until then the liveness cases suppress real resends and timeouts on the premise."
 :date "2026-11-13" :origin "Models review v12, MM024 (M3 receipts-rtt-resend-window)")
```

MM022 is not a defect: standard m9.1 requires one sample per acknowledged
send, receipt-only sends included (9905e855), so no roadmap entry for it
comes from M3 (the receipt-only sampling entry is Rowan's).

## docs/TLA-INVENTORY.md

Line 33, `message blocks`, replace the third cell with:

> exact reconstruction and bounded slots; a span padded to the ring's next lap never wraps (`SpanInsideRing`, reached from `ring_next = 5`); a chunk whose count, first number or base disagrees with its anchor is refused and nothing partial is delivered (`MCRNBlocksForged.cfg`; witness `MCRNBlocksBrokenNoAnchorCheck.cfg`)

Line 39, `round-trip filter`, append to the third cell:

> ; the estimate is the code's rounded to the tick; receipt-only packets and ack batches of nothing are modelled; a receipt-only packet sent before an outage is sampled, as m9.1 requires, and no sample taken from a packet that carried a number is outage-sized and above the estimate (`NumberSamplesNeverPoison`, `MCRNRttFilterReceiptOnlyPoisons.cfg`; witness `MCRNRttFilterBrokenNoFilterReceiptOnly.cfg`)

Line 34 (or wherever the `message window` row sits), append to the third cell:

> ; the liveness assumption `AcksLand` is shown to carry `EveryReliableAckedAndDelivered` (witness `MCRNWindowBrokenNoAcksLand.cfg`)

Line 38, `resend FIFO`, append to the third cell:

> ; the generation wraps to 1 (`MCRNResendGenerationsWrap.cfg`) and the number comparison alone keeps a stale ack off the later number

## proofs/tla/README.md

Line 183, `rnresendgenerations`, replace the second cell with:

> The names a transport ack follows into the resend FIFO, at the slot layout with W = 4, six numbers and two descriptors, the links carrying no generation, with the generation wrapping at GenMax = 2 (README-RNResendGenerations.md).

Line 158, `rnwindow`, replace the second cell with:

> Abstract reliable sender/receiver window, a packet's records accepted one at a time (README-RNWindow.md).

## Cross-family rows

- RNWindow's `AcksLand` is an assumption the code does not keep under
  receipt-only traffic; RNSentMap's `MCRNSentMapReceiptEviction.cfg` shows
  it, and the code half is the existing roadmap entry
  `sent-map-receipts-give-way` (MM028). When that entry lands, RNWindow's
  `AcksLand` comment and README text ("ASSUMPTION the code does not keep")
  change in the same commit. Its reversed witness is now
  `MCRNWindowBrokenNoAcksLand.cfg` (MA006); the checker half of MA006
  (`tools/check_tla_records.py` following named operators in `Fairness`)
  is M14's.
- RNBlocks' held-index refusal (an insert past W entries,
  sdk/rocketnet_channel.cpp (rn_channels_block_collect), the first
  rn_held_insert) is RNHeldOwnership's `Hold`; RNBlocks never fills the
  index.
- RNResendTiming's `DeadlineUpdateTakesAcks` and RNReceipts' drop arm touch
  the same update order (acks applied before `rn_endpoint_tick`, in
  sdk/rocketnet_message_server.cpp (rn_message_object_update_end)).

## Comment-only re-citations for package C

Each place is named by file and function (this folder's checker refuses
line numbers); the line in brackets is where it sits at the M3 head, base
4f2960e5 plus the sent-map merge.

- sdk/rocketnet_window_ack.cpp (rn_window_ack_commit_receipt_packet) [line 840], above the function, add:
  `// proofs/tla/RNRttFilter.tla Send (the receipt branch: a packet with no reliable number, ref_count 0, stamps no first send)`
- sdk/rocketnet_window_ack.cpp (rn_window_ack_on_acks) [line 1106], extend the silence-test comment to:
  `// (proofs/tla/RNRttFilter.tla Ack, EndsSilence; Ack(0), AckOfNothing: an ack batch whose ids match no live entry still runs this test and moves last_ack)`
- sdk/rocketnet_window_ack.cpp (rn_window_ack_on_ack) [lines 964 to 966], extend the sampling comment with:
  `// a receipt-only packet (ref_count == 0) is sampled too, as standard m9.1 requires: RNRttFilter.tla AckPacket, NumberSamplesNeverPoison`
- sdk/rocketnet_endpoint.cpp at each off-wire LOSS (rn_endpoint_in_place_finish, rn_endpoint_unreliable_drop_all, rn_endpoint_in_place_failed, and the RN_ENDPOINT_PACKET_UNRELIABLE arm of rn_endpoint_packet_failed), and sdk/rocketnet_message_server.cpp (rn_message_flush_unreliable), add:
  `// proofs/tla/RNReceipts.tla SweepLoss, the drop arm: LOSS at once for a type-5 message none of whose parts reached the wire (DropOnlyOffWire)`
- sdk/rocketnet_config.cpp (rocketnet_message_validate_config_internal) [lines 330 to 335], at the 1000 ms timeout floor, add:
  `// proofs/tla/RNResendTiming.tla ConfigAccepted (TimeoutFloor)`
- sdk/rocketnet_message_server.cpp (rn_message_object_update_end) [line 1308], at the rn_endpoint_on_acked_ids call that comes before rn_endpoint_tick, add:
  `// acks before the timeout check: proofs/tla/RNResendTiming.tla AcksOpen, DeadlineUpdateTakesAcks`
- sdk/rocketnet_window_send.cpp (rn_window_send_position) [line 46], extend its comment to:
  `// standard/MESSAGES.md m5.1 boundary padding; proofs/tla/RNRing.tla Pos, PlaceSingle and PlaceBlock; proofs/tla/RNBlocks.tla Pos (SpanInsideRing)`
- sdk/rocketnet_channel.cpp (rn_channels_dispatch_chunk) [line 966], in the comment above the function, replace "RNBlocks!Receive (the anchor by b = n - index, the copy to ring + index * F)" with:
  `RNBlocks!Receive (the anchor by (channel, ordering), the refusal of a chunk that disagrees with it, the copy to ring + index * F)`
- sdk/rocketnet_channel.cpp (rn_channels_block_collect) [line 913], at the anchor agreement test, add:
  `// proofs/tla/RNBlocks.tla Receive, Refused (MCRNBlocksBrokenNoAnchorCheck.cfg without it)`
- sdk/rocketnet_window_receive.cpp (rn_window_receive_accept), extend its citation with:
  `// record by record: proofs/tla/RNWindow.tla OpenPacket, AcceptFrom and AcceptOne`
