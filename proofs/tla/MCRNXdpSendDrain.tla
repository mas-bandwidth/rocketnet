------------------------- MODULE MCRNXdpSendDrain -------------------------
(***************************************************************************)
(* The small instance of RNXdpSendDrain: a TX ring of 3 entries on indices *)
(* modulo 8, a batch of 2, five frames, a live NIC that pauses at most 2   *)
(* passes, and a stall bound of 3 passes.  MCRNXdpSendDrain.cfg is the     *)
(* design under fairness and must pass NeverAbandonDraining and           *)
(* StopFinishes.  MCRNXdpSendDrainBrokenNoRelease.cfg must fail            *)
(* StopFinishes; MCRNXdpSendDrainBrokenNoReset.cfg,                        *)
(* MCRNXdpSendDrainBrokenNoRing.cfg and MCRNXdpSendDrainBrokenShortBound   *)
(* .cfg must fail NeverAbandonDraining.                                    *)
(***************************************************************************)
EXTENDS RNXdpSendDrain
=============================================================================
