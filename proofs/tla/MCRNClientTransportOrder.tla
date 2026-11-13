--------------------------- MODULE MCRNClientTransportOrder ---------------------------
(***************************************************************************)
(* The small instances of RNClientTransportOrder: three candidates (the default    *)
(* transport order) and one (a pin), every token shape and every outcome of every   *)
(* socket.  MCRNClientTransportOrder.cfg and MCRNClientTransportOrderPin.cfg are the       *)
(* passing controls.  MCRNClientTransportOrderBrokenNoFallback.cfg,                *)
(* MCRNClientTransportOrderBrokenOomFalls.cfg and                                   *)
(* MCRNClientTransportOrderBrokenIgnoresToken.cfg are the reversed witnesses.      *)
(* README-RNClientTransportOrder.md has the traces TLC printed.                     *)
(***************************************************************************)
EXTENDS RNClientTransportOrder
=============================================================================
