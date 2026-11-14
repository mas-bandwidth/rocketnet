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
(* One update processes at most Budget packets                             *)
(* (ROCKETNET_CLIENT_UPDATE_PACKET_BUDGET), each placing at most one       *)
(* payload, and the batch is sized by the same constant (Budget <= Cap, a  *)
(* static_assert in the code), so the slot for a packet's payload always   *)
(* exists and the count and the placement are one step.                    *)
(*                                                                         *)
(* Defect "endwipes" restores the reset that zeroed both counts, which     *)
(* threw away payloads the same update had already counted delivered.      *)
(* Defect "nobeginclear" leaves the counts at the update's start, so a     *)
(* batch kept across the end would be offered again by the next update.    *)
(* Defect "countbeforeplace" restores the protocol counting a payload      *)
(* before the transport looks for its slot, with a budget larger than the  *)
(* batch: a payload past the last slot is counted and never placed.        *)
(* Defect "refusevanishes" refuses a payload too large for the caller's    *)
(* buffer without counting it, so a payload counted delivered is neither   *)
(* returned, readable nor refused.                                         *)
(*                                                                         *)
(* The cursor refuses a payload larger than the caller's buffer whole: it  *)
(* moves past it and counts RECEIVE_CURSOR_TOO_SMALL, and the payload is   *)
(* never returned. Read's second branch is that refusal; refused is the    *)
(* ghost of the payloads it passed over. (The index surface hands out a    *)
(* pointer per index, copies nothing and refuses nothing; it is not a      *)
(* separate action here.)                                                  *)
(***************************************************************************)
EXTENDS Naturals, FiniteSets

CONSTANTS Cap,          \* the batch's slots (received_packets), small
          Budget,       \* ROCKETNET_CLIENT_UPDATE_PACKET_BUDGET: packets one update processes
          MaxIds,       \* payloads the server sends in one run
          CallerDrains, \* the caller reads the whole batch before each update
          Defect        \* "none", "endwipes", "nobeginclear", "countbeforeplace"
                        \* or "refusevanishes"

ASSUME Cap \in Nat \ {0} /\ Budget \in Nat \ {0} /\ MaxIds \in Nat \ {0}
ASSUME CallerDrains \in BOOLEAN
ASSUME Defect \in {"none", "endwipes", "nobeginclear", "countbeforeplace", "refusevanishes"}
\* the static_asserts beside rocketnet_client_received_packets_t; only the
\* countbeforeplace witness builds without them
ASSUME Defect = "countbeforeplace" \/ Budget <= Cap

Ids == 1..MaxIds
Slots == 0..(Cap - 1)

VARIABLES st,      \* the protocol: "connected" or "ended" (state <= DISCONNECTED)
          phase,   \* "caller" between updates, "update" inside one
          before,  \* the state the running update started in (state_before)
          slots,   \* received_packets.packet_data: the payload id each slot holds, 0 never written
          num,     \* received_packets.num_packets
          idx,     \* received_packets.receive_index
          taken,   \* packets this update has processed
          nextId,  \* the next payload id the server sends
          counted, \* ghost: ids counted CLIENT_PAYLOAD_DELIVERED
          handed,  \* ghost: ids returned to the caller
          passed,  \* ghost: ids offered and left unread when the next update cleared the batch
          refused, \* ghost: ids the cursor refused, too large for the caller's buffer
          reads    \* ghost: payloads returned to the caller, counting repeats

vars == <<st, phase, before, slots, num, idx, taken, nextId, counted, handed, passed, refused, reads>>

\* the entries the cursor and the index surface still reach
Readable == {slots[i] : i \in idx..(num - 1)}

\* rocketnet_client_connection_reset's effect on the batch counts
ResetCounts == IF Defect = "endwipes" THEN <<0, 0>> ELSE <<num, idx>>

Init ==
    /\ st = "connected" /\ phase = "caller" /\ before = "connected"
    /\ slots = [i \in Slots |-> 0] /\ num = 0 /\ idx = 0 /\ taken = 0
    /\ nextId = 1 /\ counted = {} /\ handed = {} /\ passed = {} /\ refused = {}
    /\ reads = 0

\* update: the batch counts back to zero before any packet is processed; the
\* entries the caller left unread are no longer reachable
Begin ==
    /\ phase = "caller"
    /\ CallerDrains => idx = num
    /\ phase' = "update" /\ before' = st /\ taken' = 0
    /\ passed' = passed \cup Readable
    /\ IF Defect = "nobeginclear"
       THEN UNCHANGED <<num, idx>>
       ELSE num' = 0 /\ idx' = 0
    /\ UNCHANGED <<st, slots, nextId, counted, handed, refused, reads>>

\* rocketnet_client_process_packet: a payload opened while connected is
\* counted delivered and placed at slot num_packets in one step. With the
\* budget tied to the batch the slot exists; the ELSE arm is reachable only
\* in the countbeforeplace witness, where the count runs ahead of the batch.
RecvPayload ==
    /\ phase = "update" /\ st = "connected"
    /\ taken < Budget /\ nextId \in Ids
    /\ taken' = taken + 1 /\ nextId' = nextId + 1
    /\ IF num < Cap
       THEN /\ slots' = [slots EXCEPT ![num] = nextId]
            /\ num' = num + 1
            /\ counted' = counted \cup {nextId}
       ELSE /\ UNCHANGED <<slots, num>>
            /\ counted' = IF Defect = "countbeforeplace" THEN counted \cup {nextId} ELSE counted
    /\ UNCHANGED <<st, phase, before, idx, handed, passed, refused, reads>>

\* a processed packet that places no payload (an ack, a keep-alive, a
\* fragment that completes nothing): it takes budget and no slot
RecvOther ==
    /\ phase = "update" /\ taken < Budget
    /\ taken' = taken + 1
    /\ UNCHANGED <<st, phase, before, slots, num, idx, nextId, counted, handed, passed, refused, reads>>

\* the connection ends inside the update: the server's disconnect opens, or
\* rocketnet_client_protocol_update finds a deadline passed
EndInUpdate ==
    /\ phase = "update" /\ st = "connected"
    /\ st' = "ended"
    /\ UNCHANGED <<phase, before, slots, num, idx, taken, nextId, counted, handed, passed, refused, reads>>

\* the end of update: the reset when this update took the connection from
\* live to ended
Return ==
    /\ phase = "update"
    /\ phase' = "caller"
    /\ IF before = "connected" /\ st = "ended"
       THEN num' = ResetCounts[1] /\ idx' = ResetCounts[2]
       ELSE UNCHANGED <<num, idx>>
    /\ UNCHANGED <<st, before, slots, taken, nextId, counted, handed, passed, refused, reads>>

\* rocketnet_client_transport_receive_packet: the cursor moves past the entry
\* at receive_index and either copies it out, or, when it is larger than the
\* caller's buffer, refuses it whole and counts RECEIVE_CURSOR_TOO_SMALL
\* (the caller's buffer size is the caller's choice: either branch)
Read ==
    /\ phase = "caller" /\ idx < num
    /\ idx' = idx + 1
    /\ \/ /\ handed' = handed \cup {slots[idx]}
          /\ reads' = reads + 1
          /\ UNCHANGED refused
       \/ /\ refused' = IF Defect = "refusevanishes" THEN refused ELSE refused \cup {slots[idx]}
          /\ UNCHANGED <<handed, reads>>
    /\ UNCHANGED <<st, phase, before, slots, num, taken, nextId, counted, passed>>

\* rocketnet_client_transport_disconnect between updates: the burst, then the
\* same reset
Disconnect ==
    /\ phase = "caller" /\ st = "connected"
    /\ st' = "ended"
    /\ num' = ResetCounts[1] /\ idx' = ResetCounts[2]
    /\ UNCHANGED <<phase, before, slots, taken, nextId, counted, handed, passed, refused, reads>>

Next == Begin \/ RecvPayload \/ RecvOther \/ EndInUpdate \/ Return \/ Read \/ Disconnect

Spec == Init /\ [][Next]_vars
FairSpec == Spec /\ WF_vars(Read) /\ WF_vars(Return)

TypeOK ==
    /\ st \in {"connected", "ended"} /\ phase \in {"caller", "update"}
    /\ before \in {"connected", "ended"}
    /\ slots \in [Slots -> 0..MaxIds]
    /\ num \in 0..Cap /\ idx \in 0..num /\ taken \in 0..Budget
    /\ nextId \in 1..(MaxIds + 1)
    /\ counted \subseteq Ids /\ handed \subseteq Ids /\ passed \subseteq Ids
    /\ refused \subseteq Ids
    /\ reads \in Nat

\* Counted delivered implies offered to the caller: every payload counted
\* CLIENT_PAYLOAD_DELIVERED was returned to the caller, refused by the cursor
\* as too large for the caller's buffer (and counted
\* RECEIVE_CURSOR_TOO_SMALL), is still readable in the batch, or was offered
\* and left unread until the next update.
CountedImpliesHanded == counted = handed \cup refused \cup passed \cup Readable

\* The layout: inside an update the batch holds no more entries than the
\* packets processed, and while the budget allows another packet the batch
\* has a slot for its payload.
SlotForNextPacket ==
    phase = "update" => (num <= taken /\ (taken < Budget => num < Cap))

\* Nothing is offered twice, and nothing uncounted is offered: the readable
\* entries are counted and neither handed, refused nor passed, a payload has
\* one fate, and no payload is returned twice.
OfferedOnce ==
    /\ Readable \subseteq counted
    /\ Readable \cap (handed \cup refused \cup passed) = {}
    /\ handed \cap passed = {}
    /\ refused \cap (handed \cup passed) = {}
    /\ reads = Cardinality(handed)

\* Under a caller that reads, every payload counted delivered reaches it;
\* with CallerDrains, every one is returned to the caller or refused as too
\* large for the caller's buffer.
DeliveredReachesCaller == \A p \in Ids : (p \in counted) ~> (p \in handed \cup refused \cup passed)
=============================================================================
