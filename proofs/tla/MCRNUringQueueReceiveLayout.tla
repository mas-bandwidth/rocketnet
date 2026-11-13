-------------------- MODULE MCRNUringQueueReceiveLayout --------------------
(***************************************************************************)
(* The small instance of RNUringQueueReceiveLayout: a bank of three slots, *)
(* two provided buffers whose 16-bit ring counters wrap at 4 (create       *)
(* leaves the tail at 2, so both tail and head wrap across four receives   *)
(* and every recycled ring entry is consumed again), receives of one or    *)
(* two datagrams, drains of up to two.  The bank fills, the ring runs dry  *)
(* and every slot and entry is reused.                                     *)
(* MCRNUringQueueReceiveLayout.cfg is the passing control; each            *)
(* MCRNUringQueueReceiveLayoutBroken*.cfg must violate the invariant it    *)
(* names (README-RNUringQueueReceive.md).                                  *)
(***************************************************************************)
EXTENDS RNUringQueueReceiveLayout

MCSpec == Spec
MCDataRearmKeep == "keep"

=============================================================================
