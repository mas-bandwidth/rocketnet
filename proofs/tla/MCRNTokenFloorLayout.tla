------------------- MODULE MCRNTokenFloorLayout -------------------
(***************************************************************************)
(* The small instance of RNTokenFloorLayout: three slots, four tokens, the *)
(* clock to 4 and expiries up to 3 past it.  MCRNTokenFloorLayout.cfg is   *)
(* the passing control; each MCRNTokenFloorLayoutBroken*.cfg must violate  *)
(* the invariant it names.                                                 *)
(***************************************************************************)
EXTENDS RNTokenFloorLayout

MCSpec == Spec

=============================================================================
