----------------------- MODULE MCRNSequencedHoldPool -----------------------
(***************************************************************************)
(* The small instance of RNSequencedHoldPool: two holds and two channels,  *)
(* so the free stack empties, both holds can be delivered in one update    *)
(* and a push past the stack lands on the delivered list.                  *)
(* MCRNSequencedHoldPool.cfg is the passing control; each                  *)
(* MCRNSequencedHoldPoolBroken*.cfg turns one rule off and must violate    *)
(* the invariant it names (README-RNSequencedHoldPool.md).                 *)
(***************************************************************************)
EXTENDS RNSequencedHoldPool

MCSpec == Spec

=============================================================================
