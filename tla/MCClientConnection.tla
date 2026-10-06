------------------------- MODULE MCClientConnection -------------------------
(***************************************************************************)
(* The small instance that checks ClientConnection.  Every run is this     *)
(* module with MCClientConnection.cfg, or that file with a few lines       *)
(* changed as README-ClientConnection.md lists; tla/RUNS.tsv records each. *)
(*                                                                         *)
(* The committed cfg is the hang check the request named: the code's       *)
(* keep-alive behaviour (RepeatKeepAlive = TRUE, KeepAliveClock =          *)
(* "keepalive") under KindLoss = "firstkind", every deadline and timeout   *)
(* off, a server application sending payloads at will, and no application  *)
(* disconnects: Connects and ServerNotStranded must hold.  Flipping         *)
(* RepeatKeepAlive to FALSE, or KeepAliveClock to "anysend", must make      *)
(* Connects fail: those are the reversed witnesses.                        *)
(***************************************************************************)
EXTENDS ClientConnection

\* the safety properties every run checks
SafetyInvariants ==
    /\ TypeOK /\ ConnectedMeansHandshake /\ TimeoutHonoured /\ DeadlineHonoured
    /\ AtMostOneLiveSession

=============================================================================
