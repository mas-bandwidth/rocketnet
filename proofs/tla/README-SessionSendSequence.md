# SessionSendSequence

`SessionSendSequence.tla` models one direction's session send counter,
`send_sequence_session`, in `sdk/rocketnet_server_protocol.cpp` and
`sdk/rocketnet_client_protocol.cpp`. The sequence is the ChaCha20-Poly1305
nonce under one directional session key (`rocketnet_sequence_nonce`), so no
two sealed packets may share one.

Fills take a sequence with one atomic fetch-add and seal later (`FillStart`,
`FillSeal`); a caller may abandon one (`FillAbort`, an environment action).
A control send (keep-alive, standalone ack, disconnect) takes its sequence
with `rocketnet_session_sequence_take` and seals it (`ControlTake`,
`ControlSeal`). The actions interleave freely: the model does not assume the
phase rules that keep control sends and fills apart. The ack baseline
(`rocketnet_*_protocol_highest_sent_session`) is the ack endpoint's
`highest_sequence`, folded from each sealed sequence.

Invariants: `TypeOK`, `SequencesUnique`, `NoZeroSequence` and
`BaselineIsHighestSealed`. Instance: sequences 1..5, two fills, one control
send.

| Case | Expected | Result |
|---|---|---|
| `MCSessionSendSequence.cfg` | pass | 1208 distinct states, every invariant holds |
| `MCSessionSendSequenceBrokenReadIncrement.cfg` | `SequencesUnique` violated | exit 12 |
| `MCSessionSendSequenceBrokenBaseline.cfg` | `BaselineIsHighestSealed` violated | exit 12 |

## Witness traces, replayed against the code by hand

`BrokenReadIncrement` is the control send before 2026-10-29: read
`send_sequence_session`, seal, then `send_sequence_session++`.

1. `ControlTake`: the control send reads 1; the counter stays 1.
2. `FillStart(f1)`: `start_payload` fetch-adds and also gets 1; the counter is 2.
3. `FillSeal(f1)`: sequence 1 sealed.
4. `ControlSeal`: sequence 1 sealed again, the counter incremented to 3.

Two packets under one key at sequence 1. The unit cases
`test_server_control_send_inside_fill_takes_a_distinct_sequence` and
`test_client_control_send_inside_fill_takes_a_distinct_sequence`
(`tests/test_unit_session_send_sequence.cpp`) force step 2 inside the control
send through the DEV sequence-taken seam and failed on this before the fix.

`BrokenBaseline` reads the baseline as `send_sequence_session - 1`.

1. `FillStart(f1)`: a fill takes 1; the counter is 2, nothing is sealed.

The baseline reads 1, a sequence never sent; the highest sealed is 0. The same
unit cases check the baseline between the control seal and the fill seal and
failed on this with the sequence fix alone.
