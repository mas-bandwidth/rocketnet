# RNSentHistory: the sent history under loss, reordering and ack delay

> STANDARD.md citations in this file are to the retired draft, kept at commit 96da6fb36107.

`RNSentHistory.tla` is the sent-record slice of `docs/TLA-INVENTORY.md` M8: a
ring of `N` records, each holding the newest sent session sequence congruent to
its index mod `N`, and what a write does to the record it replaces.

## The rule

Writing sequence `s` evicts the record of `s - N`. An empty or acknowledged
record is a clean write. An unacknowledged record is an eviction, and it is
told apart by what the sender knows:

| Evicted record | Count | Meaning |
|---|---|---|
| unacked, and `largest_acked_sequence >= seq + Threshold` | `SEND_HISTORY_LOST` | known path loss (RFC 9002 6.1.1, packet threshold 3), an outcome |
| unacked, no such later ack | `SEND_HISTORY_OVERFLOW` | unknown fate: a silent receiver, or a window too small for the path; a refusal of history that reads zero on a healthy path |

The write never refuses the send (STANDARD.md "Replay window sizing": an
implementation MUST NOT throttle sends to preserve history). The sum of the two
counts is the count of overwritten unacknowledged records the standard names.

## What the model has

- Constants: `N`, `Threshold`, `MaxSeq`, `MaxLoss`, `Flight`, `AckRange`,
  `AckSlots` and two defect switches, `CountEveryUnacked` and `RefuseWhenFull`.
- Variables: the ring (`seq`, `acked` per record), `largestAcked`, the packets
  in flight, the receiver's received set, the ack blocks in flight, the sequences
  the path lost, and ghost sets for what was acknowledged, evicted unacked,
  counted lost and counted overflow (the counters are their cardinalities).
- Actions: `Send` (always enabled below `MaxSeq`; evicts and classifies),
  `Deliver` (any in-flight packet, any order), `Lose` (at most `MaxLoss`, never
  `Threshold + 1` consecutive sequences), `ReceiverAck` (a block restating the
  receiver's received set inside its window), `ReceiveAck` (any block, any order,
  any delay; marks the records still held and unacked, folds `largestAcked`
  over exactly those; a block naming an evicted sequence is ignored), `Done`.
- `healthy` is a history variable: it stays true while every send was at most
  `Flight` ahead of `largestAcked`. `N - Flight >= Threshold` is assumed (4096
  against a bandwidth-delay product of tens to a couple of hundred packets).

## Properties

| Property | Statement |
|---|---|
| `EveryEvictionClassified` | an unacked eviction counts exactly one of lost and overflow; nothing else is counted |
| `LostImpliesLaterAcked` | a sequence counted lost has a sequence at least `Threshold` above it that the receiver received and the sender marked acknowledged |
| `LargestAckedIsMaximum` | `largestAcked` is the maximum sequence ever marked acknowledged |
| `NoOverflowUnderBoundedLoss` | on a healthy path with bounded, isolated loss, overflow stays zero |
| `SendNeverRefused` | whenever a sequence is left, `Send` is enabled |

## Running it

From the checkout root, with `tla2tools.jar` (see `proofs/tla/README.md` for the pinned
JAR and the pinned `tlacheck`):

```sh
cd tla
java -cp tla2tools.jar tlc2.TLC -workers 4 -config MCRNSentHistory.cfg MCRNSentHistory.tla
java -cp tla2tools.jar tlc2.TLC -workers 4 -config MCRNSentHistoryBrokenCountEveryUnacked.cfg MCRNSentHistory.tla
java -cp tla2tools.jar tlc2.TLC -workers 4 -config MCRNSentHistoryBrokenRefuseWhenFull.cfg MCRNSentHistory.tla
```

Recorded runs are `tlacheck run --group rnsenthistory` rows in `RUNS.tsv`.
`MCRNSentHistory.cfg` is `N = 4`, `Threshold = 2`, `MaxSeq = 7`, `MaxLoss = 2`,
`Flight = 2`, `AckRange = 4`, `AckSlots = 2`: the ring laps once. It passes
every property: 3,579,879 states generated, 695,433 distinct, depth 27, no
error.

