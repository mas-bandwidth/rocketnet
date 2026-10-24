# AckBlockWrap

`AckBlockWrap.tla` models the ack block's largest received across the wire
sequence wrap (standard/TRANSPORT.md t15): one direction of one session, the
builder that writes the block and the reader that decides it.

## What is modelled

| Variable | Meaning | Code |
|---|---|---|
| `highestSent` | the reader's highest sent session sequence, a natural from `Start - 1` | `client->send_sequence_session - 1` (sdk/rocketnet_client_protocol.cpp), `mapping->send_sequence_session - 1` (sdk/rocketnet_server_protocol.cpp) |
| `opened` | the sequences the other end has opened | the replay window, `rocketnet_replay_advance` (sdk/rocketnet_replay.cpp) |
| `flight` | the block in flight: wire largest (`highest opened mod Modulus`), first range length, range count, and its ghost truth | the 7-byte block `rocketnet_ack_block_write` lays out |
| `read` | the reader's last decision, with the block's ghost truth | the return of `rocketnet_ack_block_read` |

| Action | Code |
|---|---|
| `Send` | the sender's session sequence advances (environment: the session window bounds it to less than Modulus/2 past any sequence an ack can still name) |
| `Open(s)` | the other end opens sequence s (environment) |
| `WriteBlock` | `rocketnet_ack_block_build` then `rocketnet_ack_block_write` (sdk/rocketnet_ack.cpp): the all-zero block while nothing is opened, else the wire largest and the received run ending at it |
| `Forge(w, c)` | a malformed block on the wire: first range length 0 with a nonzero wire largest or range count (environment) |
| `ReadBlock` | `rocketnet_ack_block_read` (sdk/rocketnet_ack.cpp): empty on the wire fields, refused when the first range length is 0 with anything else nonzero, else the largest reconstructed by `rocketnet_reconstruct_near` (sdk/rocketnet_protocol.cpp; nearest to highest sent, the larger on a tie) and refused when the first range length exceeds it |

Ranges below the first are abstracted (the honest count is 0); their walk,
rules 3-5, is FragmentsAndAcks' subject. Reconstruction takes the nearest of
every candidate sharing the wire value, which picks the same value as the
code's three candidates (base - 2^32, base, base + 2^32).

Invariants: `TypeOK`; `EmptyRecognised` (a block written while nothing is
opened reads empty, whatever the reader's highest sent); `LargestExact` (a
nonempty block reconstructs to the true highest opened); `NeverRefusedHonest`
(a block the builder wrote is never refused); `MalformedRefused` (every forged
block is refused).

## Instance

Modulus 8 (2^32 in the code), Start 5 (at or above Modulus/2, as the DEV seam
`ROCKETNET_DEV_SESSION_SEQUENCE_START` near 2^32 is), MaxSeq 12: the session
crosses the wrap at 8, where a genuine largest of 8 has wire value 0 and a
first range length of at least one.

| Config | Constants | Expected |
|---|---|---|
| `MCAckBlockWrap.cfg` | `BrokenHalfRange = FALSE`, every invariant | pass (55700 distinct states on vision, 2.6 s) |
| `MCAckBlockWrapBrokenHalfRange.cfg` | `BrokenHalfRange = TRUE`, `EmptyRecognised` | exit 12, `EmptyRecognised` violated |

## The reversed witness

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
