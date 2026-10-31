------------------------ MODULE MCRNSendStoreLayout ------------------------
(***************************************************************************)
(* The small instance of RNSendStoreLayout: one store class of three       *)
(* chunks, four send descriptors (one more than the chunks, so both the    *)
(* store's refusal and the descriptor pool's are reachable), generations   *)
(* modulo 3 (so a wrap is reached in two retakes).                         *)
(* MCRNSendStoreLayout.cfg is the passing control; each                    *)
(* MCRNSendStoreLayoutBroken*.cfg turns one rule off and must violate the  *)
(* invariant it names (README-RNSendStoreLayout.md).                       *)
(***************************************************************************)
EXTENDS RNSendStoreLayout

MCSpec == Spec

=============================================================================
