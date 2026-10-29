--------------------------- MODULE MCRNClientLinkStop ---------------------------
(***************************************************************************)
(* The instances of RNClientLinkStop.  MCRNClientLinkStop.cfg is the       *)
(* worker-completion drain with a non-draining stop request;                    *)
(* MCRNClientLinkStopBrokenNoDrain.cfg and                                *)
(* MCRNClientLinkStopBrokenDueOnly.cfg each restore one defect and must   *)
(* fail.  README-RNClientLinkStop.md gives the traces and the check       *)
(* against the code by hand.                                              *)
(***************************************************************************)
EXTENDS RNClientLinkStop
=============================================================================
