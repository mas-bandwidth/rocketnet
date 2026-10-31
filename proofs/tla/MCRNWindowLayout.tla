------------------------- MODULE MCRNWindowLayout -------------------------
(***************************************************************************)
(* The small instance of RNWindowLayout: a window of 4 slots (one bit      *)
(* word), a wire modulus of 16 (half range 8) and R bounded at 20, so R    *)
(* crosses the wire wrap and the slot array turns over five times.         *)
(* MCRNWindowLayout.cfg is the passing control; each                       *)
(* MCRNWindowLayoutBroken*.cfg turns one rule off and must violate the     *)
(* invariant it names (README-RNWindowLayout.md).                          *)
(***************************************************************************)
EXTENDS RNWindowLayout

MCSpec == Spec

=============================================================================
