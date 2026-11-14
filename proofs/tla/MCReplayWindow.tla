-------------------------- MODULE MCReplayWindow --------------------------
(***************************************************************************)
(* The small instance that checks ReplayWindow.  A ring of 6 words of 2   *)
(* bits (W = 12) over sequences 1..36, three windows deep, so the long     *)
(* skip of the fix card's test (fill a window, jump two windows) and every *)
(* shorter one are reachable, on a mapping that starts without a slot.     *)
(*                                                                         *)
(* The committed run is the rule: ClearedWords = Words (a long skip clears *)
(* the whole ring), GateBeforeAdvance = TRUE and GateBeforeOpen = TRUE (the *)
(* server checks the slot before the open and before it advances),         *)
(* MarkOnAdvance = TRUE (the advance sets the sequence's bit) and          *)
(* RefuseBelowFloor = TRUE (anything at or below highest - W is too old)   *)
(* and PayloadAtAccept = TRUE (a payload enters accepted_session when it   *)
(* is accepted).  Every invariant passes.  The wide ring explores the      *)
(* ack-eligible kind alone (accepted_session follows replay_session);      *)
(* MCReplayWindowAccept.cfg is the accept instance, all three kinds on a   *)
(* ring of 2 words of 2 bits over sequences 1..12.                         *)
(*                                                                         *)
(* The reversed witnesses are the same file with a constant changed to the *)
(* defective order the code no longer uses, and TLC must go red:           *)
(*                                                                         *)
(*   ClearedWords = 4       rocketnet_replay_advance in                 *)
(*                          sdk/rocketnet_replay.cpp clearing only         *)
(*                          window[0..3]; WindowIsTruth, GenuineIsAccepted *)
(*                          (MCReplayWindowBrokenGenuine.cfg) and          *)
(*                          AckReportsOnlyReceived                         *)
(*                          (MCReplayWindowBrokenAck.cfg) are violated.    *)
(*   GateBeforeAdvance = FALSE with GateBeforeOpen = FALSE                 *)
(*                          rocketnet_server_process_sealed_packet in      *)
(*                          sdk/rocketnet_server_protocol.cpp opening and  *)
(*                          advancing before it checks the slot; the code  *)
(*                          gates before the open, so this is not its      *)
(*                          order; NoAdvanceBeforeConnect is violated      *)
(*                          (MCReplayWindowBrokenGate.cfg).                *)
(*   PayloadAtAccept = FALSE                                               *)
(*                          a payload marked in accepted_session as soon   *)
(*                          as it opens, before anything accepts it;       *)
(*                          AckReportsOnlyProcessed is violated            *)
(*                          (MCReplayWindowBrokenPayloadOnOpen.cfg, on the *)
(*                          accept instance).                              *)
(*   GateBeforeOpen = FALSE                                                *)
(*                          the order before 2026-10-21: the AEAD open     *)
(*                          runs before the slot check, so a pending       *)
(*                          mapping spends an open on every session        *)
(*                          packet; NoOpenBeforeConnect is violated        *)
(*                          (MCReplayWindowBrokenOpenBeforeGate.cfg).      *)
(*   MarkOnAdvance = FALSE  the advance moves highest_opened but does not  *)
(*                          set its bit; ReceivedIsRefused is violated     *)
(*                          (MCReplayWindowBrokenReceived.cfg) and         *)
(*                          HighestIsReceived                              *)
(*                          (MCReplayWindowBrokenHighest.cfg).             *)
(*   RefuseBelowFloor = FALSE                                              *)
(*                          the too-old check is omitted, so a sequence    *)
(*                          below the floor is read from the ring;         *)
(*                          BelowFloorIsRefused is violated                *)
(*                          (MCReplayWindowBrokenBelowFloor.cfg).          *)
(*                                                                         *)
(* README-ReplayWindow.md gives each witness's cfg lines and the trace     *)
(* TLC printed, replayed against the code by hand.                         *)
(***************************************************************************)
EXTENDS ReplayWindow

\* The instance ends when the window is full: highest = MaxSeq and every
\* sequence in range has opened.  The code's window never ends (sequences
\* are 64-bit); the bound is the instance's.  Terminating keeps TLC from
\* reporting that end as a deadlock.
Full == highest = MaxSeq /\ \A s \in Seqs : s > highest \/ AlreadyReceived(s)

Terminating == Full /\ UNCHANGED vars

MCNext == Next \/ Terminating

MCSpec == Init /\ [][MCNext]_vars

=============================================================================
