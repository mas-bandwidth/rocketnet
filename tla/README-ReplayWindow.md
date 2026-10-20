# ReplayWindow: the replay window of one encryption mapping

`tla/ReplayWindow.tla` is `struct rocketnet_replay_protection_t`
(`sdk/rocketnet_encryption_map.h`) and the two operations in
`sdk/rocketnet_replay.cpp` that own it, `rocketnet_replay_already_received` and
`rocketnet_replay_advance`, together with the one reader that speaks for it on the
wire, `rocketnet_ack_block_build` (`sdk/rocketnet_ack.cpp`), and the server's sealed
receive path that drives it (`sdk/rocketnet_server_protocol.cpp`). It has no link
under it: the window never sends, and what reaches it is a sequence that opened.
`tla/MCReplayWindow.tla` and `tla/MCReplayWindow.cfg` are the small instance. Every
measured case is a row in `tla/RUNS.tsv`.

The model checks two rules from
[t14 Replay protection](../standard/TRANSPORT.md#t14) and
[t10 Receive processing](../standard/TRANSPORT.md#t10): a forward jump of at least one
window depth clears the whole ring (the long-skip rule), and the server's slot gate
comes after the open and before the replay mark (the gate rule). Each rule is an
invariant, and each has a reversed witness: the instance set to the behaviour that
breaks the rule, which TLC rejects with a trace that replays against that behaviour by
hand. Both are recorded under "What TLC found". The code keeps both rules.

## What the model covers

**State the window owns** (`struct rocketnet_replay_protection_t` in
`sdk/rocketnet_encryption_map.h`; a new mapping starts zeroed with its slot `-1`):

| Variable | Code | Meaning |
|---|---|---|
| `highest` | `highest_opened` | the high-water mark; 0 before any open |
| `window` | `window[ROCKETNET_HISTORY_PACKETS / 64]` | the ring, one `BOOLEAN` per bit; sequence `s` lives at bit `s % W`, bit `b` in word `b div WordBits` |
| `received` | (ghost) | the sequences that opened and are still inside the retained range: the truth the window is meant to encode |
| `processed` | (ghost) | those of them a slot processed rather than ignored: what the ack block may truthfully speak for |
| `connected` | `client_index != -1` on the mapping | the mapping holds a slot |

`W == Words * WordBits` is `ROCKETNET_REPLAY_WINDOW_BITS` (`sdk/rocketnet_replay.h`),
which is `ROCKETNET_HISTORY_PACKETS` (`sdk/rocketnet_history.h`: 4096 by default and up
to 32768, always a multiple of 64). The retained range is `Floor..highest` with
`Floor = highest - W + 1` once `highest >= W`, never below 1:
`rocketnet_replay_already_received` refuses `s <= highest - W` as too old, and the ack
builder's floor is `largest - W + 1`; the two agree.

**Operators**, each cited to the function it models:

| Operator | Code | What |
|---|---|---|
| `AlreadyReceived(s)` | `rocketnet_replay_already_received`, `sdk/rocketnet_replay.cpp` | above `highest`: no; at or below `highest - W`: yes (too old, refused either way); inside: the ring bit |
| `Advance(s)` | `rocketnet_replay_advance`, `sdk/rocketnet_replay.cpp` | above `highest`: clear what the advance skips past, then move the mark; always: set the bit for `s` |
| `Cleared(s)` | the two branches of `rocketnet_replay_advance` | a skip shorter than `W` clears each skipped sequence's bit; a skip of `W` or more clears words `0..ClearedWords-1` (the code clears the whole ring) |
| `AckReports(s)` | `rocketnet_ack_block_build`, `sdk/rocketnet_ack.cpp` | the block walks from `largest = highest` down to the floor reading the same ring bit, so `s` is reported received exactly when it is in range and its bit is set |

**Actions**, one per operation and per outside event:

| Action | Code | What |
|---|---|---|
| `Receive(s)` | the sealed session path in `sdk/rocketnet_server_protocol.cpp` | a session packet with sequence `s` arrives and opens. The already-received check refuses a duplicate or a too-old sequence before the open, so an opened packet is never one the window refuses. With `GateBeforeAdvance = TRUE` (the rule, and the code: the `client_index == -1` check precedes `rocketnet_replay_advance`) a packet on a mapping without a slot leaves the window alone; with `FALSE` it advances the window and is then ignored |
| `Connect` | `rocketnet_server_process_connection_response`, `sdk/rocketnet_server_protocol.cpp` | the connection response opens and the mapping takes a slot; once |

The client is the same pair on a window that is always connected
(`rocketnet_client_open_session_packet`, `sdk/rocketnet_client_protocol.cpp`); `Receive`
with `connected` covers it. The model is `replay_session` only. The handshake window
(`replay_handshake`) is the same struct, but handshake packets are exempt from replay
marking and only raise its `highest_opened` (in `sdk/rocketnet_server_protocol.cpp` and in
`rocketnet_client_process_challenge`, `sdk/rocketnet_client_protocol.cpp`): `advance`
never runs on it, no ack block is built from it, and nothing here speaks for it.

**Constants:**

| Constant | Code | Committed | Witness |
|---|---|---|---|
| `Words`, `WordBits` | `ROCKETNET_HISTORY_PACKETS / 64` words of 64 bits | 6 words of 2 bits, `W = 12` | same |
| `ClearedWords` | the words the long-skip branch clears | `6` (all: the rule) | `4` (`window[0..3]`) |
| `MaxSeq` | the highest sequence explored | `36`, three windows | same |
| `GateBeforeAdvance` | the server checks the slot before advancing | `TRUE` (the rule) | `FALSE` |

## The rules

Safety only. The window has no clock and makes no promise about the future, so there
is no liveness property here; the connection models above it carry those.

| Invariant | Source | Says |
|---|---|---|
| `TypeOK` | | the obvious types |
| `WindowIsTruth` | the window's one job; behind the long-skip rule | inside the retained range a bit is set exactly for the sequences that opened |
| `GenuineIsAccepted` | the long-skip rule ([t14](../standard/TRANSPORT.md#t14)) | a genuine sequence inside the range that has not opened is never refused as already received |
| `AckReportsOnlyReceived` | the long-skip rule's second half (`rocketnet_ack_block_build` builds the block from the same window) | the ack block names only sequences that opened |
| `ReceivedIsRefused` | [t14 Replay protection](../standard/TRANSPORT.md#t14): a sequence whose received bit is set is refused | everything that opened is refused thereafter |
| `BelowFloorIsRefused` | [t14 Replay protection](../standard/TRANSPORT.md#t14): a sequence at or below the high-water mark minus the window depth is refused without opening | below the floor everything is refused |
| `HighestIsReceived` | the assert in `rocketnet_ack_block_build` that the largest sequence is always marked received | the high-water mark is itself marked |
| `NoAdvanceBeforeConnect` | [t10 Receive processing](../standard/TRANSPORT.md#t10): the server slot gate comes after decryption and before the replay mark | the session window of a mapping without a slot never moves |
| `AckReportsOnlyProcessed` | the harm the gate rule prevents | the ack block names only packets a slot processed |
| `Monotone` (action property) | | the mark never goes backwards; a slot is never given up |

`GenuineIsAccepted` and `AckReportsOnlyReceived` follow from `WindowIsTruth`; they are
stated separately because they are the sentences of the long-skip rule, and each gets
its own witness run.

## What the model abstracts away

- Sequence reconstruction from 32 wire bits (`rocketnet_reconstruct_sequence`); the
  model receives 64-bit sequences up to `MaxSeq`.
- The open itself. A `Receive(s)` is a packet that opened; a forgery that does not open
  never reaches `advance` and is not modelled. Who can produce a packet that opens
  under the session keys of a mapping without a slot is a question for the security
  model; here the event is simply possible, as the code's comment at the slot gate in
  `sdk/rocketnet_server_protocol.cpp` allows (a lost-response race or an injection).
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
`MCReplayWindowBrokenLongSkip.cfg` is the committed reversed witness: `ClearedWords = 4`,
checking `WindowIsTruth` alone, which TLC must reject.

The other reversed witnesses are the committed file with one constant changed to the
behaviour that breaks a rule and `INVARIANTS` cut to the one rule under test, so TLC
names that rule:

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
the one invariant, as `MCReplayWindowBrokenLongSkip.cfg` does.)

## What TLC found

Both counterexamples were replayed by hand through `rocketnet_replay_advance`,
`rocketnet_replay_already_received`, `rocketnet_ack_block_build` and the server's sealed
receive path, with the behaviour each witness names. The code clears the whole ring on a
long skip and gates on the slot before the mark, so it keeps both rules.

### 1. The long skip: stale bits that read as received (`ClearedWords < Words`)

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

At the default `W = 4096` (64 words), from a zeroed struct, a long-skip branch that
cleared only `window[0..3]` would do this:

1. `rocketnet_replay_advance(r, 2)`: `skipped = 2 < 4096`, the short-skip loop clears
   bit 1, then bit 2 is set, `highest_opened = 2`.
2. `rocketnet_replay_advance(r, 300)`: `skipped = 298`, clears bits `3..299`, sets bit
   300 (word 4), `highest_opened = 300`.
3. `rocketnet_replay_advance(r, 4397)`: `skipped = 4097 >= 4096`, so the long-skip
   branch zeroes `window[0..3]` (bits `0..255`) and nothing else, then sets bit
   `4397 % 4096 = 301`; `highest_opened = 4397`. Bit 300 is still set.
4. `rocketnet_replay_already_received(r, 4396)`: `4396 <= 4397`, `4396 > 4397 - 4096 =
   301`, so it returns bit `4396 % 4096 = 300`: **1**. A genuine packet 4396 is
   refused as already received.
5. `rocketnet_ack_block_build` on the same struct: `largest = 4397`, `floor = 302`, the
   first-run loop reads bit 301 (set) then bit 300 (set) then bit 299 (clear), so
   `first_range_length = 2`: the peer is told 4396 and 4397 arrived.

The same shape at full width (fill `1..W`, advance to `3W`) leaves every bit
`256..4095` stale, so every sequence `2W + 257 .. 3W - 1` reads as received and the
block reports a run of 3840 packets of which one arrived. A test that probes only
sequences in word 0 passes either way; `rocketnet_test_replay_long_skip_clears_whole_window`
(`sdk/rocketnet_tests.cpp`) is written in terms of `ROCKETNET_REPLAY_WINDOW_BITS` and
reaches past word 3.

The code clears the whole ring in `rocketnet_replay_advance`'s long-skip branch,
`memset( replay->window, 0, sizeof( replay->window ) )`, which is `ClearedWords = Words`
in the model, the committed run.

### 2. The gate: advancing the session window before the slot check (`GateBeforeAdvance = FALSE`)

`w-gate`, `GateBeforeAdvance = FALSE`. TLC's trace is two states: from `Init`
(`connected = FALSE`), `Receive(1)` advances the window to `highest = 1`,
`received = {1}`, and `NoAdvanceBeforeConnect` is violated.

In code terms: a session packet for a mapping with `client_index == -1` passes the
already-received check (the window is empty) and opens (it carries the session keys);
a receive path that called `rocketnet_replay_advance( replay, sequence )` before the
`client_index == -1` check would set bit 1 and move `highest_opened` to 1 for a packet
that is then ignored.

Why the order matters, in this model's terms: a sequence marked on a pending mapping is
reported by this end's next ack block as received although the packet was never
processed. `w-gate-processed` checks exactly that harm: the same two states, with
`received = {1}` and `processed = {}` in state 2, so `AckReports(1)` holds for a packet
nothing processed and `AckReportsOnlyProcessed` is violated. With the other order every
other invariant still holds (the window stays truthful about what opened), so the defect
would not be in the window but in what the window is then made to say.

The rule is the standard's: [t10 Receive processing](../standard/TRANSPORT.md#t10) places
the server slot gate after decryption and before the replay mark, and a packet it refuses
is counted `NOT_CONNECTED` and not marked. In `sdk/rocketnet_server_protocol.cpp` the
`client_index == -1` check precedes `rocketnet_replay_advance`; `GateBeforeAdvance = TRUE`,
the committed run, is that order.

## RUNS.tsv

`tla/RUNS.tsv` holds the two committed cases: `MCReplayWindow.cfg` (958,503 states,
53,249 distinct, pass) and `MCReplayWindowBrokenLongSkip.cfg` (339 states, 127 distinct,
`WindowIsTruth` violated as expected). The edited-constant witnesses in the table above
are run by hand as shown under "How to run it".
