# ReplayWindow: the replay window of one encryption mapping

`tla/ReplayWindow.tla` is `struct rocketnet_replay_protection_t`
(`sdk/rocketnet_encryption_map.h:31-35`) and the two operations in
`sdk/rocketnet_replay.cpp` that own it, `rocketnet_replay_already_received` and
`rocketnet_replay_advance`, together with the one reader that speaks for it on the
wire, `rocketnet_ack_block_build` (`sdk/rocketnet_ack.cpp:54-137`), and the server
call site that drives it (`sdk/rocketnet_server_protocol.cpp:901-972`). It has no
link under it: the window never sends, and what reaches it is a sequence that opened.
`tla/MCReplayWindow.tla` and `tla/MCReplayWindow.cfg` are the small instance. Every
TLC run is a row in `tla/RUNS.tsv`.

The model exists for two fix cards, `rnfix-replay-long-skip-clears-window-r2` (F1,
rocketnet#189, High) and `rnfix-replay-advanced-after-connect-check-r2` (OV-34, Low).
Each card's rule is an invariant, and each has a reversed witness: the committed
instance changed to the defective behavior, which TLC rejects with a trace that
replays against the code before its fix. Both are recorded under "What TLC found".

## What the model covers

**State the window owns** (`sdk/rocketnet_encryption_map.h:31-35`; a new mapping is
zeroed at `sdk/rocketnet_encryption_map.cpp:140` and its slot is `-1` at `:141`):

| Variable | Code | Meaning |
|---|---|---|
| `highest` | `highest_opened` | the high-water mark; 0 before any open |
| `window` | `window[ROCKETNET_HISTORY_PACKETS / 64]` | the ring, one `BOOLEAN` per bit; sequence `s` lives at bit `s % W`, bit `b` in word `b div WordBits` |
| `received` | (ghost) | the sequences that opened and are still inside the retained range: the truth the window is meant to encode |
| `processed` | (ghost) | those of them a slot processed rather than ignored: what the ack block may truthfully speak for |
| `connected` | `client_index != -1` on the mapping | the mapping holds a slot |

`W == Words * WordBits` is `ROCKETNET_REPLAY_WINDOW_BITS` (`sdk/rocketnet_replay.h:26`,
`ROCKETNET_HISTORY_PACKETS`, 4096 by default and up to 32768, always a multiple of
64: `sdk/rocketnet_history.h:7-14`). The retained range is `Floor..highest` with
`Floor = highest - W + 1` once `highest >= W`, never below 1: `already_received`
refuses `s <= highest - W` as too old (`sdk/rocketnet_replay.cpp:35-37`) and the ack
builder's floor is `largest - W + 1` (`sdk/rocketnet_ack.cpp:70-72`); the two agree.

**Operators**, each cited to the function it models:

| Operator | Code | What |
|---|---|---|
| `AlreadyReceived(s)` | `rocketnet_replay_already_received`, `sdk/rocketnet_replay.cpp:27-40` | above `highest`: no; at or below `highest - W`: yes (too old, refused either way); inside: the ring bit |
| `Advance(s)` | `rocketnet_replay_advance`, `sdk/rocketnet_replay.cpp:42-70` | above `highest`: clear what the advance skips past, then move the mark; always: set the bit for `s` |
| `Cleared(s)` | the two branches at `:54-65` | a skip shorter than `W` clears each skipped sequence's bit; a skip of `W` or more clears `ClearedWords` words, which is the whole ring in the current code (`ClearedWords = Words`) |
| `AckReports(s)` | `rocketnet_ack_block_build`, `sdk/rocketnet_ack.cpp:48-52, 87, 102, 114` | the block walks from `largest = highest` down to the floor reading the same ring bit, so `s` is reported received exactly when it is in range and its bit is set |

**Actions**, one per operation and per outside event:

| Action | Code | What |
|---|---|---|
| `Receive(s)` | `sdk/rocketnet_server_protocol.cpp:901-972` | a session packet with sequence `s` arrives and opens. The check at `:901` refuses a duplicate or a too-old sequence before the open, so an opened packet is never one the window refuses. With `GateBeforeAdvance = TRUE` (the rule) a packet on a mapping without a slot leaves the window alone; with `FALSE` (today, `:962` then `:967`) it advances the window and is then ignored |
| `Connect` | `sdk/rocketnet_server_protocol.cpp:710-713` | the connection response opens and the mapping takes a slot; once |

The client is the same pair on a window that is always connected
(`sdk/rocketnet_client_protocol.cpp:689` and `:713`); `Receive` with `connected`
covers it. The model is `replay_session` only. The handshake window (`replay_handshake`)
is the same struct, but handshake packets are exempt from replay marking and only raise
its `highest_opened` (`sdk/rocketnet_server_protocol.cpp:883-889` and `:949-950`;
`sdk/rocketnet_client_protocol.cpp:511-512`): `advance` never runs on it, no ack block
is built from it, and nothing here speaks for it.

**Constants:**

| Constant | Code | Committed | Witness |
|---|---|---|---|
| `Words`, `WordBits` | `ROCKETNET_HISTORY_PACKETS / 64` words of 64 bits | 6 words of 2 bits, `W = 12` | same |
| `ClearedWords` | the words the long-skip branch clears | `6` (all: the rule and current code) | `4` (`window[0..3]`: code before F1 fix) |
| `MaxSeq` | the highest sequence explored | `36`, three windows | same |
| `GateBeforeAdvance` | the server checks the slot before advancing | `TRUE` (the rule) | `FALSE` (today's code) |

## The rules

Safety only. The window has no clock and makes no promise about the future, so there
is no liveness property here; the connection models above it carry those.

| Invariant | Source | Says |
|---|---|---|
| `TypeOK` | | the obvious types |
| `WindowIsTruth` | the window's one job; behind F1 | inside the retained range a bit is set exactly for the sequences that opened |
| `GenuineIsAccepted` | F1 as the card states it | a genuine sequence inside the range that has not opened is never refused as already received |
| `AckReportsOnlyReceived` | F1's second half (`sdk/rocketnet_ack.cpp` builds the block from the same window) | the ack block names only sequences that opened |
| `ReceivedIsRefused` | STANDARD.md _Replay Protection_: "within the window accept a sequence only once" | everything that opened is refused thereafter |
| `BelowFloorIsRefused` | STANDARD.md _Replay window sizing_: "an undersized window discards old legitimate packets; it never accepts a replay" | below the floor everything is refused |
| `HighestIsReceived` | the assert at `sdk/rocketnet_ack.cpp:94` | the high-water mark is itself marked |
| `NoAdvanceBeforeConnect` | STANDARD.md _Reading packets_, the slot gate before the mark (OV-34; the standard's rule, its PR pending) | the session window of a mapping without a slot never moves |
| `AckReportsOnlyProcessed` | the harm behind OV-34, named by the STANDARD change | the ack block names only packets a slot processed |
| `Monotone` (action property) | | the mark never goes backwards; a slot is never given up |

`GenuineIsAccepted` and `AckReportsOnlyReceived` follow from `WindowIsTruth`; they are
stated separately because they are the sentences the F1 card uses, and each gets its
own witness run.

## What the model abstracts away

- Sequence reconstruction from 32 wire bits (`rocketnet_reconstruct_sequence`); the
  model receives 64-bit sequences up to `MaxSeq`.
- The open itself. A `Receive(s)` is a packet that opened; a forgery that does not open
  never reaches `advance` (`sdk/rocketnet_server_protocol.cpp:917-922`) and is not
  modelled. Who can produce a packet that opens under the session keys of a mapping
  without a slot is a question for the security model; here the event is simply
  possible, as the code's comment at `:964-965` allows ("a lost-response race or an
  injection").
- The ack block's truncation (`max_ranges`, the `0xFFFF` run and gap caps): it loses
  only the oldest information, which the next block restates.
- The handshake window: see "What the model covers". Only `replay_session` is modelled.
- Removal of the mapping. A mapping that loses its slot is removed and a new one starts
  from `Init`; the model is one mapping's lifetime.

## How to run it

On a Linux bench with the TLA+ tools (`tla2tools.jar`, TLC 2.19 or later), from
`tla/`:

```sh
java -cp /path/to/tla2tools.jar tlc2.TLC -workers 4 -cleanup -config MCReplayWindow.cfg MCReplayWindow
```

`MCReplayWindow.cfg` is the committed run: the rule on both counts (`ClearedWords = 6`,
`GateBeforeAdvance = TRUE`), checking every invariant above and `Monotone`. It finishes
in a few seconds: 958,503 states, 53,249 distinct, depth 16.

The reversed witnesses are the same file with one constant changed to what the code
does today and `INVARIANTS` cut to the one rule under test, so TLC names that rule:

| Run | Edit | `INVARIANTS` | Expected |
|---|---|---|---|
| committed | none | all | passes |
| w-longskip-window | `ClearedWords = 4` | `WindowIsTruth` | **fails**: the 5-state trace below |
| w-longskip-genuine | `ClearedWords = 4` | `GenuineIsAccepted` | **fails**: 6 states, same shape |
| w-longskip-ack | `ClearedWords = 4` | `AckReportsOnlyReceived` | **fails**: 5 states, same shape |
| w-gate | `GateBeforeAdvance = FALSE` | `NoAdvanceBeforeConnect` | **fails**: 2-state trace below |
| w-gate-processed | `GateBeforeAdvance = FALSE` | `AckReportsOnlyProcessed` | **fails**: the same 2 states |

For example:

```sh
sed -e 's/ClearedWords      = 6/ClearedWords      = 4/' \
    -e '/^INVARIANTS/,/^PROPERTIES/{/^    [A-Z]/d}' \
    -e 's/^INVARIANTS/INVARIANTS\n    WindowIsTruth/' MCReplayWindow.cfg > /tmp/witness.cfg
java -cp /path/to/tla2tools.jar tlc2.TLC -workers 4 -cleanup -config /tmp/witness.cfg MCReplayWindow
```

(or write the five-line cfg by hand: `SPECIFICATION MCSpec`, the five constants, and
the one invariant.)

## What TLC found

Both counterexamples were replayed by hand through `sdk/rocketnet_replay.cpp` and
`sdk/rocketnet_server_protocol.cpp` at main `3ad4179`. Both were real at that revision.
The F1 fix clears the whole ring; OV-34 belongs to its separate fix card.

### 1. F1: a long skip leaves stale bits that read as received (real, `sdk/rocketnet_replay.cpp:54-60`)

`w-longskip-window`, `ClearedWords = 4` of 6 (`W = 12`). With four workers TLC prints one
of several equivalent short traces (which small sequences open before the jump varies;
the jump and the end state do not). The recorded run's, five states:

| State | Step | `highest` | bits set | `received` |
|---|---|---|---|---|
| 1 | `Init` | 0 | none | `{}` |
| 2 | `Connect` | 0 | none | `{}` |
| 3 | `Receive(2)` | 2 | 2 | `{2}` |
| 4 | `Receive(8)` | 8 | 2, 8 | `{2, 8}` |
| 5 | `Receive(21)` | 21 | 8, 9 | `{21}` |

State 5 violates `WindowIsTruth`: the range is `10..21`, sequence 20 lives at bit
`20 % 12 = 8`, the bit is set, and 20 never opened. The skip from 8 to 21 is 13, at
least `W`, so the long-skip branch ran and cleared words `0..3` (bits `0..7`); bit 8,
in word 4, kept the mark sequence 8 left there. `GenuineIsAccepted` fails on the same
state (`AlreadyReceived(20)` is `TRUE`: a genuine packet 20 is refused) and so does
`AckReportsOnlyReceived` (`AckReports(20)`: the block tells the peer 20 arrived).

The code now clears the whole ring in `sdk/rocketnet_replay.cpp`'s
`rocketnet_replay_advance`, matching the committed run (`ClearedWords = Words`).
The measured recheck passes the committed control (958,503 generated, 53,249 distinct)
and rejects the `ClearedWords = 4` witness on `WindowIsTruth` (339 generated, 127
distinct, exit 12). This witness has a four-state trace: after `Receive(8)`, the jump
to `Receive(21)` leaves bit 8 set in the defective branch. The whole-window clear
removes that bit before setting bit 9 for 21, so sequence 20 remains unreceived and
cannot appear in an ack block.

Against the code before the fix, at the default `W = 4096` (64 words), from a zeroed struct:

1. `rocketnet_replay_advance(r, 2)`: `skipped = 2 < 4096`, the loop at `:63-64` clears
   bit 1, `:69` sets bit 2, `highest_opened = 2`.
2. `rocketnet_replay_advance(r, 300)`: `skipped = 298`, clears bits `3..299`, sets bit
   300 (word 4), `highest_opened = 300`.
3. `rocketnet_replay_advance(r, 4397)`: `skipped = 4097 >= 4096`, so `:56-59` zero
   `window[0..3]` (bits `0..255`) and nothing else; `:69` sets bit `4397 % 4096 = 301`;
   `highest_opened = 4397`. Bit 300 is still set.
4. `rocketnet_replay_already_received(r, 4396)`: `4396 <= 4397`, `4396 > 4397 - 4096 =
   301`, so `:39` returns bit `4396 % 4096 = 300`: **1**. A genuine packet 4396 is
   refused as already received.
5. `rocketnet_ack_block_build` on the same struct: `largest = 4397`, `floor = 302`, the
   first-run loop at `:87-93` reads bit 301 (set) then bit 300 (set) then bit 299
   (clear), so `first_range_length = 2`: the peer is told 4396 and 4397 arrived.

The card's own test (fill `1..W`, advance to `3W`) is the same shape at full width:
before the fix, after the jump every bit `256..4095` is stale, so every sequence
`2W + 257 .. 3W - 1` reads as received and the block reports a run of 3840 packets
of which one arrived. The existing `rocketnet_test_replay_protection`
(`sdk/rocketnet_tests.cpp:3503-3511`) says
"a far-ahead advance clears the whole window" but probes only sequences 1, 2 and
`W + 1`, all in word 0, so it passed before the fix; that is why the fix card asks for a test
written in terms of `ROCKETNET_REPLAY_WINDOW_BITS` that reaches past word 3.

The fix uses `memset( replay->window, 0, sizeof( replay->window ) )` in the long-skip
branch, which is `ClearedWords = Words` in the model. The committed run checks that rule.

### 2. OV-34: the server advances the session window before the slot check (real, `sdk/rocketnet_server_protocol.cpp:962` before `:967`)

`w-gate`, `GateBeforeAdvance = FALSE`. TLC's trace is two states: from `Init`
(`connected = FALSE`), `Receive(1)` advances the window to `highest = 1`,
`received = {1}`, and `NoAdvanceBeforeConnect` is violated.

Against the code at `sdk/rocketnet_server_protocol.cpp`: a session packet for a mapping
with `client_index == -1` passes the already-received check at `:901` (the window is
empty), opens at `:914` (it carries the session keys), reaches
`rocketnet_replay_advance( replay, sequence )` at `:962`, which sets bit 1 and moves
`highest_opened` to 1, and only then hits `if ( mapping->client_index == -1 )` at `:967`
and is ignored. The window moved before connect, exactly as the card says.

Why the order matters, in this model's terms: a sequence marked on a pending mapping is
reported by this end's next ack block as received although the packet was never
processed. `w-gate-processed` checks exactly that harm: the same two states, with
`received = {1}` and `processed = {}` in state 2, so `AckReports(1)` holds for a packet
nothing processed and `AckReportsOnlyProcessed` is violated. With the current order every
other invariant still holds (the window stays truthful about what opened), so the defect
is not in the window but in what the window is then made to say.

The rule is the standard's. The code's comment at `:957-960` follows the reading order
STANDARD.md gave before this model: _Reading packets_ marked every opened session packet
and _Processing session packets at the server_ then ignored the slotless one. The owner
ruled that the standard changes first when correctness requires it, and it does here:
STANDARD.md _Reading packets_ now ignores a session packet whose mapping has no slot
after the open and before the replay-window mark, citing `NoAdvanceBeforeConnect`, and
_Processing session packets at the server_ agrees (the STANDARD.md change is PR #207;
the code fix is the card `rnfix-replay-advanced-after-connect-check-r2`, which needs that
PR merged first). `GateBeforeAdvance = TRUE`, the committed run, is that rule.

## RUNS.tsv

Six models landed in parallel; this PR does not edit `tla/RUNS.tsv`. The rows it owes,
tab-separated, bench hetzner2, 2026-10-13:

```
MCReplayWindow	MCReplayWindow.cfg (ClearedWords 6, GateBeforeAdvance TRUE); checks all invariants, Monotone	958503	53249	pass	hetzner2	2026-10-13
MCReplayWindow	MCReplayWindow.cfg with ClearedWords 4; checks WindowIsTruth (reversed witness, F1)	737	246	fail as expected (F1): Invariant WindowIsTruth is violated, 5-state trace, see README-ReplayWindow.md	hetzner2	2026-10-13
MCReplayWindow	MCReplayWindow.cfg with ClearedWords 4; checks GenuineIsAccepted (reversed witness, F1)	798	252	fail as expected (F1): Invariant GenuineIsAccepted is violated, 6-state trace	hetzner2	2026-10-13
MCReplayWindow	MCReplayWindow.cfg with ClearedWords 4; checks AckReportsOnlyReceived (reversed witness, F1)	937	270	fail as expected (F1): Invariant AckReportsOnlyReceived is violated, 5-state trace	hetzner2	2026-10-13
MCReplayWindow	MCReplayWindow.cfg with GateBeforeAdvance FALSE; checks NoAdvanceBeforeConnect (reversed witness, OV-34)	2	2	fail as expected (OV-34): Invariant NoAdvanceBeforeConnect is violated, 2-state trace	hetzner2	2026-10-13
MCReplayWindow	MCReplayWindow.cfg with GateBeforeAdvance FALSE; checks AckReportsOnlyProcessed (reversed witness, OV-34, the harm)	2	2	fail as expected (OV-34): Invariant AckReportsOnlyProcessed is violated, 2-state trace	hetzner2	2026-10-13
```
