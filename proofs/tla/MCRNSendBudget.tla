--------------------------- MODULE MCRNSendBudget ---------------------------
(***************************************************************************)
(* The small instance that checks RNSendBudget.  Every run is this module  *)
(* with MCRNSendBudget.cfg, or that file with one constant flipped as      *)
(* README-RNSendBudget.md lists.                                           *)
(*                                                                         *)
(* The committed cfg is the design's rule set (CheckBudget and             *)
(* ReliableFirst TRUE) with a window of W = 2 ticks, a refill of 1 byte    *)
(* per tick (Budget = 2 per window), a bucket of Depth = 2 (one maximum    *)
(* packet), reliable items of 2 bytes (larger than one tick's refill),     *)
(* unreliable items of 1 byte arriving up to 2 per update, 2 reliable      *)
(* arrivals, 1 transport loss and 1 refused finish (MaxFail, refunded      *)
(* whole: RefundOnFail TRUE).  The reversed witnesses:                     *)
(*   MCRNSendBudgetBrokenOverspend.cfg        CheckBudget   NeverOverspend *)
(*   MCRNSendBudgetBrokenUnreliableFirst.cfg  ReliableFirst                *)
(*                                     EveryReliableEventuallySent         *)
(*   MCRNSendBudgetBrokenUncapped.cfg   UncapHonoured <- BrokenUncap       *)
(*                                     UncappedNeverDropsOrWaits           *)
(*   MCRNSendBudgetBrokenNoRefund.cfg   RefundOnFail                       *)
(*                                     FailedSendCostsNothing              *)
(***************************************************************************)
EXTENDS RNSendBudget

\* the reversal MCRNSendBudgetBrokenUncapped.cfg substitutes for UncapHonoured
BrokenUncap == FALSE

\* every safety invariant at once, for a run that wants one name
SafetyInvariants ==
    /\ TypeOK /\ NeverOverspend /\ ReliableNeverDropped
    /\ UnreliableDroppedOnlyOverBudget /\ ClassOrder
    /\ UncappedNeverDropsOrWaits /\ FailedSendCostsNothing

=============================================================================
