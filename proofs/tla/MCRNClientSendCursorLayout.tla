-------------------------- MODULE MCRNClientSendCursorLayout --------------------------
(***************************************************************************)
(* The capacity-4 instance of RNClientSendCursorLayout.  The control       *)
(* (CursorAt = "slots") and the reversed witness CursorAt = "plus", which  *)
(* fails AtMostOnce: cursor = send_slot + consumed ignores aborted slots.  *)
(***************************************************************************)
EXTENDS RNClientSendCursorLayout
=============================================================================
