-------------------- MODULE MCRNReceivedBlocksLayout --------------------
(***************************************************************************)
(* The small instance of RNReceivedBlocksLayout: four blocks of two      *)
(* slots, a ring of 8 and an older half of 4, sequences to 18 in any     *)
(* order, so retention moves across the ring end and query pieces start  *)
(* and end inside blocks.  MCRNReceivedBlocksLayout.cfg is the passing   *)
(* control; each MCRNReceivedBlocksLayoutBroken*.cfg must violate the    *)
(* invariant it names (README-RNReceivedBlocksLayout.md).               *)
(***************************************************************************)
EXTENDS RNReceivedBlocksLayout

MCSpec == Spec

=============================================================================
