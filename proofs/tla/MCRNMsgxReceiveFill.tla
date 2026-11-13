------------------------- MODULE MCRNMsgxReceiveFill -------------------------
(***************************************************************************)
(* The small instances of RNMsgxReceiveFill.  MCRNMsgxReceiveFill.cfg: two *)
(* sockets, banks of three, a batch of two (so a read meets a room of one), *)
(* a budget of three, kernel buffers of three, seven arrivals and two      *)
(* errors, every safety property.  MCRNMsgxReceiveFillLive.cfg: the same   *)
(* with the worker and main fair, AllConsumed.  Reversed witnesses:        *)
(* MCRNMsgxReceiveFillBrokenReadPastRoom.cfg (a whole batch whatever the   *)
(* room, NoFullDrop), MCRNMsgxReceiveFillBrokenPublishOverReady.cfg (a full *)
(* bank handed over a ready one, Conserved) and                            *)
(* MCRNMsgxReceiveFillBrokenHoldWithoutPublish.cfg (a full bank held when  *)
(* main has taken the last, HoldOnlyWhenNowhere).                          *)
(***************************************************************************)
EXTENDS RNMsgxReceiveFill
=============================================================================
