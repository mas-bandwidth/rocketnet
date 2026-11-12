------------------------ MODULE MCRNUringQueueSend ------------------------
(***************************************************************************)
(* The small instance of RNUringQueueSend: three banks of two slots, two   *)
(* destinations, runs of up to two slots, three claims a fill (so a bank   *)
(* refuses one), three flushes (so every bank is reused) and one eagain.   *)
(* MCRNUringQueueSend.cfg (safety) and MCRNUringQueueSendLive.cfg (Drains) *)
(* are the passing controls; each MCRNUringQueueSendBroken*.cfg must       *)
(* violate the property it names (README-RNUringQueueSend.md).             *)
(***************************************************************************)
EXTENDS RNUringQueueSend

MCSpec == Spec
MCLiveSpec == LiveSpec

=============================================================================
