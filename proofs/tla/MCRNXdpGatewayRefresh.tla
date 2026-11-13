----------------------- MODULE MCRNXdpGatewayRefresh -----------------------
(***************************************************************************)
(* The small instance of RNXdpGatewayRefresh. MCRNXdpGatewayRefresh.cfg is *)
(* the design: a period of 2 over a platform clock to 6 in steps up to 6,  *)
(* two gateway changes and two failed resolves.                            *)
(* MCRNXdpGatewayRefreshBrokenFailureWritesTable.cfg writes a failed       *)
(* resolve's unusable answer and must fail StagedMacUsable;                *)
(* MCRNXdpGatewayRefreshBrokenRefreshInFill.cfg runs the refresh in the    *)
(* fill phase and must fail NoTornMac;                                     *)
(* MCRNXdpGatewayRefreshBrokenNeverRefreshed.cfg resolves at create only   *)
(* and must fail FreshAfterPeriod.                                         *)
(***************************************************************************)
EXTENDS RNXdpGatewayRefresh
=============================================================================
