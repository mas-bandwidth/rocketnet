------------------------ MODULE MCRNUringQueueReceive ------------------------
(***************************************************************************)
(* The small instance of RNUringQueueReceive: a bank of two slots, two     *)
(* provided buffers, a socket that holds one datagram, four datagrams     *)
(* each accepted or refused, drains of up to two.  The socket overflows,   *)
(* the ring runs dry (ENOBUFS), the bank fills and every slot is reused.   *)
(* MCRNUringQueueReceive.cfg (safety) and MCRNUringQueueReceiveLive.cfg   *)
(* (Rearms) are the passing controls; each MCRNUringQueueReceiveBroken*   *)
(* .cfg must violate the property it names                                *)
(* (README-RNUringQueueReceive.md).                                        *)
(***************************************************************************)
EXTENDS RNUringQueueReceive

MCSpec == Spec
MCLiveSpec == LiveSpec

=============================================================================
