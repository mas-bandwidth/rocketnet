--------------------- MODULE MCRNServerMaintenanceSweep ---------------------
(***************************************************************************)
(* The small instance of RNServerMaintenanceSweep: four slots, slots 1    *)
(* and 2 unreachable, room for one or two frames an update.               *)
(* MCRNServerMaintenanceSweep.cfg is the rule; MCRNServerMaintenance      *)
(* SweepBrokenBreak.cfg breaks on an unreachable refusal and must fail    *)
(* BoundedWait.  README-RNServerMaintenanceSweep.md gives the trace and   *)
(* the check against the code by hand.                                    *)
(***************************************************************************)
EXTENDS RNServerMaintenanceSweep
=============================================================================
