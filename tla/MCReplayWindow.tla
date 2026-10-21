-------------------------- MODULE MCReplayWindow --------------------------
(***************************************************************************)
(* The small instance that checks ReplayWindow.  A ring of 6 words of 2   *)
(* bits (W = 12) over sequences 1..36, three windows deep, so the long     *)
(* skip of the fix card's test (fill a window, jump two windows) and every *)
(* shorter one are reachable, on a mapping that starts without a slot.     *)
(*                                                                         *)
(* The committed run is the rule: ClearedWords = Words (a long skip clears *)
(* the whole ring) and GateBeforeAdvance = TRUE (the server checks the     *)
(* slot before it advances).  Every invariant passes.                      *)
(*                                                                         *)
(* The reversed witnesses are the same file with one constant changed to   *)
(* what the code does today, and TLC must go red:                          *)
(*                                                                         *)
(*   ClearedWords = 4       sdk/rocketnet_replay.cpp:56-59 clears          *)
(*                          window[0..3]; WindowIsTruth, GenuineIsAccepted *)
(*                          and AckReportsOnlyReceived are violated.       *)
(*   GateBeforeAdvance = FALSE                                             *)
(*                          sdk/rocketnet_server_protocol.cpp:962 advances *)
(*                          before :967 checks the slot;                   *)
(*                          NoAdvanceBeforeConnect is violated, and so is  *)
(*                          AckReportsOnlyProcessed: the ack block names   *)
(*                          a packet nothing processed.                    *)
(*   GateBeforeOpen = FALSE                                                *)
(*                          the order before 2026-10-21: the AEAD open     *)
(*                          runs before the slot check, so a pending       *)
(*                          mapping spends an open on every session        *)
(*                          packet; NoOpenBeforeConnect is violated        *)
(*                          (MCReplayWindowBrokenOpenBeforeGate.cfg).      *)
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
