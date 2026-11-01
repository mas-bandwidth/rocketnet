--------------------- MODULE MCRNXdpReplayMarksLayout ---------------------
(***************************************************************************)
(* The small instance of RNXdpReplayMarksLayout: a buffer of three         *)
(* entries (one in the capacity controls), two keys and two values.        *)
(* Kept forgets and marks and stale cells are reachable; capacity-one     *)
(* controls also reach refusals. MCRNXdpReplayMarksLayout.cfg is the      *)
(* passing control; each MCRNXdpReplayMarksLayoutBroken*.cfg turns one     *)
(* rule off and must violate the invariant it names                        *)
(* (README-RNXdpReplayMarksLayout.md).                                     *)
(***************************************************************************)
EXTENDS RNXdpReplayMarksLayout

CONSTANTS k1, k2

MCSpec == Spec

=============================================================================
