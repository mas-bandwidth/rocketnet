---------------------------- MODULE MCSplitStart ----------------------------
(***************************************************************************)
(* The small instance of SplitStart: two shards, windows of 3 ticks, a     *)
(* readiness lease of 5, and messages that take 1 or 2 ticks (a loss and  *)
(* its resend).  The code's lease is 6 times its population window (60 s  *)
(* to 10 s); 5 ticks keeps both endings of the population wait inside     *)
(* MaxTime.  MCSplitStart.cfg is the committed run with the one-deadline   *)
(* rule.  MCSplitStartBrokenPerPhase.cfg is the rule it replaced and       *)
(* MCSplitStartBrokenNoRun.cfg the rule without the RunTimeout term: both  *)
(* reversed witnesses of ServerIsTheAuthority.  MCSplitStartCompletes.cfg  *)
(* is the reversed witness that every shard can reach RUN, and             *)
(* MCSplitStartReadyLease.cfg the one that the lease can end the           *)
(* population wait (PopulationWindowDecides).                              *)
(***************************************************************************)
EXTENDS SplitStart
=============================================================================
