# RNSentMap: the sent-packet map against the window

`RNSentMap.tla` models the sender's sent-packet map in
`sdk/rocketnet_window_ack.cpp` (`rn_window_ack_commit`,
`rn_window_ack_reclaim_head`, `rn_window_ack_on_ack`), resent from by the walk
of `rn_ordered_write` (`rn_ordered_walk`, `sdk/rocketnet_channel.cpp`),
against the reliable window, and the config rule that sizes it in
`rocketnet_message_validate_config_internal` (`sdk/rocketnet_config.cpp`).

## The question

The map holds `P` entries (`packets_in_flight`), one per packet that carried
reliable numbers. It is a ring in send order: an acked entry is a hole until
it reaches the head, the head is reclaimed past holes, and a write into a ring
spanning `P` entries evicts the oldest entry first. An ack whose packet id the
map no longer holds is ignored (standard/MESSAGES.md m9.1), so evicting an
entry whose ack is still on its way costs a resend of every number it carried,
and a sender that keeps doing it stalls into `RELIABLE_TIMED_OUT`.

What must `P` be, relative to the window `W` (`reliable_messages_in_flight`),
for that never to happen?

## The answer: P >= W (per packet against per number)

Every entry carries at least one reliable number. With a fixed round trip RTT
and a resend delay RTO >= RTT:

1. a number is written at most once in any round trip (its next write waits
   RTO >= RTT, and by then its earlier packet's ack has landed and been
   taken, since each update takes its acks before it writes);
2. every number written in the last round trip is still unacked, and so lies
   in `U .. next - 1`, at most `W` numbers;
3. so at most `W` packets were written in the last round trip.

If the evicted head's ack were still on its way, the head and every later
entry (`P` of them) and the new packet were all written within one round trip:
`P + 1 <= W`. `P >= W` rules it out. It is also needed: `W` first sends of one
number each in one update need `W` entries before any ack can land.

The rule is per packet against per number. `W` counts reliable numbers (a
block takes one per fragment), not messages; packing several records into a
packet only lowers the packet count.

The code had `W = 1024` and `P = 256` by default, which violates it. The unit
case `test_message_config_sent_map_holds_a_window_burst` measured it at that
base: a burst of 1000 single-packet reliable messages with acks held for the
round trip evicted 744 entries whose acks then arrived and were ignored, and
`U` stayed at 0. The fix sets the default `P` to 1024 and refuses a config
with `packets_in_flight < reliable_messages_in_flight` at create
(`ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE`).

## The model

Time is ticks. A written packet is lost, or opened and its ack lands exactly
RTT ticks later. `Write(k, R, lost)` takes `k` first sends from `next` inside
the window and any due resends `R`, 1..`MaxPerPacket` numbers; `Ack(q)` marks
a held entry's numbers acked and leaves a hole, or is ignored; `Tick` advances
the clock once no ack is due; `Done` is the bounded run's end. The ghost
`liveEvicted` records an eviction whose ack was still on its way.

Invariants: `TypeOK`, `MapBounded` (`Len(smap) <= P`), `WindowBounded`
(`next - U <= W`) and `NoLiveEviction`.

Not modelled: blocks (`U'` only narrows the window, so it cannot add packets)
and the 16P reference ring (each packet holds at most 16 references, so it
never binds before the packet ring).

## Receipt-only packets: the rule does not hold with them (open risk)

A packet of unreliable records with receipts and no reliable number takes a
ring entry too (`rn_window_ack_commit_receipt_packet`), and `W` does not bound
those packets, so step 3 above fails once they share the ring.
`WriteReceipt(lost)` writes one, at most `ReceiptsPerUpdate` an update;
`NoLiveEviction` is about entries that carry numbers (an evicted receipt-only
entry resolves its receipts as LOSS, RNReceipts `EvictMap`). The committed run
holds `ReceiptsPerUpdate = 0`; `MCRNSentMapReceiptEviction.cfg` (one a update,
`P = W`) fails `NoLiveEviction`.

Its counterexample, checked against the code by hand: at t = 0 three packets
of one number each (ids 0, 1, 2 carrying 0, 1, 2), all opened, acks due at
t = 2, then a receipt-only packet (id 3) at t = 0. The ring spans `P = 3`
entries, so `rn_window_ack_commit`'s eviction loop drops packet 0 and counts
`MESSAGE_SENT_MAP_EVICTED`; at t = 2 `rn_window_ack_on_ack` does not find id 0,
and number 0 waits for its resend. Measured on the code's window harness
(`rn_ordered_init` on a unit arena, `P = W = 64`, 64 single-number reliable
packets in one 200 ms round trip, all acks landing one round trip later):
with no receipt-only packets 64 of 64 acks are found and nothing is evicted;
with one receipt-only packet 63 are found and one entry is evicted; with 32,
48 are found and 32 evicted.

No point-release repair keeps receipts as they are: a receipt-only packet
that refuses to evict a reliable entry still leaves its entry in the ring for
the next reliable write to pass `P` on; reserving `W` entries for numbered
packets makes every receipt LOSS at the default `P = W = 1024`; and a budget
or ring of their own changes the map's layout and memory. The repair is a
design change for a minor release.

## Cases

| Config | Constants | Expected | Result |
|---|---|---|---|
| `MCRNSentMap.cfg` | W = 3, P = 3, RTT = 2, RTO = 2, numbers 0..4, clock 0..6, two numbers a packet, no receipt-only packets | pass | PASS, 409,730 distinct states, 6.7 s |
| `MCRNSentMapBrokenSmallMap.cfg` | P = 2 | `NoLiveEviction` | exit 12 at depth 4 |
| `MCRNSentMapBrokenEarlyResend.cfg` | RTO = 1 | `NoLiveEviction` | exit 12 |
| `MCRNSentMapReceiptEviction.cfg` | ReceiptsPerUpdate = 1 | `NoLiveEviction` | exit 12 at depth 5 |

The control runs at the tightest sizes the rule allows (`P = W`, `RTO = RTT`).

`MCRNSentMapBrokenSmallMap.cfg`'s counterexample, checked against the code by
hand: at t = 0 the sender writes three packets of one number each (ids 0, 1,
2 carrying 0, 1, 2), all opened, acks due at t = 2. The third write finds the
ring spanning two entries; `rn_window_ack_commit`'s eviction loop drops packet
0, removes it from `sent_index` and counts `MESSAGE_SENT_MAP_EVICTED`; at t = 2
`rn_window_ack_on_ack` does not find id 0 and returns 0, and number 0 stays
unacked until its resend.

`MCRNSentMapBrokenEarlyResend.cfg` shows the rule needs its assumption: with a
resend delay below the round trip, a resend inside the round trip evicts the
first send of the same number before its ack lands. The code's resend delay,
2 x srtt + 4 x deviation + 30 ms (standard/MESSAGES.md m9.2), keeps it above
the measured round trip; a configured `message_resend_delay_max_ms` below the
real round trip would not, and is outside this rule.

The records are in `RUNS.tsv` (group `rnsentmap`), measured with the pinned
runner and JAR on a Linux bench.
