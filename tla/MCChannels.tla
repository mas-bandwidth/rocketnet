---------------------------- MODULE MCChannels ----------------------------
(***************************************************************************)
(* The small instance that checks Channels: N = 2 channels, Window = 4,    *)
(* five messages, the first-version types {0, 3}, over a link with         *)
(* capacity 2 that duplicates and reorders, under Pattern "arbitrary".     *)
(*                                                                         *)
(* Safety (MCChannels.cfg, Spec): every invariant, with loss unbounded.    *)
(* Liveness (MCChannelsLive.cfg, SpecFair): EveryReliableDelivered and     *)
(* EveryReliableAcked with BoundedLoss = TRUE.  README-Channels.md lists   *)
(* the reversed witnesses as edits to these two files.                     *)
(***************************************************************************)
EXTENDS Channels

Spec     == Init /\ [][Next]_vars
SpecFair == Init /\ [][Next]_vars /\ Fairness

\* Under Pattern "arbitrary" the link's send counter feeds nothing (no send
\* is lost by count), so the safety run folds it out of the state.
View == <<senderVars, wire, pulse, drops, ackVars, rcvVars>>

=============================================================================
