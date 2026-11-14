# AckBlockWrap

`AckBlockWrap.tla` models the ack block's largest received across the wire
sequence wrap (standard/TRANSPORT.md t15): one direction of one session, the
builder that writes the block and the reader that decides it.

## What is modelled

| Variable | Meaning | Code |
|---|---|---|
| `highestSent` | the reader's highest sent session sequence, a natural from `Start - 1` | `rocketnet_client_protocol_highest_sent_session` (sdk/rocketnet_client_protocol.cpp: the ack endpoint's `highest_sequence`, the highest sequence a packet sealed) and `rocketnet_server_protocol_highest_sent_session` (sdk/rocketnet_server_protocol.cpp: the client's ack endpoint's `highest_sequence`; `mapping->send_sequence_session - 1`, floored at 0, for a protocol built without the ack bookkeeping or a mapping with no client slot, `client_index < 0`) |
| `opened` | the sequences the other end has opened | the replay window, `rocketnet_replay_advance` (sdk/rocketnet_replay.cpp) |
| `flight` | the block in flight: wire largest (`highest opened mod Modulus`), first range length, range count, and its ghost truth | the block `rocketnet_ack_block_write` lays out (7 bytes, plus 4 per range) |
| `read` | the reader's last decision, with the block's ghost truth | the return of `rocketnet_ack_block_read` |

| Action | Code |
|---|---|
| `Send` | the sender's session sequence advances (environment: the session window bounds it to less than Modulus/2 past any sequence an ack can still name) |
| `Open(s)` | the other end opens sequence s (environment) |
| `WriteBlock` | `rocketnet_ack_block_build` then `rocketnet_ack_block_write` (sdk/rocketnet_ack.cpp): the all-zero block while nothing is opened, else the wire largest and the received run ending at it |
| `Forge(w, f, c)` | a block the builder did not write (environment): any wire largest, first range length 0 to 2 and range count 0 or 1, but not the all-zero empty block, and no ranges behind a first run (that block's fate is the walk's, abstracted here). It covers both malformed shapes on the fixed fields (first range length 0 with anything else nonzero; a first run longer than the largest it reconstructs to) and well-formed forged blocks of a first run alone |
| `ReadBlock` | `rocketnet_ack_block_read` (sdk/rocketnet_ack.cpp): empty on the wire fields, refused when the first range length is 0 with anything else nonzero, else the largest reconstructed by `rocketnet_reconstruct_near` (sdk/rocketnet_protocol.cpp; nearest to highest sent, the larger on a tie) and refused when the first range length exceeds it |

Ranges below the first are abstracted (the honest count is 0); their walk,
t15 rules 4 and 5, is SessionAcks' subject. Reconstruction takes the nearest of
every candidate sharing the wire value, which picks the same value as the
code's three candidates (base - 2^32, base, base + 2^32).

Invariants: `TypeOK`; `EmptyRecognised` (a block written while nothing is
opened reads empty, whatever the reader's highest sent); `LargestExact` (a
nonempty block reconstructs to the true highest opened); `NeverRefusedHonest`
(a block the builder wrote is never refused); `MalformedRefused` (a forged
block of either malformed shape is refused). `MalformedRefused` names t15
rules 2 and 3, the two on the fixed fields (rule 1, the size, and rules 4
and 5, the walk, are abstracted), on the block's own fields (`read.frl`, and
`read.near`, the largest the wire value reconstructs to with the same
nearest-candidate rule `Decide` uses; `LargestExact` checks that rule against
the truth), not through `Decide`'s refusals. A reader without the run-bound
refusal fails it (`MCAckBlockWrapBrokenRunBound.cfg`); one without the
zero-first-run refusal would too (`Forge(1, 0, 0)` would read nonempty), but
that has no committed witness. Until
2026-11-13 Forge wrote only first range length 0 and the invariant was "every
forged block is refused", which held only because every forged block had that
one shape; a first run longer than the reconstructed largest was never on the
wire, and the reader's refusal of it (`first_range_length > largest`,
sdk/rocketnet_ack.cpp `rocketnet_ack_block_read`) was unchecked.

## Instance

Modulus 8 (2^32 in the code), Start 5 (at or above Modulus/2, as the DEV seam
`ROCKETNET_DEV_SESSION_SEQUENCE_START` near 2^32 is), MaxSeq 12: the session
crosses the wrap at 8, where a genuine largest of 8 has wire value 0 and a
first range length of at least one. Highest sent starts at 4 there, so a
wire zero never reconstructs to 0; `MCAckBlockWrapFromOne.cfg` runs the same
rule from Start 1, the production start, to MaxSeq 6, where below Modulus/2 a wire zero
with a first run reconstructs to 0 and is refused by the run bound.

| Config | Constants | Expected |
|---|---|---|
| `MCAckBlockWrap.cfg` | `BrokenHalfRange = FALSE`, `RunBoundChecked = TRUE`, every invariant | pass (4,149,761 generated, 807,244 distinct on aws-mut-3, 9.0 s, 2026-11-13) |
| `MCAckBlockWrapFromOne.cfg` | `Start = 1`, `MaxSeq = 6`, `BrokenHalfRange = FALSE`, `RunBoundChecked = TRUE`, every invariant | pass (830,526 generated, 158,261 distinct on aws-mut-3, 2.5 s, 2026-11-13) |
| `MCAckBlockWrapBrokenHalfRange.cfg` | `BrokenHalfRange = TRUE`, `EmptyRecognised` | exit 12, `EmptyRecognised` violated |
| `MCAckBlockWrapBrokenRunBound.cfg` | `RunBoundChecked = FALSE`, `MalformedRefused` | exit 12, `MalformedRefused` violated |

## The reversed witnesses

`BrokenHalfRange = TRUE` is the reader before 2026-10-24: the largest is
reconstructed first and the empty test is made on the reconstructed value. TLC's
trace is three states: `highestSent = 4`, nothing opened; `WriteBlock` writes
the all-zero block; `ReadBlock` reconstructs wire 0 against 4 to 8 (8 - 4 = 4
ties 4 - 0, and the larger wins), the largest is nonzero with first range
length 0, and the block is refused.

Replayed against sdk/rocketnet_ack.cpp at the soak head 60fa1fb6e961 by hand:
`rocketnet_reconstruct_near( 0, 2^32 - 1001 )` (the soak's start through the
seam, less one) has base 0, candidate 0 at distance 2^32 - 1001 and above
2^32 at distance 1001, so returns 2^32; `largest == 0` is false and
`first_range_length == 0` returns 0, BAD_ACK_BLOCK. The same holds at every
highest sent of 2^31 or more (at exactly 2^31 the tie goes to 2^32), which is
what `test_ack_block_empty_read_across_wrap` (tests/test_unit_ack_block_wrap.cpp)
pinned red at that head.

`RunBoundChecked = FALSE` drops the reader's refusal of a first range length
above the reconstructed largest. TLC's trace (one worker, the shortest) is
three states: `highestSent = 4`, nothing opened; `Forge(1, 2, 0)` puts wire
largest 1, first range length 2, no ranges on the wire; `ReadBlock`
reconstructs wire 1 against 4 to 1 (candidates 1 and 9, distances 3 and 5)
and, with no bound, reads it nonempty with largest 1: a run of two ending at
1 names sequence 0, which no session sends. Replayed against
sdk/rocketnet_ack.cpp `rocketnet_ack_block_read` by hand: the first range
length is not zero, so the empty test is passed by;
`rocketnet_reconstruct_ack_largest( 1, 4 )` is 1, and
`first_range_length > largest` (2 > 1) returns 0, so the code refuses the
block that the broken reader accepts.
