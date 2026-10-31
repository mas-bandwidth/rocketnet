# RNSentRingLayout

The sender's sent-packet ring, its reference ring and the receipt ring of
`sdk/rocketnet_window_ack.cpp`, and the receipt serial counter of
`sdk/rocketnet_endpoint.cpp`, at their layout and with the code's own
arithmetic. [RNSentMap](README-RNSentMap.md), `RNSentHistory.tla` and
`RNReceipts.tla` check these as a map, a set and a sequence; this model checks
the arrays and counters behind them.

## State

| Model | Code |
|---|---|
| `pk[i]` | `region->sent_packets[i]`: `id` is `packet_id != 0`, `rfirst`, `rcount`, `unrel`, `rcfirst` are `ref_first`, `ref_count`, `unreliable_receipts`, `receipt_first`; `gid` is a ghost, the write index of the packet in the slot |
| `ph`, `pt` | `ack->pkt_head`, `ack->pkt_tail` (64-bit, never wrap) |
| `rf[c]` | `region->sent_refs[c]`, holding the ghost identity of what was written there (`[k, j]`: packet `k`'s `j`-th reference) and `z`, a zeroed serial |
| `rh`, `rt` | `ack->ref_head`, `ack->ref_tail` |
| `rq[i]` | `region->receipts[i]`, holding the ghost absolute index of the event written there |
| `qh`, `qt`, `vh`, `vt` | `receipt_head`, `receipt_tail`, `receipt_visible_head`, `receipt_visible_tail` |
| `sweep` | `ack->receipt_sweep` |
| `serial` | `endpoint->serial`, modulo `SerialMod` (`2^32` in the code) |

The packet ring has `P` entries, slot `k % P` for the `k`-th packet. The
reference ring has `max_refs` entries (`16P` in the code, `sent_ref_entries`,
`sdk/rocketnet_allocator.cpp`): a packet's reliable numbers then its receipt
serials from `ref_tail % max_refs` on, at most `R` of them
(`RN_MESSAGE_RECEIPT_SERIALS_MAX` = 16 in the code). The receipt ring has
`Rq` entries (`max_message_receipts`).

The receipt ring takes events from the sent rings' resolves and from
elsewhere, and nothing else joins them, so the constant `Part` checks each
half apart: `"sent"` runs the packet and reference rings with every resolve's
outcome checked but not written to the receipt ring, `"receipt"` the receipt
ring and the serial with emits from anywhere.

## Actions and the code they mirror

Line numbers are `sdk/rocketnet_window_ack.cpp` at this change unless stated.

- **`Commit(count, rc)`**: `rn_window_ack_commit` (592). The reclaim
  (`rn_window_ack_reclaim_head`, 375: the head past holes, 379, and both
  reference counters back to 0 once the ring is empty, 391-395), then the
  eviction loop (646) while `pkt_tail - pkt_head >= P` or
  `( ref_tail - ref_head ) + total > max_refs`: a live head's receipts resolve
  as LOSS, its `packet_id` is cleared, `ref_head` moves past its references,
  the head advances and the ring is reclaimed again (664). Then the write:
  the references at `( ref_tail + i ) % max_refs` (671-677), the entry at
  `pkt_tail % P` (670) with `receipt_first = ( ref_first + count ) % max_refs`
  (685).
- **`Ack(k)`**: `rn_window_ack_on_ack` (710) for an id the ring still holds:
  its receipts resolve ACKED, its `packet_id` is cleared and the head is
  reclaimed (817). The `sent_index` lookup is RNOpenAddress.tla's; the model
  acks the live entry directly.
- Resolve, inside both: `rn_window_ack_receipt_resolve` (574), each serial not
  yet zeroed emits and is zeroed (581).
- **`Sweep(h)`**: `rn_window_ack_receipt_sweep` (851): the cursor clamped at
  `pkt_head` (864), then entries in order until the first live one inside the
  deadline. The deadline is fixed and entries are in send order, so the
  expired ones are those written before some `h`.
- **`EmitOther`**: `rn_window_ack_receipt_emit` (543) from outside the rings,
  `rn_window_ack_receipt_emit_loss` (568) and a reliable message's completion
  in `rn_window_ack_on_ack`; a full ring drops its oldest (555-557).
- **`BeginUpdate`**, **`EndUpdate`**: `rn_window_ack_receipt_begin_update`
  (827) and `rn_window_ack_receipt_end_update` (843).
- **`ReadReceipts`**: `rn_window_ack_receipts` (880), the first event clamped
  at `receipt_head` (886).
- **`IssueSerial`**: `rn_endpoint_finish` (`sdk/rocketnet_endpoint.cpp`
  482-487), `rn_endpoint_in_place_finish` (696-698) and the in-place commit of
  `rn_message_object_finish` (`sdk/rocketnet_message_server.cpp` 1165-1167):
  `serial + 1`, and 1 in place of 0.

## Invariants

- **`RingBounded`**: `pkt_tail - pkt_head <= P`, `0 <= ref_tail - ref_head <= max_refs`,
  `receipt_tail - receipt_head <= Rq`.
- **`SlotsIntact`**: every slot from `pkt_head` to `pkt_tail` holds the packet
  written at that index.
- **`RefsIntact`**: every live entry's references are still in their cells,
  `receipt_first` is `ref_first + ref_count`, `ref_tail - ref_head` is the sum of
  the entries' references, and the head entry's references start at
  `ref_head % max_refs`.
- **`OutcomesOwn`**: every outcome a resolve reports is one of its own packet's
  serials.
- **`ReadExact`**: every reported event is the one written at its index.
- **`SerialNeverZero`**: no serial is 0 (`rn_window_ack_receipt_emit` drops a
  serial of 0, 545-546, so a 0 would lose its receipt).
- **`RefEvictionIdle`** (the code-ratio control only): the reference test
  never evicts.

## The instances

`P = 3`, `R = 2`, `Rq = 3`, `SerialMod = 4`.

- `MCRNSentRingLayout.cfg` (sent part): a reference ring of four, below the
  code's ratio, so the reference test binds. 20,742 distinct states, depth 13.
- `MCRNSentRingLayoutCodeRatio.cfg` (sent part): a reference ring of
  `R * P = 6`, the code's ratio (`16P` against at most 16 references a packet).
  `RefEvictionIdle` holds: the packet test leaves at most `P - 1` entries of
  at most `R` references each before a write of at most `R`, so the reference
  test is a defense that never fires at the code's sizing. 38,883 distinct
  states, depth 12.
- `MCRNSentRingLayoutReceipts.cfg` (receipt part): eight events through a ring
  of three.

The two sent-part controls use `VIEW View`. `pkt_head` and `pkt_tail` move in
steps of `P` and the reference counters in steps of `max_refs` without
changing any slot, so the view takes each back to its first lap; a packet slot
outside `[pkt_head, pkt_tail)` and a reference cell no live entry names are
dead (every action writes them before it reads them, which `SlotsIntact` and
`RefsIntact` check), so the view leaves them out. With it the state graph
closes (depth 13 and 12) well inside `MaxWrites = 30`, so the bound does not
cut the search: every reachable configuration of the rings is checked, not
only the first thirty packets. The witnesses run without the view at
`MaxWrites = 6`.

## Reversed witnesses

Each turns one rule off and must fail with the invariant it names. Every
counterexample was checked against the C by hand.

1. **`MCRNSentRingLayoutBrokenEvictPastP.cfg`** (`EvictAtP = FALSE`): the
   eviction loop on `pkt_tail - pkt_head > P` (646). TLC: four commits of one
   reference each, no ack. At the fourth, `pkt_tail - pkt_head = 3` is not
   above `P`, and `ref_tail - ref_head + 1 = 4` is not above four, so nothing
   is evicted and the entry is written at `3 % 3 = 0`, over packet 0, which is
   still live (`SlotsIntact`). Against the C: `sent_index` still maps packet
   0's id to entry 0, so its ack reads packet 3's entry (an assert in debug,
   the wrong numbers acked in release).
2. **`MCRNSentRingLayoutBrokenRefTestOffByOne.cfg`** (`RefTestExact = FALSE`):
   the reference test `+ total > max_refs + 1`. TLC: commits of one, two and
   two references; at the third, three references are in use and three plus
   two is not above five, so the write takes cells 3 and 0, and cell 0 is
   packet 0's live reference (`RefsIntact`). Against the C: packet 0's ack
   reads `sent_refs[0]`, a number of packet 2, and acks it.
3. **`MCRNSentRingLayoutBrokenResetNonEmpty.cfg`** (`ResetEmpty = FALSE`): the
   reset of `ref_head` and `ref_tail` whenever the head moved, not only when
   `pkt_head == pkt_tail` (391). TLC: two commits of one reference, then the
   ack of packet 0; the reclaim advances the head and resets both counters
   while packet 1 is live, so `ref_tail - ref_head = 0` against one reference
   in use (`RefsIntact`). Against the C: the next commit writes from cell 0
   and overwrites packet 1's reference.
4. **`MCRNSentRingLayoutBrokenNoReceiptClamp.cfg`** (`ReceiptClamp = FALSE`):
   `rn_window_ack_receipts` without `if ( first < ack->receipt_head ) first =
   ack->receipt_head;` (886-887). TLC: three events fill the ring; the update
   ends (`visible = [0, 3)`); a fourth event (a loss from the send pass) drops
   the oldest (`receipt_head = 1`) and is written into cell 0; the read starts
   at 0 and reports cell 0, which holds event 3 (`ReadExact`). Against the C:
   the application is handed a later receipt in place of the dropped one.
5. **`MCRNSentRingLayoutBrokenSerialZero.cfg`** (`SerialSkip = FALSE`): the
   serial wraps to 0. TLC: four issues at `SerialMod = 4` reach 0
   (`SerialNeverZero`). Against the C: a send with serial 0 has no receipt
   (`rn_window_ack_receipt_emit` returns at 545-546), the case the
   `if ( serial == 0 ) serial = 1;` steps (endpoint.cpp 485-486, 697-698;
   message_server.cpp 1166-1167) exist for.
