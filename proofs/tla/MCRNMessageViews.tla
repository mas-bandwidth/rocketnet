------------------------- MODULE MCRNMessageViews -------------------------
(***************************************************************************)
(* The small instance of RNMessageViews: a delivery table of three entries *)
(* and a batch of two, so a batch and a single receive share one update    *)
(* and the release search wraps.  MCRNMessageViews.cfg is the passing      *)
(* control; each MCRNMessageViewsBroken*.cfg turns one rule off and must   *)
(* violate the invariant it names (README-RNMessageViews.md).              *)
(***************************************************************************)
EXTENDS RNMessageViews

MCSpec == Spec

=============================================================================
