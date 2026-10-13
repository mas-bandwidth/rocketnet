------------------------- MODULE MCFragmentsAndAcks -------------------------
(***************************************************************************)
(* The small instance of FragmentsAndAcks.  One sender, one receiver, one  *)
(* reassembly slot, eight sequences, four fragmented payloads at most,     *)
(* both links under a pattern with duplication, reordering and one tick of *)
(* jitter.  The committed cfg is the honest sender under the code's rules  *)
(* and "arbitrary" loss; README-FragmentsAndAcks.md gives every other run  *)
(* (the four deterministic patterns, the defective sender, the two         *)
(* findings and the six reversed witnesses) as edits to its lines.         *)
(*                                                                         *)
(* SpecSafety drops the fairness for the safety-only runs, where the       *)
(* defective sender's packet set would make the link's per-packet strong   *)
(* fairness expensive for nothing.                                         *)
(***************************************************************************)
EXTENDS FragmentsAndAcks

SpecSafety == Init /\ [][Next]_vars

\* The run that shows why Spec states link fairness per link: with Link's
\* per-packet strong fairness TLC 2.19 reports every temporal property a
\* tautology at this packet-set size (README-FragmentsAndAcks.md).
SpecLinkFair == Init /\ [][Next]_vars /\ Fairness /\ D2R!Fairness /\ R2D!Fairness

=============================================================================
