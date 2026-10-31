-------------------------- MODULE MCRNSendRotation --------------------------
(***************************************************************************)
(* The small instances of RNSendRotation: three connections.              *)
(* MCRNSendRotation.cfg is the passing control for the write pass (three  *)
(* frames a flush, stalls and budget refusals); MCRNSendRotationFair.cfg  *)
(* the passing control for the rotation (one frame a flush, five          *)
(* flushes).  Each MCRNSendRotationBroken*.cfg turns one rule off and     *)
(* must violate the invariant it names (README-RNSendRotation.md).        *)
(***************************************************************************)
EXTENDS RNSendRotation

MCSpec == Spec

=============================================================================
