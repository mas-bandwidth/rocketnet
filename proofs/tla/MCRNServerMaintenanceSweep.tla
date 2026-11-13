--------------------- MODULE MCRNServerMaintenanceSweep ---------------------
(***************************************************************************)
(* The small instance of RNServerMaintenanceSweep: four slots, slot 1     *)
(* unreachable, slot 2 refused with room left, room for one or two frames *)
(* an update.  MCRNServerMaintenanceSweep.cfg is the rule;                *)
(* MCRNServerMaintenanceSweepBrokenBreak.cfg breaks on an unreachable     *)
(* refusal and MCRNServerMaintenanceSweepBrokenRefused.cfg on a refusal   *)
(* with room left; each must fail BoundedWait.                            *)
(* README-RNServerMaintenanceSweep.md gives the traces and the checks     *)
(* against the code by hand.                                              *)
(***************************************************************************)
EXTENDS RNServerMaintenanceSweep
=============================================================================
