---------------------------- MODULE MCSessionAcks ----------------------------
(***************************************************************************)
(* The small instance of SessionAcks: one sender, one receiver, four       *)
(* sequences, both links "arbitrary" with duplication and reordering and   *)
(* no jitter.  MCSessionAcks.cfg is the code's rules checked for safety    *)
(* (SpecSafety, the eight invariants); MCSessionAcksLive.cfg is the same   *)
(* instance under Spec with the liveness property.  Every other run (the   *)
(* loss patterns, the bitmap instance, the reversed witnesses) is its own  *)
(* root cfg, named once in tla/CASES.tsv; README-SessionAcks.md maps the   *)
(* runs to the cfgs.                                                       *)
(*                                                                         *)
(* SpecSafety drops the fairness for the safety runs.                      *)
(***************************************************************************)
EXTENDS SessionAcks

SpecSafety == Init /\ [][Next]_vars

=============================================================================
