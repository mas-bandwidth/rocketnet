------------------------- MODULE MCRNOrderingWrap -------------------------
(***************************************************************************)
(* The small instance of RNOrderingWrap: W = 2, a wire ordering modulo 8,  *)
(* a read counter modulo 32 (so it passes the wire wrap three times and    *)
(* wraps itself) and a delivery table of two.  MCRNOrderingWrap.cfg is the *)
(* passing control; MCRNOrderingWrapBrokenUnmasked.cfg compares the masked *)
(* ordering with the unmasked counter in the reservation and must violate  *)
(* NoExhaust (README-RNOrderingWrap.md).                                   *)
(***************************************************************************)
EXTENDS RNOrderingWrap

MCSpec == Spec

=============================================================================
