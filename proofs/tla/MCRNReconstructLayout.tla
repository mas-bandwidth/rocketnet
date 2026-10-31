-------------------- MODULE MCRNReconstructLayout --------------------
(***************************************************************************)
(* The small instance of RNReconstructLayout: a wire modulus of 8 in a    *)
(* space of 32, every expected value and every wire value, so the first *)
(* lap (no candidate below) and the last (the candidate above wraps) are *)
(* both checked.  MCRNReconstructLayout.cfg is the passing control; each *)
(* MCRNReconstructLayoutBroken*.cfg must violate the invariant it names  *)
(* (README-RNReconstructLayout.md).                                      *)
(***************************************************************************)
EXTENDS RNReconstructLayout

MCSpec == Spec

=============================================================================
