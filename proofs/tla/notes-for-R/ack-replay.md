# Notes for package R from package M1 (ack-replay)

Branch `models/v1.1.x/ack-replay`. Everything below is for the records package
(R) or the code-comment package (C) to apply; M1 did not edit these files.

## Rows for R

### MM004 (records, SessionAcks)

COVERAGE row `session-acks-window` (sdk/rocketnet_ack.cpp, SessionAcks) still
lists `MCSessionAcksLive.cfg` among the passing controls and calls the gate
instance "both links arbitrary with duplication and reordering". RUNS.tsv
records `MCSessionAcksLive.cfg` as exit 124 (CASES marks it bench debt,
`committed-instance-liveness-over-budget`), and `MCSessionAcks.cfg` sets
`Duplication = FALSE`. Suggested text for those two phrases:

- "passing controls MCSessionAcks.cfg (the gate instance: four sequences, burns
  on, both links arbitrary with reordering, duplication off)"
- "MCSessionAcksLive.cfg (the gate instance under Spec; bench debt, over the
  case budget)" moved out of the passing list.

M1 edited the rest of that row (the accepted_session window, RecvAckRepeat,
MCSessionAcksLiveLap.cfg and MCSessionAcksReachLap.cfg) and left these two
phrases as they were.

### RNWrittenRing byte count (cross-family, from MM010)

`rocketnet_ack_endpoint_reset` now also zeroes the cached top pair and the
middle and right aggregates (commit c77fc1eca), so the bytes reset writes in
the reclaimed received-record summary words are 592, not 520:

