---------------------------- MODULE RNFlows ----------------------------
\* Flows on the message server (docs/FLOWS.md): a flow is a server-side set
\* of subscribed slots with an id. Send to a flow queues one copy to each
\* subscriber's endpoint, the way broadcast does. A subscription is tagged
\* with the connection id of its slot (as RNSlots binds endpoints) and ends
\* when that connection ends: at Observe, every slot whose connection
\* changed loses its subscriptions before any later send can reach the new
\* connection. Flow ids never repeat, so a destroyed id never resolves to
\* the flow that later takes its table index.
\*
\* The ghost `wants` is the application's intent: the (connection, flow id)
\* pairs it subscribed and has not unsubscribed or destroyed. `entitled`
\* records, at each send, every connection that wanted that flow then.
\* `delivered` is what reached a connection. DeliveredOnlyToSubscribers
\* says nothing reaches a connection that was not subscribed at the send.
EXTENDS Naturals, FiniteSets

CONSTANTS SlotCount, MaxConn, MaxFlows, MaxIds, MaxMessages,
          MaxSubsPerFlow, MaxFlowsPerClient, Broken

Slots == 1..SlotCount
Flows == 1..MaxFlows
Conns == 1..MaxConn
Ids == 1..MaxIds
Msgs == 1..MaxMessages
Sub == [slot : Slots, conn : Conns]
Want == [conn : Conns, id : Ids]
Copy == [conn : Conns, id : Ids, msg : Msgs]
Reasons == {"FLOW_TABLE_FULL", "FLOW_UNKNOWN", "FLOW_NOT_CONNECTED",
            "FLOW_ALREADY_SUBSCRIBED", "FLOW_NOT_SUBSCRIBED",
            "FLOW_SUBSCRIBERS_FULL", "FLOW_CLIENT_FLOWS_FULL"}

VARIABLES phase, owner, nextConn, bound, table, nextId, subs, clientFlows,
          queue, wants, entitled, delivered, nextMsg
vars == <<phase, owner, nextConn, bound, table, nextId, subs, clientFlows,
          queue, wants, entitled, delivered, nextMsg>>
app == <<table, nextId, subs, clientFlows, wants, entitled, nextMsg>>

Init ==
    /\ phase = "application"
    /\ owner = [s \in Slots |-> 0] /\ nextConn = 1
    /\ bound = [s \in Slots |-> 0]
    /\ table = [i \in Flows |-> 0] /\ nextId = 1
    /\ subs = [i \in Flows |-> {}] /\ clientFlows = [s \in Slots |-> {}]
    /\ queue = [s \in Slots |-> {}]
    /\ wants = {} /\ entitled = {} /\ delivered = {}
    /\ nextMsg = 1

\* The flow table index holding id, or 0 when no live flow has it.
IndexOf(id) == IF \E i \in Flows : table[i] = id
               THEN CHOOSE i \in Flows : table[i] = id ELSE 0
Issued == 1..(nextId - 1)
\* A slot whose endpoint is bound to the connection the transport still has:
\* not free and not kicked since the last Observe.
Live(s) == bound[s] # 0 /\ owner[s] = bound[s]
\* A refusal changes no flow state: the design counts it under the name r
\* (FLOWS.md 4); the counter is not modelled, so a refusal is a stutter.
Refuse(r) == /\ r \in Reasons /\ UNCHANGED vars

\* ---- application phase: the main thread between updates (FLOWS.md 3) ----

\* rn_flows_create: the first free index; FLOW_TABLE_FULL when none.
\* MaxIds bounds the model's lifetime, not the design: ids never repeat.
Create ==
    /\ phase = "application" /\ nextId <= MaxIds
    /\ IF \E i \in Flows : table[i] = 0
       THEN LET i == CHOOSE j \in Flows : table[j] = 0
            IN /\ table' = [table EXCEPT ![i] = nextId]
               /\ subs' = [subs EXCEPT ![i] = {}]
               /\ nextId' = nextId + 1
               /\ UNCHANGED <<phase, owner, nextConn, bound, clientFlows, queue,
                              wants, entitled, delivered, nextMsg>>
       ELSE Refuse("FLOW_TABLE_FULL")

\* rn_flows_destroy: ends every subscription to the flow, frees the index.
Destroy(id) ==
    /\ phase = "application"
    /\ LET i == IndexOf(id)
       IN IF i = 0 THEN Refuse("FLOW_UNKNOWN")
          ELSE /\ table' = [table EXCEPT ![i] = 0]
               /\ subs' = [subs EXCEPT ![i] = {}]
               /\ clientFlows' = [s \in Slots |-> clientFlows[s] \ {i}]
               /\ wants' = {w \in wants : w.id # id}
               /\ UNCHANGED <<phase, owner, nextConn, bound, nextId, queue,
                              entitled, delivered, nextMsg>>

