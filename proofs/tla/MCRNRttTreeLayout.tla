-------------------- MODULE MCRNRttTreeLayout --------------------
(***************************************************************************)
(* The small instance of RNRttTreeLayout: a history of 8 (a tree three     *)
(* levels deep), written samples 0 (not positive), 1 and 2 over the        *)
(* reset's -1, and NONE as 9.  MCRNRttTreeLayout.cfg is the passing        *)
(* control; each MCRNRttTreeLayoutBroken*.cfg must violate the invariant   *)
(* it names (README-RNRttTreeLayout.md).                                   *)
(***************************************************************************)
EXTENDS RNRttTreeLayout

MCSamples == {0, 1, 2}

MCSpec == Spec

=============================================================================
