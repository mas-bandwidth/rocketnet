# RNServerStats: the running per-slot statistics over a sliding window

`RNServerStats.tla` models the running aggregates in `sdk/rocketnet_ack.cpp`
that keep each slot's published rtt, loss and rates current as packets are
sent, received and acknowledged, against the reference walk
`rocketnet_ack_endpoint_update_stats`.

## The rule

A ring of `N` records holds at record `i` the newest sequence congruent to `i`
mod `N` written there. The reference counts the present records in the older
half of the retained window,

    [base, top],  base = max(1, highest - N + 1),  top = min(highest, base + H - 1),  H = N / 2

and, for loss, the part of it at or below `highest - H` (the newest half of the
ring is still in flight). The aggregates must equal the reference after every
write and every ack. Every edge is a function of `highest`, so the code stores
none. On the sent side, a write that raises `highest` from `h0` to `h1`:

1. moves the window in one step, before anything is overwritten
   (`rocketnet_ack_stats_sent_advance`):
   the counted records below the new base leave (all at once, in O(1), when
   the new base is past the old top, so a large sequence gap costs nothing to
   traverse); the counted survivors crossing into the loss-eligible
   part are counted there; the present records between the old top and the
   new one, and no higher than `h0` (nothing above it is written yet), enter;
2. retires the record it replaces, when that one is counted;
3. writes, and counts the new record if it lands in the window.

The received side instead keeps 64 fixed physical-slot summaries for the
2,048-record ring. `rocketnet_ack_stats_received_advance` clears summaries for
whole physical blocks that leave the retained full window and rebuilds at most
two partial edge blocks. A write then replaces the exact full-width sequence
tag, rebuilds that one 32-slot block, and
`rocketnet_ack_stats_received_query` reduces the older-half range from whole
summaries plus at most two query edges. Exact tags keep a stale physical alias
out of every rebuild and query; receive-time order is never inferred from
sequence order.

The received side refuses a sequence below the window base (at least one ring
behind `highest`) before any of this, and leaves the ring and the aggregates
unchanged. Replay admits sequences further back than the ring (its window is
wider), and such a sequence's slot holds a newer sequence of `[base, highest]`:
writing it would erase a newer, possibly counted, record that no recompute
can restore. Inside the window a slot's resident is the same sequence (a
duplicate) or one at least a ring older (below the base, never counted), so
the replace path stays. The sent side already refuses a stale write the same
way (`rocketnet_ack_endpoint_record_sent_classified`).

Each sent sequence enters and leaves once, so sent-side work is O(1) per
sequence amortized and at most half a ring per call. A removed low bound moves
to the next counted sequence. Sent times follow sequence (update refuses a
time that goes backwards, and concurrent fill writers of one phase share its
time), so sent spans come from the sequence bounds. Received writes may arrive
in any sequence or time order, so each block summary holds count, bytes, and
receive-time extrema. The immutable 2,048-slot layout has a fixed worst case
of 224 block/record visits per receive: at most 129 for retention, 32 for the
point-block rebuild and 63 for the 1,024-slot query. Its 2,048 summary bytes
reuse the former four link bytes in each received record, so endpoint memory
does not grow.

## What the model has

