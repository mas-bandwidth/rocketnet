----------------------- MODULE MCRNReceiveAllowance -----------------------
(***************************************************************************)
(* The small instance that checks RNReceiveAllowance. Every run is this   *)
(* module with MCRNReceiveAllowance.cfg, or that file with one constant   *)
(* flipped as README-RNReceiveAllowance.md lists:                         *)
(*   MCRNReceiveAllowanceBrokenNoRefill.cfg   Refill   UnderRateNeverRefused *)
(*   MCRNReceiveAllowanceBrokenSmallCap.cfg   CapFull  UnderRateNeverRefused *)
(*   MCRNReceiveAllowanceBrokenShared.cfg     PerClient Isolated          *)
(*   MCRNReceiveAllowanceBrokenTimeBack.cfg   Monotone NoMint             *)
(***************************************************************************)
EXTENDS RNReceiveAllowance

=============================================================================
