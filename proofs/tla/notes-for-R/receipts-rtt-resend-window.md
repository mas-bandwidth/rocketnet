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

(item "rtt-receipt-only-samples" :group "review-v1-2" :area "sdk"
 :title "Receipt-only packets and ack batches of nothing move the RTT filter"
 :text "rn_window_ack_on_ack (sdk/rocketnet_window_ack.cpp, the ref_count == 0 arm near line 968) samples a packet that carried no reliable number, and the silence test in rn_window_ack_on_acks (near line 1107) needs has_oldest and U < next, so with no reliable number owed no silence ends: a receipt-only packet sent before an outage and acked after it samples the outage into srtt. MESSAGES.md m9.1 reads the sample rule per number. RNRttFilter models the code as it is since M3 (Send's receipt branch, Ack(0) for an ack batch that matches nothing), and MCRNRttFilterReceiptOnlyPoisons.cfg is the recorded expected failure of OutageNeverPoisons. The code-wrong report is codewrong/receipts-rtt-resend-window-MM022.md. Smallest fixes: sample a receipt-only packet only while a reliable number is owed, or end a silence on the age of the oldest receipt-only entry as well. The fix turns MCRNRttFilterReceiptOnlyPoisons.cfg into a passing control and keeps the old rule as its reversed witness."
 :date "2026-11-13" :origin "Models review v12, MM021 and MM022 (M3 receipts-rtt-resend-window)")
```

## docs/TLA-INVENTORY.md

Line 33, `message blocks`, replace the third cell with:

> exact reconstruction and bounded slots; a span padded to the ring's next lap never wraps (`SpanInsideRing`, reached from `ring_next = 5`); a chunk whose count, first number or base disagrees with its anchor is refused and nothing partial is delivered (`MCRNBlocksForged.cfg`; witness `MCRNBlocksBrokenNoAnchorCheck.cfg`)

Line 39, `round-trip filter`, append to the third cell:

> ; the estimate is the code's rounded to the tick; receipt-only packets and ack batches of nothing are modelled, and a receipt-only packet sent before an outage samples it (the recorded expected failure `MCRNRttFilterReceiptOnlyPoisons.cfg`, roadmap `rtt-receipt-only-samples`)

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
  change in the same commit.
- RNBlocks' held-index refusal (an insert past W entries,
  `rn_channels_block_collect` near channel.cpp:891-895) is RNHeldOwnership's
  `Hold`; RNBlocks never fills the index.
- RNResendTiming's `DeadlineUpdateTakesAcks` and RNReceipts' drop arm touch
  the same update order (acks applied before `rn_endpoint_tick`,
  message_server.cpp near 1308 and 1347).

## Comment-only re-citations for package C

Line numbers are at the M3 head (base 4f2960e5 plus the sent-map merge).

- `sdk/rocketnet_window_ack.cpp:840`, above `rn_window_ack_commit_receipt_packet`, add:
  `// proofs/tla/RNRttFilter.tla Send (the receipt branch: a packet with no reliable number, ref_count 0, stamps no first send)`
- `sdk/rocketnet_window_ack.cpp:1106`, extend the comment to:
  `// (proofs/tla/RNRttFilter.tla Ack, EndsSilence; Ack(0), AckOfNothing: an ack batch whose ids match no live entry still runs this test and moves last_ack)`
- `sdk/rocketnet_window_ack.cpp:964-966`, extend the sampling comment with:
  `// a receipt-only packet (ref_count == 0) is sampled too: RNRttFilter.tla AckPacket, MCRNRttFilterReceiptOnlyPoisons.cfg`
- `sdk/rocketnet_endpoint.cpp` at each off-wire LOSS (`rn_endpoint_in_place_finish`, `rn_endpoint_unreliable_drop_all`, `rn_endpoint_in_place_failed`, and the `RN_ENDPOINT_PACKET_UNRELIABLE` arm of `rn_endpoint_packet_failed`), and `sdk/rocketnet_message_server.cpp` at `rn_message_flush_unreliable`, add:
  `// proofs/tla/RNReceipts.tla SweepLoss, the drop arm: LOSS at once for a type-5 message none of whose parts reached the wire (DropOnlyOffWire)`
- `sdk/rocketnet_config.cpp:330-335` (the 1000 ms timeout floor), add:
  `// proofs/tla/RNResendTiming.tla ConfigAccepted (TimeoutFloor)`
- `sdk/rocketnet_message_server.cpp:1308` (on_acked_ids before rn_endpoint_tick), add:
  `// acks before the timeout check: proofs/tla/RNResendTiming.tla AcksOpen, DeadlineUpdateTakesAcks`
- `sdk/rocketnet_window_send.cpp:46` (`rn_window_send_position`), extend:
  `// standard/MESSAGES.md m5.1 boundary padding; proofs/tla/RNRing.tla Pos, PlaceSingle and PlaceBlock; proofs/tla/RNBlocks.tla Pos (SpanInsideRing)`
- `sdk/rocketnet_channel.cpp:913` (the anchor agreement test), add:
  `// proofs/tla/RNBlocks.tla Receive, Refused (MCRNBlocksBrokenNoAnchorCheck.cfg without it)`
- `sdk/rocketnet_window_receive.cpp` above `rn_window_receive_accept`, extend its citation with:
  `// record by record: proofs/tla/RNWindow.tla OpenPacket, AcceptFrom and AcceptOne`
