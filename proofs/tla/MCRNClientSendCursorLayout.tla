-------------------------- MODULE MCRNClientSendCursorLayout --------------------------
(***************************************************************************)
(* The capacity-4 instance of RNClientSendCursorLayout.  The control       *)
(* (CursorAt = "slots") and the reversed witness CursorAt = "plus", which  *)
(* fails AtMostOnce: cursor = send_slot + consumed ignores aborted slots.  *)
(* The reversed witness DropKept = TRUE fails NoneLost: the drop arm also  *)
(* drops an unstarted packet of the kept family.                           *)
(***************************************************************************)
EXTENDS RNClientSendCursorLayout
=============================================================================
