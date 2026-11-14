------------------------ MODULE MCClientConnectionEarly ------------------------
(***************************************************************************)
(* The reach probe for the early send (TRANSPORT.md t11 rule 7), kept out  *)
(* of MCClientConnection so the cases that read only that module keep      *)
(* their records.  MCClientConnectionReachSFirstSmall.cfg runs Spec under   *)
(* the constants of MCClientConnectionSFirstSmall.cfg and checks           *)
(* NotEarlyConnected; TLC must violate it, which shows those constants     *)
(* carry an attempt to connected through both early sends, so the safety   *)
(* case checks the early send and not only the states before it.          *)
(***************************************************************************)
EXTENDS MCClientConnection

EarlyConnected    == cstate = 4 /\ cEarlySends = 2
NotEarlyConnected == ~EarlyConnected

=============================================================================
