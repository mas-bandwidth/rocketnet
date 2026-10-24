---------------------------- MODULE MCRNSimFaults ----------------------------
(***************************************************************************)
(* The small instance of RNSimFaults.  Two clients and the server; one     *)
(* client crash, one server crash and one clean disconnect per run; the    *)
(* clocks at the design's extremes in units of 1/200 s per step (offset    *)
(* 0.25 s is 50 units, rate 1 +- 0.005 is 200 +- 1 units per step): one    *)
(* client fast and ahead, one slow and behind, the server on time.         *)
(*                                                                         *)
(* MCRNSimFaults.cfg is the design and passes.  The reversed witnesses     *)
(* override one rule switch each:                                          *)
(* MCRNSimFaultsBrokenRestartKeepsSession.cfg sets                         *)
(* FreshOnRestart <- FreshOnRestartBroken (a restarted client reconnects   *)
(* under its old generation) and must fail RestartIsFresh;                 *)
(* MCRNSimFaultsBrokenCrashedSends.cfg sets                                *)
(* CrashSilences <- CrashSilencesBroken (a crash flips the phase and       *)
(* leaves the endpoint running) and must fail CrashedEndpointSilent.       *)
(***************************************************************************)
EXTENDS RNSimFaults

CONSTANTS C1, C2, S0

MCOffset == [p \in {C1, C2, S0} |-> IF p = C1 THEN 50 ELSE IF p = C2 THEN -50 ELSE 0]
MCRate   == [p \in {C1, C2, S0} |-> IF p = C1 THEN 201 ELSE IF p = C2 THEN 199 ELSE 200]

FreshOnRestartBroken == FALSE
CrashSilencesBroken  == FALSE
=============================================================================
