--------------------------- MODULE RNClientDelivery ---------------------------
(***************************************************************************)
(* The client transport's payload batch across the end of its connection.  *)
(* rocketnet_client_transport_update clears the batch (received_packets:   *)
(* num_packets and receive_index), opens the exposed packets, each payload *)
(* written at slot num_packets and counted CLIENT_PAYLOAD_DELIVERED, runs  *)
(* the protocol's deadlines, and, when the connection ended inside this    *)
(* update, runs rocketnet_client_connection_reset. The caller then drains  *)
(* the batch through rocketnet_client_transport_receive_packet (the cursor *)
(* at receive_index) or the index surface, until the next update. The      *)
(* caller's rocketnet_client_transport_disconnect runs the same reset      *)
(* between updates.                                                        *)
(*                                                                         *)
(* The layout is the code's: slots 0..Cap-1 hold payload ids, num and idx  *)
(* are the two counts, the readable entries are idx..num-1, and only the   *)
(* counts are cleared (the bytes stay, unreachable past num). Payload ids  *)
(* never repeat, so an id read twice or a stale id exposed is visible.     *)
(*                                                                         *)
(* Defect "endwipes" restores the reset that zeroed both counts, which     *)
(* threw away payloads the same update had already counted delivered.      *)
(* Defect "nobeginclear" leaves the counts at the update's start, so a     *)
(* batch kept across the end would be offered again by the next update.    *)
(***************************************************************************)
EXTENDS Naturals, FiniteSets

CONSTANTS Cap,          \* ROCKETNET_NUM_CLIENT_PACKETS, small
          MaxIds,       \* payloads the server sends in one run
          CallerDrains, \* the caller reads the whole batch before each update
          Defect        \* "none", "endwipes" or "nobeginclear"

ASSUME Cap \in Nat \ {0} /\ MaxIds \in Nat \ {0}
ASSUME CallerDrains \in BOOLEAN
ASSUME Defect \in {"none", "endwipes", "nobeginclear"}

Ids == 1..MaxIds
Slots == 0..(Cap - 1)

VARIABLES st,      \* the protocol: "connected" or "ended" (state <= DISCONNECTED)
          phase,   \* "caller" between updates, "update" inside one
          before,  \* the state the running update started in (state_before)
          slots,   \* received_packets.packet_data: the payload id each slot holds, 0 never written
          num,     \* received_packets.num_packets
          idx,     \* received_packets.receive_index
          nextId,  \* the next payload id the server sends
          counted, \* ghost: ids counted CLIENT_PAYLOAD_DELIVERED
          handed,  \* ghost: ids returned to the caller
          passed,  \* ghost: ids offered and left unread when the next update cleared the batch
          reads    \* ghost: payloads returned to the caller, counting repeats

vars == <<st, phase, before, slots, num, idx, nextId, counted, handed, passed, reads>>

\* the entries the cursor and the index surface still reach
Readable == {slots[i] : i \in idx..(num - 1)}

\* rocketnet_client_connection_reset's effect on the batch counts
ResetCounts == IF Defect = "endwipes" THEN <<0, 0>> ELSE <<num, idx>>

Init ==
    /\ st = "connected" /\ phase = "caller" /\ before = "connected"
    /\ slots = [i \in Slots |-> 0] /\ num = 0 /\ idx = 0
    /\ nextId = 1 /\ counted = {} /\ handed = {} /\ passed = {} /\ reads = 0

\* update: the batch counts back to zero before any packet is processed; the
\* entries the caller left unread are no longer reachable
Begin ==
    /\ phase = "caller"
    /\ CallerDrains => idx = num
    /\ phase' = "update" /\ before' = st
    /\ passed' = passed \cup Readable
    /\ IF Defect = "nobeginclear"
       THEN UNCHANGED <<num, idx>>
       ELSE num' = 0 /\ idx' = 0
    /\ UNCHANGED <<st, slots, nextId, counted, handed, reads>>

\* rocketnet_client_process_packet: a payload opened while connected is
\* counted delivered and written at slot num_packets. The exposed packets of
\* one update fit the batch (rocketnet_client_update_expose_legacy_receive's
\* packet budget), so a payload always has a slot.
RecvPayload ==
    /\ phase = "update" /\ st = "connected"
    /\ num < Cap /\ nextId \in Ids
    /\ slots' = [slots EXCEPT ![num] = nextId]
    /\ num' = num + 1
    /\ counted' = counted \cup {nextId}
    /\ nextId' = nextId + 1
    /\ UNCHANGED <<st, phase, before, idx, handed, passed, reads>>

\* the connection ends inside the update: the server's disconnect opens, or
\* rocketnet_client_protocol_update finds a deadline passed
EndInUpdate ==
    /\ phase = "update" /\ st = "connected"
    /\ st' = "ended"
    /\ UNCHANGED <<phase, before, slots, num, idx, nextId, counted, handed, passed, reads>>

\* the end of update: the reset when this update took the connection from
\* live to ended
Return ==
    /\ phase = "update"
    /\ phase' = "caller"
    /\ IF before = "connected" /\ st = "ended"
       THEN num' = ResetCounts[1] /\ idx' = ResetCounts[2]
       ELSE UNCHANGED <<num, idx>>
    /\ UNCHANGED <<st, before, slots, nextId, counted, handed, passed, reads>>

\* rocketnet_client_transport_receive_packet: the cursor copies out the
\* entry at receive_index and moves on
Read ==
    /\ phase = "caller" /\ idx < num
    /\ handed' = handed \cup {slots[idx]}
    /\ idx' = idx + 1
    /\ reads' = reads + 1
    /\ UNCHANGED <<st, phase, before, slots, num, nextId, counted, passed>>

\* rocketnet_client_transport_disconnect between updates: the burst, then the
\* same reset
Disconnect ==
    /\ phase = "caller" /\ st = "connected"
    /\ st' = "ended"
    /\ num' = ResetCounts[1] /\ idx' = ResetCounts[2]
    /\ UNCHANGED <<phase, before, slots, nextId, counted, handed, passed, reads>>

Next == Begin \/ RecvPayload \/ EndInUpdate \/ Return \/ Read \/ Disconnect

Spec == Init /\ [][Next]_vars
FairSpec == Spec /\ WF_vars(Read) /\ WF_vars(Return)

TypeOK ==
    /\ st \in {"connected", "ended"} /\ phase \in {"caller", "update"}
    /\ before \in {"connected", "ended"}
    /\ slots \in [Slots -> 0..MaxIds]
    /\ num \in 0..Cap /\ idx \in 0..num
    /\ nextId \in 1..(MaxIds + 1)
    /\ counted \subseteq Ids /\ handed \subseteq Ids /\ passed \subseteq Ids
    /\ reads \in Nat

\* Counted delivered implies handed to the caller: every payload counted
\* CLIENT_PAYLOAD_DELIVERED was returned to the caller, is still readable
\* in the batch, or was offered and left unread until the next update.
CountedImpliesHanded == counted = handed \cup passed \cup Readable

\* Nothing is offered twice, and nothing uncounted is offered: the readable
\* entries are counted and neither handed nor passed, and no payload is
\* returned twice.
OfferedOnce ==
    /\ Readable \subseteq counted
    /\ Readable \cap (handed \cup passed) = {}
    /\ handed \cap passed = {}
    /\ reads = Cardinality(handed)

\* Under a caller that reads, every payload counted delivered reaches it;
\* with CallerDrains, every one is returned to the caller.
DeliveredReachesCaller == \A p \in Ids : (p \in counted) ~> (p \in handed \cup passed)
=============================================================================
