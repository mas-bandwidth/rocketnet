------------------------- MODULE MCRNSentRingLayout -------------------------
(***************************************************************************)
(* The small instance of RNSentRingLayout: three sent-packet entries, up  *)
(* to two references a packet, a receipt ring of three, serials modulo 4. *)
(* MCRNSentRingLayout.cfg is the passing control with a reference ring of *)
(* four, below the code's ratio, so the reference test binds;             *)
(* MCRNSentRingLayoutCodeRatio.cfg sizes it at the code's ratio (R * P)   *)
(* and checks that the reference test then never fires.  Each             *)
(* MCRNSentRingLayoutBroken*.cfg turns one rule off and must violate the  *)
(* invariant it names (README-RNSentRingLayout.md).                       *)
(***************************************************************************)
EXTENDS RNSentRingLayout

MCSpec == Spec

=============================================================================
