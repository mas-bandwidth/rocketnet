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
(* arrivals and 1 transport loss.  The two reversed witnesses:             *)
(*   MCRNSendBudgetBrokenOverspend.cfg        CheckBudget   NeverOverspend *)
(*   MCRNSendBudgetBrokenUnreliableFirst.cfg  ReliableFirst                *)
(*                                     EveryReliableEventuallySent         *)
(***************************************************************************)
EXTENDS RNSendBudget

\* every safety invariant at once, for a run that wants one name
SafetyInvariants ==
    /\ TypeOK /\ NeverOverspend /\ ReliableNeverDropped
    /\ UnreliableDroppedOnlyOverBudget /\ ClassOrder
    /\ UncappedNeverDropsOrWaits

=============================================================================
