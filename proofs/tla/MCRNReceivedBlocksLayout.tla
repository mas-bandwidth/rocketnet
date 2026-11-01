-------------------- MODULE MCRNReceivedBlocksLayout --------------------
(***************************************************************************)
(* The small instance of RNReceivedBlocksLayout: four blocks of two      *)
(* slots, a ring of 8 and an older half of 4, sequences in any order and *)
(* two payloads, so generations retag across the ring end, base and top  *)
(* leave their generations together and apart, and duplicates replace.   *)
(* MCRNReceivedBlocksLayout.cfg is the passing control; each             *)
(* MCRNReceivedBlocksLayoutBroken*.cfg must violate the invariant it     *)
(* names (README-RNReceivedBlocksLayout.md).                             *)
(***************************************************************************)
EXTENDS RNReceivedBlocksLayout

MCSpec == Spec

=============================================================================
