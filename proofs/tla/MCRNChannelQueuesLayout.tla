---------------------- MODULE MCRNChannelQueuesLayout ----------------------
(***************************************************************************)
(* The small instance of RNChannelQueuesLayout: three send descriptors,    *)
(* two channels, ready tickets wrapping at 4 (skipping 0) and descriptor   *)
(* generations wrapping after 2.  MCRNChannelQueuesLayout.cfg is the       *)
(* passing control; each MCRNChannelQueuesLayoutBroken*.cfg removes one    *)
(* line of the code and must violate the invariant it names                *)
(* (README-RNChannelQueuesLayout.md).                                      *)
(***************************************************************************)
EXTENDS RNChannelQueuesLayout

MCSpec == Spec

=============================================================================
