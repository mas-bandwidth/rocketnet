----------------------- MODULE MCRNXdpGatewayRefresh -----------------------
(***************************************************************************)
(* The small instance of RNXdpGatewayRefresh. MCRNXdpGatewayRefresh.cfg is *)
(* the design: a period of 2 over 6 updates and two gateway changes.       *)
(* MCRNXdpGatewayRefreshBrokenRefreshInFill.cfg runs the refresh in the    *)
(* fill phase and must fail NoTornMac;                                     *)
(* MCRNXdpGatewayRefreshBrokenNeverRefreshed.cfg resolves at create only   *)
(* and must fail FreshAfterPeriod.                                         *)
(***************************************************************************)
EXTENDS RNXdpGatewayRefresh
=============================================================================
