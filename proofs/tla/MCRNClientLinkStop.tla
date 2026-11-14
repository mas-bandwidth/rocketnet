--------------------------- MODULE MCRNClientLinkStop ---------------------------
(***************************************************************************)
(* The instances of RNClientLinkStop.  MCRNClientLinkStop.cfg is both     *)
(* paths: the legacy worker-completion drain behind a non-draining stop   *)
(* request, and the I/O-core stop that drains itself.                     *)
(* MCRNClientLinkStopBrokenNoDrain.cfg and                                *)
(* MCRNClientLinkStopBrokenDueOnly.cfg each restore one defect and must   *)
(* fail.  MCRNClientLinkStopReachAllDropped.cfg is a reach case: a burst  *)
(* whose every sent copy is dropped never frees the slot.                 *)
(* README-RNClientLinkStop.md gives the traces and the check against the  *)
(* code by hand.                                                          *)
(***************************************************************************)
EXTENDS RNClientLinkStop
=============================================================================
