--------------------------- MODULE MCRNEndpoint ---------------------------
(***************************************************************************)
(* The small instance that checks RNEndpoint: N messages per side, the    *)
(* first N - 1 reliable ordered on channel 0 and the last unreliable on    *)
(* channel 1 (N = 3 for safety, 2 for liveness), W = 2,                   *)
(* and two configurations, equal (num_channels 2 at both ends) and         *)
(* differing in num_channels (2 and 1), Link capacity 2.                   *)
(*                                                                         *)
(*   MCRNEndpoint.cfg          safety, "arbitrary" loss with reordering,   *)
(*                             failed sends, ack silence, timeouts at any  *)
(*                             time                                        *)
(*   MCRNEndpointLive.cfg      liveness under LiveSpec, each link losing   *)
(*                             its first packet                            *)
(*   MCRNEndpointBrokenConfigUnreliable.cfg  ConfigReliable = FALSE:       *)
(*                             ConfigVerifiedDespiteFirstLoss fails        *)
(*   MCRNEndpointBrokenConfigNotCompared.cfg ConfigCompared = FALSE:       *)
(*                             MismatchErrorsBothEnds fails                *)
(* README-RNEndpoint.md has the traces and the action-to-section map.      *)
(***************************************************************************)
EXTENDS RNEndpoint

\* the last message is the unreliable one, on channel 1
MCMsgRel  == [k \in 1..N |-> IF k = N THEN "unreliable" ELSE "ordered"]
MCMsgChan == [k \in 1..N |-> IF k = N THEN 1 ELSE 0]
MCCfgPairs == {[a |-> 2, b |-> 2], [a |-> 2, b |-> 1]}

=============================================================================