\* rn_flows_subscribe: the refusals in FLOWS.md 4, in that order.
Subscribe(id, s) ==
    /\ phase = "application"
    /\ LET i == IndexOf(id)
       IN IF i = 0 THEN Refuse("FLOW_UNKNOWN")
          ELSE IF ~Live(s) THEN Refuse("FLOW_NOT_CONNECTED")
          ELSE IF i \in clientFlows[s] THEN Refuse("FLOW_ALREADY_SUBSCRIBED")
          ELSE IF Cardinality(subs[i]) >= MaxSubsPerFlow THEN Refuse("FLOW_SUBSCRIBERS_FULL")
          ELSE IF Cardinality(clientFlows[s]) >= MaxFlowsPerClient THEN Refuse("FLOW_CLIENT_FLOWS_FULL")
          ELSE /\ subs' = [subs EXCEPT ![i] = @ \cup {[slot |-> s, conn |-> bound[s]]}]
               /\ clientFlows' = [clientFlows EXCEPT ![s] = @ \cup {i}]
               /\ wants' = wants \cup {[conn |-> bound[s], id |-> id]}
               /\ UNCHANGED <<phase, owner, nextConn, bound, table, nextId, queue,
                              entitled, delivered, nextMsg>>

\* rn_flows_unsubscribe: removes the slot from both lists. The broken
\* variant "sendafterunsubscribe" clears the client's list only, so the
\* flow's subscriber list keeps the slot and a later send still reaches it.
Unsubscribe(id, s) ==
    /\ phase = "application"
    /\ LET i == IndexOf(id)
       IN IF i = 0 THEN Refuse("FLOW_UNKNOWN")
          ELSE IF i \notin clientFlows[s] THEN Refuse("FLOW_NOT_SUBSCRIBED")
          ELSE /\ subs' = IF "sendafterunsubscribe" \in Broken THEN subs
                          ELSE [subs EXCEPT ![i] = {x \in @ : x.slot # s}]
               /\ clientFlows' = [clientFlows EXCEPT ![s] = @ \ {i}]
               /\ wants' = {w \in wants : ~(w.id = id /\ w.conn = bound[s])}
               /\ UNCHANGED <<phase, owner, nextConn, bound, table, nextId, queue,
                              entitled, delivered, nextMsg>>

\* rn_flows_send: one copy into each subscriber slot's endpoint, as
\* rocketnet_message_server_broadcast sends to a slot. A stale id reaches
\* nobody. A free or kicked slot is skipped: its send path refuses it and
\* counts that (not a flow refusal). The model checks the send with no connection-tag test,
\* so the drop at Observe alone has to keep it safe.
Send(id) ==
    /\ phase = "application" /\ nextMsg <= MaxMessages
    /\ LET i == IndexOf(id)
           m == nextMsg
       IN IF i = 0 THEN Refuse("FLOW_UNKNOWN")
          ELSE /\ queue' = [s \in Slots |->
                     IF Live(s) /\ \E x \in subs[i] : x.slot = s
                     THEN queue[s] \cup {[conn |-> bound[s], id |-> id, msg |-> m]}
                     ELSE queue[s]]
               /\ entitled' = entitled \cup
                     {[conn |-> w.conn, id |-> id, msg |-> m] : w \in {v \in wants : v.id = id}}
               /\ nextMsg' = nextMsg + 1
               /\ UNCHANGED <<phase, owner, nextConn, bound, table, nextId, subs,
                              clientFlows, wants, delivered>>

\* rn_message_object_kick from the send path (region exhausted) on the main
\* thread, possibly in the middle of a flow send: the transport ends the
\* connection now and the endpoint takes nothing more. The flow lists are
\* not touched; the slot is a changed slot at the next Observe, which drops
\* its subscriptions (FLOWS.md 5, Kick).
Kick(s) ==
    /\ phase = "application" /\ Live(s)
    /\ owner' = [owner EXCEPT ![s] = 0]
    /\ queue' = [queue EXCEPT ![s] = {}]
    /\ UNCHANGED <<phase, nextConn, bound, delivered>> /\ UNCHANGED app

BeginUpdate ==
    /\ phase = "application"
    /\ phase' = "transport"
    /\ UNCHANGED <<owner, nextConn, bound, queue, delivered>> /\ UNCHANGED app

\* ---- transport phase: slot churn and the wire ----

\* Connection ids never repeat (RNSlots Connect); any churn may precede
\* one Observe.
Connect(s) ==
    /\ phase = "transport" /\ owner[s] = 0 /\ nextConn <= MaxConn
    /\ owner' = [owner EXCEPT ![s] = nextConn] /\ nextConn' = nextConn + 1
    /\ UNCHANGED <<phase, bound, queue, delivered>> /\ UNCHANGED app

Disconnect(s) ==
    /\ phase = "transport" /\ owner[s] # 0
    /\ owner' = [owner EXCEPT ![s] = 0]
    /\ UNCHANGED <<phase, nextConn, bound, queue, delivered>> /\ UNCHANGED app

\* An endpoint's packets reach only the connection it is bound to (RNSlots
\* NoCrossConnectionApply carries this; it is assumed here, not re-proved).
Deliver(s, c) ==
    /\ phase = "transport" /\ c \in queue[s] /\ owner[s] = bound[s] /\ bound[s] # 0
    /\ delivered' = delivered \cup {[conn |-> owner[s], id |-> c.id, msg |-> c.msg]}
    /\ queue' = [queue EXCEPT ![s] = @ \ {c}]
    /\ UNCHANGED <<phase, owner, nextConn, bound>> /\ UNCHANGED app

