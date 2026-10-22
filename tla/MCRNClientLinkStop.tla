--------------------------- MODULE MCRNClientLinkStop ---------------------------
(***************************************************************************)
(* The instances of RNClientLinkStop.  MCRNClientLinkStop.cfg is the       *)
(* committed run with stop draining every staged copy;                    *)
(* MCRNClientLinkStopBrokenNoDrain.cfg and                                *)
(* MCRNClientLinkStopBrokenDueOnly.cfg each restore one defect and must   *)
(* fail.  README-RNClientLinkStop.md gives the traces and the check       *)
(* against the code by hand.                                              *)
(***************************************************************************)
EXTENDS RNClientLinkStop
=============================================================================