## Reversed witnesses

Each is the committed configuration with one defect switch flipped. TLC must
fail each on its named property; a witness that passes is a model defect.

**`MCRNSentHistoryBrokenCountEveryUnacked.cfg`** (`CountEveryUnacked = TRUE`:
the rule before the split, every unacked eviction is overflow) fails
`NoOverflowUnderBoundedLoss`, exit 12, in twelve states:

1. `Send 1`, `Send 2`: both in flight.
2. `Deliver 2`, `ReceiverAck {2}`, `ReceiveAck`: `largestAcked = 2`.
3. `Send 3`, `Send 4`: the ring is full, record 0 holds 4.
4. `Deliver 3`, `ReceiverAck {2, 3}`, `ReceiveAck`: `largestAcked = 3`.
5. `Send 5` evicts the record of 1. Sequence 1 is still in flight (or lost), the
   receiver has acknowledged 3 >= 1 + 2, and `healthy` is still true (every send was at
   most 2 ahead). The eviction is counted `overflowSet = {1}`.

This is the w3 finding: a packet the path lost (or delayed past the threshold)
is never acknowledged, so one lap later the base rule counts it as overflow
while the receiver is acknowledging everything else. By hand against the code: at
the base, `rocketnet_ack_endpoint_record_sent` returns `!record->sequence || record->acked` for the replaced record, false for every unacked eviction, and
all eight callers count that false as `SEND_HISTORY_OVERFLOW`. The fixed model
counts sequence 1 lost (`3 >= 1 + 2`) and `overflowSet` stays empty.

**`MCRNSentHistoryBrokenRefuseWhenFull.cfg`** (`RefuseWhenFull = TRUE`: `Send`
disabled while every record is unacked) fails `SendNeverRefused`, exit 12, in
five states: `Send 1` to `Send 4` fill the ring with unacknowledged records, and
at `next = 5 <= MaxSeq` `Send` is not enabled. The code never refuses: all eight
callers count the result and still transmit.

## Mutation controls run by hand

Not committed (the switches are the two witnesses above); each was run once on a
scratch copy and failed on the named property, so each property is not vacuous:

- the threshold dropped (`largestAcked >= old.seq`): `LostImpliesLaterAcked`;
- `largestAcked` folded from a whole block rather than the newly marked records:
  `LargestAckedIsMaximum`;
- an unknown-fate eviction never counted: `EveryEvictionClassified`;
- a healthy behaviour that counts a sequence lost exists (`healthy /\ lostSet # {}` reached), so the fixed run exercises the lost class.

## Code sites

| Rule | Site |
|---|---|
| the classification | `rocketnet_ack_endpoint_record_sent_classified`, `sdk/rocketnet_ack.cpp` |
| the threshold | `ROCKETNET_LOSS_PACKET_THRESHOLD`, `sdk/rocketnet_constants.h` |
| `largestAcked` fold | `rocketnet_ack_apply_run`, `sdk/rocketnet_ack.cpp` |
| the counts | `rocketnet_ack_count_sent_result`; `SEND_HISTORY_LOST` and `SEND_HISTORY_OVERFLOW`, `sdk/rocketnet_counters.h` |
| no refusal | the eight callers in `sdk/rocketnet_server_protocol.cpp` and `sdk/rocketnet_client_protocol.cpp` count and still transmit |
| unit tests | `rocketnet_test_send_history_lossy_long_path`, `..._silent_receiver_overflows`, `..._late_ack_before_eviction` in `sdk/rocketnet_tests.cpp` |
| functional run | `scripts/netem-pair-soak` |

## Not modelled

Duplicate delivery (a duplicate ack block is a no-op), ack-block loss (a lost
block is a delayed one that never arrives, and the next block restates), the
wrapping of the 32-bit wire sequence, the aggregate ring, keep-alive marking and
the concurrent writers behind `sent_lock` (the stale-writer refusal is
`REFUSED_STALE`, counted overflow; the model has the single in-order writer).
`LostImpliesLaterAcked` is the reordering reading of RFC 9002 6.1.1: a packet
delayed past the threshold counts lost, and a later ack for it is ignored once
its record is gone.
