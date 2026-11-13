------------------------- MODULE MCRNXdpSendDrain -------------------------
(***************************************************************************)
(* The small instance of RNXdpSendDrain: a TX ring of 3 entries on indices *)
(* modulo 4, a batch of 2, six frames (so both indices wrap), a live NIC   *)
(* that pauses at most 2 passes, and a stall bound of 3 ticks.             *)
(* MCRNXdpSendDrain.cfg is the design under fairness and must pass         *)
(* NeverAbandonDraining and StopFinishes.                                  *)
(* MCRNXdpSendDrainBrokenNoRelease.cfg must fail StopFinishes;             *)
(* MCRNXdpSendDrainBrokenNoReset.cfg, MCRNXdpSendDrainBrokenNoRing.cfg,    *)
(* MCRNXdpSendDrainBrokenNoClaim.cfg and                                   *)
(* MCRNXdpSendDrainBrokenShortBound.cfg (a bound of SlowGap) must fail     *)
(* NeverAbandonDraining.                                                   *)
(***************************************************************************)
EXTENDS RNXdpSendDrain
=============================================================================
