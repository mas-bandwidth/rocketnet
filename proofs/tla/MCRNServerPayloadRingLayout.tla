-------------------- MODULE MCRNServerPayloadRingLayout --------------------
(***************************************************************************)
(* The small instance of RNServerPayloadRingLayout: a frame stride of 2    *)
(* (the broken recovery divides by 1).  The controls run three game        *)
(* threads at a send capacity of three: MCRNServerPayloadRingLayout.cfg    *)
(* with the ring exactly the capacity (the code's sizing at                *)
(* ROCKETNET_MAX_CLIENTS clients) and four acquires per flush,             *)
(* MCRNServerPayloadRingLayoutSpare.cfg with a ring one larger (fewer      *)
(* clients) and five.  The witnesses run two threads at a capacity of two. *)
(* Each MCRNServerPayloadRingLayoutBroken*.cfg turns one rule off and must *)
(* violate the invariant it names (README-RNServerPayloadRingLayout.md).   *)
(***************************************************************************)
EXTENDS RNServerPayloadRingLayout

MCSpec == Spec

=============================================================================
