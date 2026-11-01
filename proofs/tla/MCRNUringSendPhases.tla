-------------------- MODULE MCRNUringSendPhases --------------------
(***************************************************************************)
(* The small instance of RNUringSendPhases: phases of two slots, four     *)
(* updates of up to two sends each.  MCRNUringSendPhases.cfg (any         *)
(* completion may wait any number of calls) and                           *)
(* MCRNUringSendPhasesLateReap.cfg (every completion reaped by the next   *)
(* receive call) are the passing controls; each                           *)
(* MCRNUringSendPhasesBroken*.cfg must violate the invariant it names     *)
(* (README-RNUringSendPhases.md).                                         *)
(***************************************************************************)
EXTENDS RNUringSendPhases

MCSpec == Spec

=============================================================================
