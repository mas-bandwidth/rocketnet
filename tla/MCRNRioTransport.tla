----------------------------- MODULE MCRNRioTransport -----------------------------
(***************************************************************************)
(* The small instance of RNRioTransport: two slots per send phase, two       *)
(* receive slices, one datagram.  MCRNRioTransport.cfg is the committed      *)
(* run, all rules on; MCRNRioTransportBrokenSwitchWithoutDrain.cfg,          *)
(* MCRNRioTransportBrokenFreeBeforeDrain.cfg and                             *)
(* MCRNRioTransportBrokenRetireOnRefusal.cfg each restore one defect and     *)
(* must fail.  README-RNRioTransport.md gives the traces TLC printed and the *)
(* check against the design by hand.                                      *)
(***************************************************************************)
EXTENDS RNRioTransport
=============================================================================
