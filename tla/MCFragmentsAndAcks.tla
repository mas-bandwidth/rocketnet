------------------------- MODULE MCFragmentsAndAcks -------------------------
(***************************************************************************)
(* The small instance of FragmentsAndAcks.  One sender, one receiver, one  *)
(* reassembly slot, four sequences, two fragmented payloads at most, both   *)
(* links "arbitrary" with duplication and reordering and no jitter.  The   *)
(* committed cfg is the honest sender under the code's rules checked for   *)
(* safety (SpecSafety, the nine invariants and CountPinned);               *)
(* MCFragmentsAndAcksLive.cfg is the same instance under Spec with the     *)
(* liveness property.  README-FragmentsAndAcks.md gives every other run    *)
(* (the findings, the defective sender, the six reversed witnesses, the    *)
(* fairness record) as edits to the committed cfg's lines.                 *)
(*                                                                         *)
(* SpecSafety drops the fairness for the safety runs; SpecLinkFair is the  *)
(* run that records why Spec does not use Link!Fairness.                   *)
(***************************************************************************)
EXTENDS FragmentsAndAcks

SpecSafety == Init /\ [][Next]_vars

\* The run that shows why Spec states link fairness per link: with Link's
\* per-packet strong fairness TLC 2.19 reports every temporal property a
\* tautology at this packet-set size (README-FragmentsAndAcks.md).
SpecLinkFair == Init /\ [][Next]_vars /\ Fairness /\ D2R!Fairness /\ R2D!Fairness

=============================================================================
