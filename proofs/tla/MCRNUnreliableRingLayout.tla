--------------------- MODULE MCRNUnreliableRingLayout ----------------------
(***************************************************************************)
(* The small instance of RNUnreliableRingLayout: three descriptors in a   *)
(* ring of six entries (the code's 2 * queue), tickets 1..3 so the shared *)
(* counter wraps every three draws.  MCRNUnreliableRingLayout.cfg is the  *)
(* passing control; each MCRNUnreliableRingLayoutBroken*.cfg turns one    *)
(* rule off and must violate the invariant it names                       *)
(* (README-RNUnreliableRingLayout.md).                                    *)
(***************************************************************************)
EXTENDS RNUnreliableRingLayout

MCSpec == Spec

=============================================================================
