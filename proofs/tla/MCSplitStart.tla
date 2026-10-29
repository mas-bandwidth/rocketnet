---------------------------- MODULE MCSplitStart ----------------------------
(***************************************************************************)
(* The small instance of SplitStart: two shards, windows of 3 ticks, and   *)
(* messages that take 1 or 2 ticks (a loss and its resend).               *)
(* MCSplitStart.cfg is the committed run with the one-deadline rule.       *)
(* MCSplitStartBrokenPerPhase.cfg is the rule it replaced and              *)
(* MCSplitStartBrokenNoRun.cfg the rule without the RunTimeout term: both  *)
(* reversed witnesses of ServerIsTheAuthority.  MCSplitStartCompletes.cfg  *)
(* is the reversed witness that every shard can reach RUN.                *)
(***************************************************************************)
EXTENDS SplitStart
=============================================================================
