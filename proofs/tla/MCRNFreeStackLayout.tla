-------------------- MODULE MCRNFreeStackLayout --------------------
(***************************************************************************)
(* The small instance of RNFreeStackLayout: capacity 3, map generations  *)
(* to 4, owner generations 1 and 2.  TakeMap, TakeSlots, PeekClaim,    *)
(* PopClaim and Release interleave on one stack.                        *)
(* MCRNFreeStackLayout.cfg is the passing                               *)
(* control; each MCRNFreeStackLayoutBroken*.cfg must violate the         *)
(* invariant it names (README-RNFreeStackLayout.md).                     *)
(***************************************************************************)
EXTENDS RNFreeStackLayout

MCSpec == Spec

=============================================================================
