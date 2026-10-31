--------------------- MODULE MCRNXdpReplayMarksLayout ---------------------
(***************************************************************************)
(* The small instance of RNXdpReplayMarksLayout: a buffer of three         *)
(* entries (two in the witnesses that need no more), two keys and two      *)
(* values, so kept forgets, kept marks, stale cells past each count and    *)
(* both refusals are all reached.  MCRNXdpReplayMarksLayout.cfg is the     *)
(* passing control; each MCRNXdpReplayMarksLayoutBroken*.cfg turns one     *)
(* rule off and must violate the invariant it names                        *)
(* (README-RNXdpReplayMarksLayout.md).                                     *)
(***************************************************************************)
EXTENDS RNXdpReplayMarksLayout

CONSTANTS k1, k2

MCSpec == Spec

=============================================================================
