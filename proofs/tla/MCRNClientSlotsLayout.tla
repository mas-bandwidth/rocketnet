------------------------ MODULE MCRNClientSlotsLayout ------------------------
(***************************************************************************)
(* The small instance of RNClientSlotsLayout: three slots, two client ids  *)
(* (so the same id can return to the slot it left while the other holds   *)
(* another), five fresh generations.  MCRNClientSlotsLayout.cfg is the     *)
(* passing control; each MCRNClientSlotsLayoutBroken*.cfg turns one rule   *)
(* off and must violate the invariant it names                             *)
(* (README-RNClientSlotsLayout.md).                                        *)
(***************************************************************************)
EXTENDS RNClientSlotsLayout

MCSpec == Spec

=============================================================================