- `proofs/tla/RNWrittenRing.tla` (the module header's byte count): "base (520 bytes)" becomes "base, top,
  middle and right (592 bytes)".
- `proofs/tla/README-RNWrittenRing.md` (the reset paragraph): "words (520 bytes)" becomes
  "words (592 bytes)".

### DST mirror rows (COVERAGE rows 63 to 65, ReplayWindow, tests/dst_checks.h)

`AckReportsOnlyReceived` and `HighestIsReceived` now read `accepted_session`
(the window the ack block is built from), not `replay_session`. The rows
`dst-invariant-checks-replaywindow-ackreportsonlyreceived` and
`dst-invariant-checks-replaywindow-highestisreceived` say the DST mirrors them
by `acked_means_opened` and `replay_monotone`. Read at the base:
`acked_means_opened` is a sender-side ledger (an acked packet id was issued),
reading neither window, so it mirrors the end-to-end consequence and is
unaffected; `replay_monotone` reads `replay_session.highest_opened`
(tests/dst_checks.h, the replay_monotone check), so row 65 (HighestIsReceived) and row 64 (Monotone)
mirror the replay_session half only, not `accepted_session`. Suggested: say so
in the two notes, or M2 adds the accepted_session mark to `replay_monotone`
(M2's file). `Monotone` now also covers `accHighest`.

## Code-comment re-citations for package C (comment text only)

- `sdk/rocketnet_ack.cpp` (the comment after `rocketnet_ack_block_write`, before `rocketnet_ack_block_build`): "the replay window is the received state the
  block is built from: bits are meaningful for sequences in (highest_opened -
  window, highest_opened] ..." becomes "accepted_session is the received state
  the block is built from (rocketnet_session_ack_block_build passes it): bits
  are meaningful for sequences in (highest_opened - window, highest_opened] ...".
- `sdk/rocketnet_ack.cpp` (the SendAck citation above `rocketnet_ack_block_build`): "proofs/tla/SessionAcks.tla SendAck: builds the
  block the session packet carries from the replay window" becomes
  "proofs/tla/SessionAcks.tla SendAck: builds the block the session packet
  carries from accepted_session".
- `sdk/rocketnet_server_protocol.cpp` (above
  `rocketnet_server_protocol_accept_payload`) and
  `sdk/rocketnet_client_protocol.cpp` (above
  `rocketnet_client_protocol_accept_payload`): add
  "// proofs/tla/ReplayWindow.tla Accept: a delivered payload enters
  accepted_session unless that window already refuses it." Once one of them is
  in a current ReplayWindow row's code file (add a COVERAGE row
  `replay-window-accept` on sdk/rocketnet_server_protocol.cpp, module
  ReplayWindow, current, note "Accept advances accepted_session for an
  accepted payload."), drop "and Accept" from the "Environment actions Forge
  and Accept." sentence of row `replay-window`.
- `sdk/rocketnet_server_protocol.cpp` and
  `sdk/rocketnet_client_protocol.cpp` (the `rocketnet_session_ack_eligible_on_open` branch, the open-path advance of
  accepted_session): add "// proofs/tla/ReplayWindow.tla Receive and
  OpenNeverAcceptedFirst: an ack-eligible type is advanced into
  accepted_session here, unchecked; replay_session's check above is what keeps
  rocketnet_replay_advance's assert true."
- `tests/test_unit_replay_trace.cpp` (`replay_window_ref_t` and its step functions): the mirror of the model's
  CONSTANTS and VARIABLES now lacks `PayloadAtAccept`, `Kinds`, `accHighest`,
  `accWindow`, `accepted`, `delivered`, and `Receive` takes a kind. The test is
  still right for `replay_session` (the wide-ring runs' `Kinds = {"eligible"}`);
  its comments should say it mirrors the replay_session half at kind
  "eligible", or the mirror grows the second window.

## README and inventory lines for R

- `proofs/tla/README.md` (the case list, if it names cfgs) and
  `docs/TLA-INVENTORY.md`: new cases `MCReplayWindowAccept.cfg`,
  `MCReplayWindowBrokenPayloadOnOpen.cfg`, `MCSessionAcksLiveLap.cfg`,
  `MCSessionAcksReachLap.cfg`, `MCRNClientIoDecryptReachTooOld.cfg`,
  `MCRNClientIoDecryptReachCountedFailure.cfg`,
  `MCRNClientIoDecryptBrokenNoRetireLive.cfg`,
  `MCRNClientIoDecryptBrokenSharedWindowLive.cfg`,
  `MCRNAckApplyPhaseOneBlock.cfg`, `MCRNAckApplyPhaseBrokenDuplicateSurfaces.cfg`
  and `MCAckBlockWrapBrokenRunBound.cfg` (MA003);
  removed `MCReplayWindowBrokenProcessed.cfg`.
- One-line summaries: ReplayWindow models two windows per mapping,
  replay_session and accepted_session, and the ack block reads the second;
  SessionAcks builds the block from accepted_session; RNClientIoDecrypt zeroes
  its windows at the retire and the fill threads read accepted_session.

## Roadmap entries (sexp text for docs/roadmap.sexp)

```
  (item "session-acks-deferred-accept" :group "after-v1-1-landing" :area "proofs"
   :title "SessionAcks with deferred payload acks"
   :text "proofs/tla/SessionAcks.tla takes the payload acks as not deferred: RecvData advances replay_session and accepted_session in one step, so the two windows hold the same set. With defer_payload_acks on, a payload enters accepted_session only when the application calls accept (rocketnet_server_protocol_accept_payload, rocketnet_client_protocol_accept_payload), later or never, so the block names it late or not at all. ReplayWindow's Accept checks the window side of that order; SessionAcks would add an Accept action between RecvData and SendAck and check OpenedSurfacesOrLaps with the antecedent narrowed to accepted payloads."
   :date "2026-11-13" :origin "Models review MM001, the accepted_session window")
  (item "client-io-decrypt-accept-window" :group "after-v1-1-landing" :area "proofs"
   :title "RNClientIoDecrypt: packets refused after the open, and deferred accept"
   :text "proofs/tla/RNClientIoDecrypt.tla advances accepted_session (accWin) beside replay_session (mainWin) for every opened record update accepts, so accWin follows mainWin. The code leaves a packet out of accepted_session when the ack block read or the opened-size check refuses it after the open, and a payload until it is accepted (later, with deferred acks). Neither is modelled; the fill threads' read of accWin is correct for the modelled behaviours only. Add the post-open refusal and a deferred Accept, and check that accWin is contained in mainWin and the ack mark never exceeds the accepted mark."
   :date "2026-11-13" :origin "Models review MM001 and MM005")
```
