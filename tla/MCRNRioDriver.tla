----------------------------- MODULE MCRNRioDriver -----------------------------
(***************************************************************************)
(* The small instance of RNRioDriver: two slots per send phase, two       *)
(* receive slices, one datagram.  MCRNRioDriver.cfg is the committed      *)
(* run, both rules on; MCRNRioDriverBrokenSwitchWithoutDrain.cfg and      *)
(* MCRNRioDriverBrokenFreeBeforeDrain.cfg each restore one defect and     *)
(* must fail.  README-RNRioDriver.md gives the traces TLC printed and the *)
(* check against the design by hand.                                      *)
(***************************************************************************)
EXTENDS RNRioDriver
=============================================================================
