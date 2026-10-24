------------------------- MODULE MCFragmentsAndAcks -------------------------
(***************************************************************************)
(* The small instance of FragmentsAndAcks.  One sender, one receiver, one  *)
(* reassembly slot, four sequences, two fragmented payloads at most, both   *)
(* links "arbitrary" with duplication and reordering and no jitter.  The   *)
(* committed cfg is the honest sender under the code's rules checked for   *)
(* safety (SpecSafety, the thirteen invariants and CountPinned);           *)
(* MCFragmentsAndAcksLive.cfg is the same instance under Spec with the     *)
(* liveness property.  Every other run (the findings, the defective       *)
(* sender, the reversed witnesses) is its own root cfg, named once in     *)
(* tla/CASES.tsv; README-FragmentsAndAcks.md maps the runs to the cfgs.   *)
(*                                                                         *)
(* SpecSafety drops the fairness for the safety runs; SpecLinkFair is the  *)
(* probe that records why Spec does not use Link!Fairness (TLC refuses    *)
(* it as a tautology, so it has no case in tla/CASES.tsv).                *)
(***************************************************************************)
EXTENDS FragmentsAndAcks

SpecSafety == Init /\ [][Next]_vars

\* The run that shows why Spec states link fairness per link: with Link's
\* per-packet strong fairness TLC 2.19 reports every temporal property a
\* tautology at this packet-set size (README-FragmentsAndAcks.md).
SpecLinkFair == Init /\ [][Next]_vars /\ Fairness /\ D2R!Fairness /\ R2D!Fairness

\* Behaviors that never send an ack. An ack step does not change
\* reassembly, and the batch-full schedule sends none, so this keeps
\* every fragment schedule and drops the ack interleaving.
AckLinkIdle == r2dSent = 0

=============================================================================
