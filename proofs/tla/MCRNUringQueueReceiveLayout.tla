-------------------- MODULE MCRNUringQueueReceiveLayout --------------------
(***************************************************************************)
(* The small instance of RNUringQueueReceiveLayout: a bank of three slots, *)
(* four provided buffers whose 16-bit ring counters wrap at 8 (so the      *)
(* tail, which create leaves at 4, wraps after four returns), receives of  *)
(* one or two datagrams, drains of up to two.  The bank fills, the ring    *)
(* runs dry and every slot and entry is reused.                            *)
(* MCRNUringQueueReceiveLayout.cfg is the passing control; each            *)
(* MCRNUringQueueReceiveLayoutBroken*.cfg must violate the invariant it   *)
(* names (README-RNUringQueueReceive.md).                                  *)
(***************************************************************************)
EXTENDS RNUringQueueReceiveLayout

MCSpec == Spec

=============================================================================
