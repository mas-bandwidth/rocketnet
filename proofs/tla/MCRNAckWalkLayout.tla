-------------------- MODULE MCRNAckWalkLayout --------------------
(***************************************************************************)
(* The small instances of RNAckWalkLayout: a sent ring of six slots in    *)
(* three words of two bits, every reconciled pair of bitmaps, every run  *)
(* the block may name up to two past the highest sent (Part = "run"),    *)
(* and every slot state with every in-window range (Part = "lowest").    *)
(* MCRNAckWalkLayout.cfg and MCRNAckWalkLayoutLowest.cfg are the passing *)
(* controls; each MCRNAckWalkLayoutBroken*.cfg must violate the          *)
(* invariant it names (README-RNAckWalkLayout.md).                       *)
(***************************************************************************)
EXTENDS RNAckWalkLayout

MCSpec == Spec

=============================================================================
