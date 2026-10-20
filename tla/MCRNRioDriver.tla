----------------------------- MODULE MCRNRioDriver -----------------------------
(***************************************************************************)
(* The small instance of RNRioDriver: two slots per send phase, two       *)
(* receive slices, one datagram.  MCRNRioDriver.cfg is the committed      *)
(* run, all rules on; MCRNRioDriverBrokenSwitchWithoutDrain.cfg,          *)
(* MCRNRioDriverBrokenFreeBeforeDrain.cfg and                             *)
(* MCRNRioDriverBrokenRetireOnRefusal.cfg each restore one defect and     *)
(* must fail.  README-RNRioDriver.md gives the traces TLC printed and the *)
(* check against the design by hand.                                      *)
(***************************************************************************)
EXTENDS RNRioDriver
=============================================================================
