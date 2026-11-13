-------------------------- MODULE MCRNSendRotation --------------------------
(***************************************************************************)
(* The small instances of RNSendRotation, two or three connections.       *)
(* MCRNSendRotation.cfg is the passing control for the write pass (three  *)
(* frames a flush, stalls and budget refusals); MCRNSendRotationShortAck  *)
(* for the window's numbering test; MCRNSendRotationRounds for the        *)
(* writer list; MCRNSendRotationFair.cfg for the rotation (one frame a    *)
(* flush, five flushes); MCRNSendRotationVain.cfg for the vain ordered    *)
(* write.  Each MCRNSendRotationBroken*.cfg turns one rule                *)
(* off and must violate the invariant it names                            *)
(* (README-RNSendRotation.md).                                            *)
(***************************************************************************)
EXTENDS RNSendRotation

MCSpec == Spec

=============================================================================
