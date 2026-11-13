------------------------- MODULE MCRNMsgxReceiveFill -------------------------
(***************************************************************************)
(* The small instances of RNMsgxReceiveFill.  MCRNMsgxReceiveFill.cfg: two *)
(* sockets, banks of three, a batch of two (so a read meets a room of one), *)
(* a budget of three, kernel buffers of three, calls of one or three (so a *)
(* call can be cut), six arrivals and one error, every safety property.    *)
(* MCRNMsgxReceiveFillLive.cfg: five arrivals, the worker and main fair,   *)
(* AllConsumed and StopEnds.  Reversed witnesses, one defect each:         *)
(* ReadPastRoom (NoFullDrop), ReadPastBudget (BudgetKept), HoldWithoutMove *)
(* (HoldOnlyWhenNowhere), FreeNotReset (Conserved), ReadOnlyLeft, the      *)
(* ready-slot rule (DeliveredByNextCall), and StopCountSkipsWorker         *)
(* (StopCounted).                                                          *)
(***************************************************************************)
EXTENDS RNMsgxReceiveFill
=============================================================================
