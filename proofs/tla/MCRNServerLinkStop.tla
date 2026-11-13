--------------------------- MODULE MCRNServerLinkStop ---------------------------
(***************************************************************************)
(* The instances of RNServerLinkStop.  MCRNServerLinkStop.cfg (two send   *)
(* queues) and MCRNServerLinkStopOneQueue.cfg (one) are the stop's drain   *)
(* of the simulator judged per destination queue, with a discard once the *)
(* transport is idle; MCRNServerLinkStopBrokenFewest.cfg,                 *)
(* MCRNServerLinkStopBrokenNoDiscard.cfg and                              *)
(* MCRNServerLinkStopBrokenEarlyDiscard.cfg each restore one defect and   *)
(* must fail.  README-RNServerLinkStop.md gives the traces and the check  *)
(* against the code by hand.                                              *)
(***************************************************************************)
EXTENDS RNServerLinkStop
=============================================================================
