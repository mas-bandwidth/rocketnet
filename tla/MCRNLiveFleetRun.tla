-------------------------- MODULE MCRNLiveFleetRun --------------------------
(***************************************************************************)
(* The small instance of RNLiveFleetRun.  MCRNLiveFleetRun.cfg is the      *)
(* design: two clients, two reconnects each, max_clients equal to the      *)
(* client count (the tight case), one outside event per run.  The          *)
(* reversed witnesses override one rule switch each:                       *)
(* MCRNLiveFleetRunBrokenXdpLeftAttached.cfg sets                          *)
(* TeardownDetachesOnCrash <- TeardownDetachesOnCrashBroken (teardown      *)
(* trusts the server's own destroy, so a crashed server leaves XDP         *)
(* attached) and must fail TeardownDetaches;                               *)
(* MCRNLiveFleetRunBrokenServerOrphaned.cfg sets                           *)
(* WatchdogPresent <- WatchdogPresentBroken (no watchdog) and must fail    *)
(* XdpNeverOrphaned.                                                       *)
(***************************************************************************)
EXTENDS RNLiveFleetRun

CONSTANTS C1, C2, S1

TeardownDetachesOnCrashBroken == FALSE
WatchdogPresentBroken         == FALSE
=============================================================================
