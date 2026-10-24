# RNServerStats: the running per-slot statistics over a sliding window

`RNServerStats.tla` models the running aggregates in `sdk/rocketnet_ack.cpp`
that keep each slot's published rtt, loss and rates current as packets are
sent, received and acknowledged, against the reference walk
`rocketnet_ack_endpoint_update_stats`.

## The rule

A ring of `N` records holds at record `i` the newest sequence congruent to `i`
mod `N` written there. The reference counts the present records in the older
half of the retained window,

    [base, min(base + H - 1, highest)],  base = max(1, highest - N + 1),  H = N / 2

and the aggregates must equal it after every write and every ack. A write:

1. slides the window to the base its sequence implies, before anything is
   overwritten: each step removes the record leaving at the low edge, moves the
   base, then counts the record entering at the new top edge
   (`base + H - 1`); a jump of `H` or more re-counts the new window, so one
   write does at most `H` records of work even for a sequence the remote end
   chose;
2. retires the record it replaces, when that one is counted (on the received
   side a late or duplicate sequence can replace one anywhere in the window);
3. writes, raises `highest`, and counts the new record if it lands in the
   window.

Removing a bound moves it by scanning the ring for the next present sequence,
as the code does. The sent side writes in increasing sequence order, so its
send times follow its sequences and its spans come from the sequence bounds.
The received side writes any sequence at any time, so its counted records are
kept on a list ordered by receive time (inserted by walking back from the
tail) and its span comes from the list's ends.

## What the model has

- Constants: `N`, `MaxSeq`, `MaxWrites`, `Late` (the received side's writes),
  `SeqTimes` (spans from the sequence bounds rather than the list) and one
  defect switch, `BrokenEntering`.
- Variables: the ring (`seq`, `acked`, `time` per record), `highest`, `base`,
  `writes` (also the clock), `agg` (the counted set, `cnt`, `unacked`, the
  four bounds and the time-ordered list), and `pc`/`pending` for a write's
  micro-steps.
- Actions: `Send(s)` (sent side: above `highest`, gaps and jumps; received
  side: any sequence), `Slide` (one step or one jump), `Write`, `Ack(s)` (sent
  side, any held unacknowledged record of the retained window, any order),
  `Done`.

## Properties

All stated at rest (`pc = "idle"`), where the transports read the numbers.

| Property | Statement |
|---|---|
| `BaseFollowsHighest` | the base is the one `highest` implies |
| `CountedIsWindow` | the counted set, `cnt` and `unacked` are exactly the reference window's |
| `BoundsExact` | the lowest/highest present and acknowledged sequences are the reference's |
| `OrderedByTime` | the list holds the counted records once each, in time order |
| `PublishedSpanExact` | the published span is the reference's earliest-to-latest time |
| `AckedSpanExact` | the acknowledged span is the reference's (sent side) |

## Cases

| Config | Instance | Expected |
|---|---|---|
| `MCRNServerStats.cfg` | sent side, `N = 6`, sequences to 14, 8 writes | pass (730,490 distinct states) |
| `MCRNServerStatsReceived.cfg` | received side, `N = 6`, sequences to 9, 7 writes | pass (2,061,957 distinct states) |
| `MCRNServerStatsBrokenEntering.cfg` | sent side, `BrokenEntering = TRUE` | `CountedIsWindow` violated |
| `MCRNServerStatsBrokenSeqTimes.cfg` | received side, `SeqTimes = TRUE` | `PublishedSpanExact` violated |

## The reversed witnesses, replayed by hand

`BrokenEntering` is the defect the first incremental version shipped: the
entering record was inserted at `base + H` while the base had not moved, and
the insert's own window test (`seq <= base + H - 1`) refused it, so every slide
removed a record and added none. TLC's trace: write 4 (window `[1, 3]`, 4 not
yet in it), then write 7: the target base is 2, the step removes nothing at 1
(absent), and the record 4 entering at the top edge is refused; after the write
the window `[2, 4]` holds 4 and the counted set is empty. In the code at
`adbc1f1` this is `rocketnet_ack_stats_sent_sync` calling
`rocketnet_ack_stats_sent_insert` with `entering = sent_base + HALF` before
`sent_base++`; with 4096 records it drains the published numbers to zero after
4096 sends.

`BrokenSeqTimes` reads the received span from the lowest and highest counted
sequences. TLC's trace: receive 2 (time 1), then 1 late (time 2). The window
holds 1 and 2, the earliest time is 1 (at sequence 2), but the sequence bounds
give time 2 at sequence 1: the span is negative and the published received rate
reads 0 where the reference has a rate. The code reads it from the list's head
and tail instead.

## Running it

From the checkout root, with the pinned `tla2tools.jar` and `tlacheck` (see
`tla/README.md`):

```sh
"$job/tlacheck" run --root . --jar "$jar" --java "$java" \
  --group rnserverstats --workers 2 --timeout 110s --dir "$job/rnserverstats"
```
