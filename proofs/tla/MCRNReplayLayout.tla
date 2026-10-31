-------------------- MODULE MCRNReplayLayout --------------------
(***************************************************************************)
(* The small instance of RNReplayLayout: three words of four bits, a ring *)
(* of 12, sequences to 30, so the ring turns over twice and a skipped    *)
(* range can cover a partial first word, a whole word and a partial last *)
(* word, or wrap at the ring end.  MCRNReplayLayout.cfg is the passing   *)
(* control; each MCRNReplayLayoutBroken*.cfg must violate the invariant  *)
(* it names (README-RNReplayLayout.md).                                  *)
(***************************************************************************)
EXTENDS RNReplayLayout

MCSpec == Spec

=============================================================================
