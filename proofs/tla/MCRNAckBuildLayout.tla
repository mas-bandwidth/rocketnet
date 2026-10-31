-------------------- MODULE MCRNAckBuildLayout --------------------
(***************************************************************************)
(* The small instance of RNAckBuildLayout: three words of three bits, a   *)
(* window of 9, every largest to 19 (the window turns over twice), every *)
(* word value with the largest's bit set, and max_ranges 0 to 2.         *)
(* MCRNAckBuildLayout.cfg is the passing control; each                   *)
(* MCRNAckBuildLayoutBroken*.cfg must violate the invariant it names     *)
(* (README-RNAckBuildLayout.md).                                         *)
(***************************************************************************)
EXTENDS RNAckBuildLayout

MCSpec == Spec

=============================================================================
