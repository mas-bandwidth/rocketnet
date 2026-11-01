----------------------------- MODULE MCRNClientIocp -----------------------------
(***************************************************************************)
(* The small instance of RNClientIocp: two posted receives, two           *)
(* datagrams.  MCRNClientIocp.cfg is the committed run with every rule    *)
(* on; MCRNClientIocpBrokenFreeBeforeDrain.cfg lets Free follow Close at  *)
(* once and must fail on FreedOnlyWhenDrained;                            *)
(* MCRNClientIocpBrokenCopyPosted.cfg lets Drain copy a buffer not        *)
(* dequeued ready and must fail on NeverCopiedWhilePosted.                *)
(***************************************************************************)
EXTENDS RNClientIocp
=============================================================================
