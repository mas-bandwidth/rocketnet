----------------------- MODULE MCRNReceiveAllowance -----------------------
(***************************************************************************)
(* The small instances that check RNReceiveAllowance. Every run is this   *)
(* module with one of three passing files, or one of them with one        *)
(* constant flipped as README-RNReceiveAllowance.md lists. The bytes:     *)
(* MCRNReceiveAllowance.cfg, and                                          *)
(*   MCRNReceiveAllowanceBrokenNoRefill.cfg   Refill   UnderRateNeverRefused *)
(*   MCRNReceiveAllowanceBrokenSmallCap.cfg   CapFull  UnderRateNeverRefused *)
(*   MCRNReceiveAllowanceBrokenShared.cfg     PerClient Isolated          *)
(*   MCRNReceiveAllowanceBrokenTimeBack.cfg   Monotone NoMint             *)
(* The credits for a correct client: MCRNReceiveAllowanceCredit.cfg, and  *)
(*   MCRNReceiveAllowanceBrokenNoAckCredit.cfg AckCredit UnderRateNeverRefused *)
(*   MCRNReceiveAllowanceBrokenNoControl.cfg  Control  UnderRateNeverRefused *)
(* The credits against a flooder: MCRNReceiveAllowanceFlood.cfg, and      *)
(*   MCRNReceiveAllowanceBrokenFreeAcks.cfg   CreditBound AcksBounded     *)
(*   MCRNReceiveAllowanceBrokenServerAckCredits.cfg ServerAckSpends AcksBounded *)
(***************************************************************************)
EXTENDS RNReceiveAllowance

=============================================================================