- Constants: `N`, `MaxSeq`, `MaxWrites`, `Late` (the received side's writes),
  `Reorder` (sent writes below `highest`, as fill writers record out of
  order), `SeqTimes` (received spans from sequence bounds rather than the
  exact-tagged range reduction) and
  three defect switches, `BrokenEntering`, `BrokenCrossing` and `BrokenAlias`
  (the received write without its floor below the window base).
- Variables: the ring (`seq`, `acked`, `time` per record), `highest`, `base`,
  `writes` (also the clock), `agg` (the counted set, `cnt`, `unacked`, the
  eligible `ecnt` and `eunacked`, the four bounds and the exact-tagged
  `leaves` reduced for the received range),
  `pc`/`pending` for a write's two micro-steps, and the history `written`
  (every sequence handed to `Write`).
- Actions: `Send(s)`, `Slide` (the one-step window move, raising `highest`),
  `Write`, `Ack(s)` (sent side, any held unacknowledged record of the retained
  window, any order), `Done`.

## Properties

The invariants are stated at rest (`pc = "idle"`), where the transports read
the numbers; `LateNeverShrinks` is an action property over every step.

| Property | Statement |
|---|---|
| `BaseFollowsHighest` | the base is the one `highest` implies |
| `CountedIsWindow` | the counted set, `cnt` and `unacked` are exactly the reference window's |
| `EligibleExact` | `ecnt` and `eunacked` are exactly the reference's loss-eligible sends |
| `BoundsExact` | the lowest/highest present and acknowledged sequences are the reference's |
| `RangeAggregateExact` | exact-tagged reduced leaves are precisely the reference window |
| `OrderedByTime` | historical checked name; aliases `RangeAggregateExact` |
| `PublishedSpanExact` | the published span is the reference's earliest-to-latest time |
| `AckedSpanExact` | the acknowledged span is the reference's (sent side) |
| `RetainedIsNewest` | every sequence written at or above the base is still in the ring |
| `LateNeverShrinks` | a write at or below `highest` never lowers the counted total (action property) |

## Cases

| Config | Instance | Expected |
|---|---|---|
| `MCRNServerStats.cfg` | sent side, `N = 6`, sequences to 14, 8 writes | pass (934,209 distinct states) |
| `MCRNServerStatsReordered.cfg` | sent side with writes below `highest`, sequences to 10, 6 writes; counts and bounds | pass (3,440,833 distinct states) |
| `MCRNServerStatsReceived.cfg` | received side, `N = 6`, sequences to 9, 7 writes | pass (3,406,318 distinct states) |
| `MCRNServerStatsAlias.cfg` | received side, `N = 4`, sequences to 9, 4 writes: late writes more than a ring behind | pass (18,090 distinct states) |
| `MCRNServerStatsBrokenEntering.cfg` | sent side, `BrokenEntering = TRUE` | `CountedIsWindow` violated |
| `MCRNServerStatsBrokenCrossing.cfg` | sent side, `BrokenCrossing = TRUE` | `EligibleExact` violated |
| `MCRNServerStatsBrokenSeqTimes.cfg` | received side, `SeqTimes = TRUE` | `PublishedSpanExact` violated |
| `MCRNServerStatsBrokenAlias.cfg` | received side, `N = 4`, `BrokenAlias = TRUE` | `RetainedIsNewest` violated |

`MCRNServerStatsReceived.cfg` cannot reach the alias: a counted record `r`
with a slot-mate `r - N >= 1` needs `r >= 7`, so `highest >= 10 > MaxSeq`.
The ring of 4 in `MCRNServerStatsAlias.cfg` reaches it in two writes.

The reordered instance leaves the spans to the in-order one: the model's clock
is the write count, so two out-of-order sends get different times, where the
code gives one fill phase one time.

## The reversed witnesses, replayed by hand

`BrokenEntering` is the defect the first incremental version shipped (head
`adbc1f1`): the record entering the window's top edge was never counted, so
every slide removed a record and added none, and after one ring of sends the
published numbers drained to zero. TLC's trace: write 4 (window `[1, 3]`, 4
not in it), then write 7: the window becomes `[2, 4]` and 4, present, must
enter (`(old top, min(new top, old highest)] = {4}`); skipped, the counted set
is empty where the reference holds 4. In the code this is the entering loop at
the end of `rocketnet_ack_stats_sent_advance`; removing it fails
`test_server_stats_current_after_window_slides` at its first slide.

`BrokenCrossing` forgets the survivors crossing into the loss-eligible part.
TLC's trace: write 1 (window `[1, 1]`, nothing eligible while `highest <= H`),
then write 4: the eligible part becomes `[1, 1]` and the counted record 1 must
be counted there; skipped, `ecnt` is 0 where the reference has 1, so loss is
taken over too few sends while the ring fills. In the code this is the crossing
loop in `rocketnet_ack_stats_sent_advance`; removing it fails the same unit
case.

`BrokenSeqTimes` reads the received span from the lowest and highest counted
sequences. TLC's trace: receive 2 (time 1), then 1 late (time 2). The window
holds 1 and 2, the earliest time is 1 (at sequence 2), but the sequence bounds
give time 2 at sequence 1: the span is negative and the published received rate
reads 0 where the reference has a rate. The code instead reduces receive-time
extrema from exact-tagged block summaries and partial edge records.

`BrokenAlias` is the received write without its floor. TLC's trace: receive 5
(window `[2, 3]`, the retained range `[2, 5]`), then 1, admitted by replay
whose window is wider than the ring: 1 is below the base and shares record 1
with 5, and the unconditional write replaces 5, so a sequence of the retained
range is gone from the ring and no recompute can bring it back. The same write
erases a counted record when the window already holds it: receive 5, then 8
(window `[5, 6]`, 5 counted), then 1, and the counted total drops. In the code,
with the ring of 2048: receive 3000, 3001 and 5000 (window `[2953, 3976]`),
then 952, which replay admits 4048 behind 5000 and which shares 3000's slot;
before the floor the received count fell from 2 to 1 and the bytes from 700
to 400. `rocketnet_ack_endpoint_record_received` now returns before touching
the ring when the sequence is at least one ring behind the high-water mark;
`test_ack_received_late_alias_keeps_counted_newer` pins it.

## Running it

From the checkout root, with the pinned `tla2tools.jar` and `tlacheck` (see
`proofs/tla/README.md`), one shard per case:

```sh
for i in $(seq 0 5); do
  "$job/tlacheck" run --root . --jar "$jar" --java "$java" \
    --group rnserverstats --shards 6 --shard "$i" --workers 2 --timeout 110s \
    --dir "$job/rnserverstats-$i"
done
"$job/tlacheck" merge --root . --keep proofs/tla/RUNS.tsv \
  --out "$job/RUNS.tsv" "$job"/rnserverstats-*/RUNS.tsv
```
