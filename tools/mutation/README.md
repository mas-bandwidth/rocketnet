# Mutation gate — parser, replay, ack, fragment and message core

The scheduled mutation run for the wire-facing core (RN-CORRECTNESS
2026-10-14, R4). It generalises the single hand-written mutant in
`tools/check_fuzz_oracle.py` into a real LLVM-IR mutation pass: Mull mutates
the five files below, `./bin/test` tries to kill each mutant, and the
surviving count per file is the floor this gate holds.

## Files under mutation

- `sdk/rocketnet_base64.cpp` — the strict decoder (read path)
- `sdk/rocketnet_replay.cpp` — the replay-protection window
- `sdk/rocketnet_ack.cpp` — the general ack block
- `sdk/rocketnet_fragment.cpp` — fragmentation and reassembly
- `sdk/rocketnet_message.cpp` — the message record parser

## Running it

    bash tools/mutation/run_mull.sh

On a bench with Mull it prints the per-file killed/survived counts and writes
`tools/mutation/floor.tsv`. The script exits non-zero when a mutant survives
that the floor had killed (a suite regression), and it shrinks the floor
when a run kills more. On a bench without Mull it refuses loudly (exit 3) and
writes nothing — an absent tool is not a green gate.

Mull is the tool the card names. If the bench carries another LLVM-IR
mutation tool, set `MULL_RUNNER` to its entry point; its report must still
carry one `file:line:col: warning: (Killed|Survived):` line per mutant.

## The floor

`floor.tsv` records the surviving-mutant count per file on BASE, one row per
file, columns `file`, `killed`, `survived`. A row whose `survived` cell is
`-` has no floor yet: the first run on a Mull bench establishes it. A run
with more survivors than the floor fails; a run with fewer survivors shrinks
the floor in the same pull request.

The floor on this branch is unpopulated: the bench this card ran on carries
no LLVM-IR mutation tool (Mull, and every other mutation tool checked, is
absent), so no killed/survived counts were produced. Populate it by running
the gate on a bench that has Mull.

## Findings: surviving mutants on boundary constants

The mutators this gate runs (`cxx_gt_to_ge`, `cxx_ge_to_gt`, `cxx_lt_to_ge`,
`cxx_eq_to_ne`, `cxx_rem_to_div`, the constant-replacement and
branch-removal families) are aimed at the boundary constants the card names
— 4096, 64, the word counts — and at the comparison edges around them. Each
surviving mutant is recorded here with the line, the mutation, and the test
that should have killed it. The boundary edges the run probes:

- `sdk/rocketnet_base64.cpp:103` — `( input_length % 4 ) != 0`: the
  four-character-group rule; a mutant that drops the remainder check admits
  non-canonical spellings. Killed by `test_base64_*` in `./bin/test`.
- `sdk/rocketnet_base64.cpp:123` — `output_length > output_size`: the decode
  capacity guard. This is the exact site `tools/check_fuzz_oracle.py` mutates
  by hand; a `>`→`>=` mutant makes a full buffer decode one byte past it.
- `sdk/rocketnet_base64.cpp:148` — `bits > 0 && ( accumulator & ... )`: the
  canonical-tail check; dropping it makes two strings decode to one byte
  string (the `+w`/`-w` two-spellings-one-key bug class).
- `sdk/rocketnet_replay.cpp:11-12` — `sequence % ROCKETNET_REPLAY_WINDOW_BITS`
  and `bit / 64`, `bit % 64`: the ring's 64-bit word stride and the word
  count that sizes `window`. A `64`→`63` or a word-count fold reads a
  different word than it writes.
- `sdk/rocketnet_replay.cpp:35` — `highest_opened >= ROCKETNET_REPLAY_WINDOW_BITS`:
  the window-wrapped-ago short-circuit; inverting it returns "already
  received" for fresh sequences near the window edge.
- `sdk/rocketnet_ack.cpp:71` — `block.largest >= ROCKETNET_REPLAY_WINDOW_BITS`:
  the ack block's floor sequence; the same edge on the receive side.
- `sdk/rocketnet_fragment.cpp:18` — `fragment_count >= 2 && fragment_count <=
  ROCKETNET_MAX_FRAGMENTS`: the fragment-count bounds the wire encodes as
  `count - 1`; a `<=`→`<` mutant refuses a valid maximum, a `>= 2`→`> 2`
  admits the invalid single fragment.
- `sdk/rocketnet_fragment.cpp:280` — `fragment_index >= fragment_count`: the
  out-of-range index; inverting it reads past the slot.
- `sdk/rocketnet_message.cpp:94` — `record->channel >= 256`: the WIDE/narrow
  form switch; a `>=`→`>` mutant mis-parses a channel 256 header and shifts
  every following field.
- `sdk/rocketnet_message.cpp:340` — `wide && record->channel < 256`: the
  check that a WIDE form is only emitted when the channel really is wide.

No surviving mutant is listed below on this branch: the bench that ran this
card has no mutation tool, so no report was produced. The first Mull run
records its survivors here, one line each:

    <file>:<line>: <mutation> — <test that should have killed it>

That list is the gate's findings, not its failure mode: a finding means a
boundary edge the suite does not yet hold, and the fix is a test in
`./bin/test` that kills the mutant — never a change to the source it guards.