\* rocketnet_message_server_update's Observe (RNSlots Observe): a slot whose
\* connection changed is reset, and rn_flows_drop_slot ends all of its
\* subscriptions before the application can send again. The broken variant
\* "keepatreuse" skips the drop.
Changed(s) == owner[s] # bound[s]
Observe ==
    /\ phase = "transport"
    /\ bound' = owner
    /\ queue' = [s \in Slots |-> IF Changed(s) THEN {} ELSE queue[s]]
    /\ IF "keepatreuse" \in Broken
       THEN UNCHANGED <<subs, clientFlows>>
       ELSE /\ subs' = [i \in Flows |-> {x \in subs[i] : ~Changed(x.slot)}]
            /\ clientFlows' = [s \in Slots |-> IF Changed(s) THEN {} ELSE clientFlows[s]]
    /\ phase' = "application"
    /\ UNCHANGED <<owner, nextConn, table, nextId, wants, entitled, delivered,
                   nextMsg>>

Next ==
    \/ Create \/ BeginUpdate \/ Observe
    \/ \E id \in Issued : Destroy(id) \/ Send(id)
    \/ \E id \in Issued, s \in Slots : Subscribe(id, s) \/ Unsubscribe(id, s)
    \/ \E s \in Slots : Connect(s) \/ Disconnect(s) \/ Kick(s)
    \/ \E s \in Slots : \E c \in queue[s] : Deliver(s, c)

Spec == Init /\ [][Next]_vars

TypeOK ==
    /\ phase \in {"application", "transport"}
    /\ owner \in [Slots -> 0..MaxConn] /\ bound \in [Slots -> 0..MaxConn]
    /\ nextConn \in 1..MaxConn+1 /\ nextId \in 1..MaxIds+1 /\ nextMsg \in 1..MaxMessages+1
    /\ table \in [Flows -> 0..MaxIds]
    /\ subs \in [Flows -> SUBSET Sub] /\ clientFlows \in [Slots -> SUBSET Flows]
    /\ queue \in [Slots -> SUBSET Copy]
    /\ wants \subseteq Want /\ entitled \subseteq Copy /\ delivered \subseteq Copy

\* No message sent to a flow reaches a connection that was not subscribed
\* to that flow when it was sent.
DeliveredOnlyToSubscribers == delivered \subseteq entitled

\* Every subscription names the connection its slot's endpoint is bound
\* to now: none survives the slot's reuse by another connection, and none
\* sits on a free slot.
NoSubscriptionSurvivesReuse ==
    \A i \in Flows : \A x \in subs[i] : bound[x.slot] = x.conn

\* The bounds of FLOWS.md 2: no flow over its subscriber limit, no client
\* over its flow limit, the two lists agree, and a free index is empty.
BoundsHeld ==
    /\ \A i \in Flows : Cardinality(subs[i]) <= MaxSubsPerFlow
    /\ \A s \in Slots : Cardinality(clientFlows[s]) <= MaxFlowsPerClient
    /\ \A i \in Flows : table[i] = 0 => subs[i] = {}
    /\ \A i \in Flows, s \in Slots :
          (i \in clientFlows[s]) <=> (\E x \in subs[i] : x.slot = s)
=============================================================================
