---------------------------- MODULE RNSlots ----------------------------
\* Connection-tagged transport batches, region lifetime and object updates.
\* CHANNELS.md 10.2 and 15.1 supply Observe/Admit; the exhaustion and explicit
\* release rules extend that draft. Views require explicit Release before
\* the next object update; an early update is rejected.
EXTENDS Naturals, FiniteSets, Sequences

CONSTANTS SlotCount, MaxConn, MaxMessages, TicketCount, RegionCapacity,
          LinkCapacity, Stable, Broken

Slots == 1..SlotCount
Records == [conn : 1..MaxConn, msg : 1..MaxMessages]
Packets == [slot : Slots, conn : 1..MaxConn,
            msg : 1..MaxMessages, id : 1..TicketCount]
Entry == [kind : {"payload", "ack"}, packet : Packets]
Audit == [slot : Slots, from : 1..MaxConn, to : 0..MaxConn,
          kind : {"payload", "ack"}, msg : 1..MaxMessages]
Record(p) == [conn |-> p.conn, msg |-> p.msg]
AuditOf(e, to) == [slot |-> e.packet.slot, from |-> e.packet.conn,
                   to |-> to, kind |-> e.kind, msg |-> e.packet.msg]

VARIABLES st, wire, sent, pulse
vars == <<st, wire, sent, pulse>>
Data == INSTANCE Link WITH Capacity <- LinkCapacity, MaxDelay <- 0,
        Duplication <- TRUE, Reordering <- TRUE, Pattern <- "arbitrary",
        BurstStart <- 1, BurstLen <- 1

Init ==
    /\ Data!Init
    /\ st = [owner |-> [s \in Slots |-> IF Stable /\ s = 1 THEN 1 ELSE 0],
             nextConn |-> IF Stable THEN 2 ELSE 1,
             bound |-> [s \in Slots |-> 0],
             state |-> [s \in Slots |-> "Free"],
             reason |-> [s \in Slots |-> "NONE"],
             region |-> [s \in Slots |-> {}], held |-> [s \in Slots |-> {}],
             received |-> [s \in Slots |-> {}],
             phase |-> "application", batch |-> {}, deferred |-> {}, todo |-> {},
             requested |-> {}, delivered |-> {}, released |-> {}, acked |-> {},
             opened |-> {}, packetMap |-> {}, applied |-> {}, dropped |-> {},
             changed |-> [s \in Slots |-> FALSE],
             badKick |-> FALSE, badLifetime |-> FALSE, error |-> "NONE",
             pending |-> 0, pendingConn |-> 0]

\* Transport connection ids never repeat in the bounded lifetime. Several
\* connect/disconnect steps may precede a single Observe.
Connect(s) ==
    /\ ~Stable /\ st.phase = "transport" /\ st.owner[s] = 0
    /\ st.state[s] \notin {"Exhausted", "Disconnected"}
    /\ st.nextConn <= MaxConn
    /\ st' = [st EXCEPT !.owner[s] = st.nextConn, !.nextConn = @ + 1]
    /\ UNCHANGED <<wire, sent, pulse>>

Disconnect(s) ==
    /\ ~Stable /\ st.phase = "transport" /\ st.owner[s] # 0
    /\ st' = [st EXCEPT !.owner[s] = 0]
    /\ UNCHANGED <<wire, sent, pulse>>

\* The live workload submits the next record only after earlier views are
\* released. Fair release alone does not prevent an earlier exhaustion kick.
Request(s, m) ==
    /\ st.phase \in {"application", "transport"} /\ st.owner[s] # 0
    /\ LET r == [conn |-> st.owner[s], msg |-> m]
       IN /\ r \notin st.requested
          /\ (~Stable \/ (\A old \in st.requested : old.conn = r.conn => old \in st.released))
          /\ st' = [st EXCEPT !.requested = @ \cup {r}]
    /\ UNCHANGED <<wire, sent, pulse>>

\* Finite tickets follow RNWindow's abstraction: recycle only after no old
\* copy or deferred feed can survive, and atomically evict the old map entry.
Referenced == {wire[i].pkt : i \in DOMAIN wire} \cup st.opened
              \cup {e.packet : e \in st.batch \cup st.deferred \cup st.todo}
TicketFree(id) == \A p \in Referenced : p.id # id

Send(p) ==
    /\ st.phase = "transport" /\ st.owner[p.slot] = p.conn
    /\ st.state[p.slot] \notin {"Exhausted", "Disconnected"}
    /\ Record(p) \in st.requested \ st.acked /\ TicketFree(p.id)
    /\ Data!Send(p)
    /\ st' = [st EXCEPT !.packetMap = {q \in @ : q.id # p.id} \cup {p}]

\* The transport opens a packet, stamps its connection at that moment and
\* appends a payload entry. Old connections are drained without application.
TransportOpen(p) ==
    /\ st.phase = "transport"
    /\ Data!Deliver(p)
    /\ IF st.owner[p.slot] = p.conn
          THEN st' = [st EXCEPT !.batch = @ \cup {[kind |-> "payload", packet |-> p]},
                               !.opened = @ \cup {p}]
          ELSE UNCHANGED st

\* This is the existing transport's acked-packet-id notification (P2/P4),
\* not a message ACK. Only an opened packet may yield one. Its original
\* connection tag survives both slot churn and deferred publication.
TransportAck(p, defer) ==
    /\ st.phase = "transport" /\ p \in st.opened
    /\ LET e == [kind |-> "ack", packet |-> p]
       IN st' = [st EXCEPT !.opened = @ \ {p},
                  !.batch = IF defer THEN @ ELSE @ \cup {e},
                  !.deferred = IF defer THEN @ \cup {e} ELSE @]
    /\ UNCHANGED <<wire, sent, pulse>>

\* The identity Observe compares. The "clientid" mutant takes the client id
\* for the connection id (sdk/rocketnet_message_server.cpp before the link
\* read the transport's P9a id): a reconnect keeps its client id, so only a
\* slot going from free to held or back looks changed.
SeenChange(s) == IF "clientid" \in Broken
                 THEN (st.owner[s] = 0) # (st.bound[s] = 0)
                 ELSE st.owner[s] # st.bound[s]

\* ResetCancel (sdk/rocketnet_message_server.cpp, rn_message_object_update:
\* rn_message_connection_cancel_at_reset before each connection reset): a
\* start pending on a slot the update resets is cancelled with that reset.
\* "pendingacrossreset" removes the cancel, the defect it fixes.
CancelsPending(owner) ==
    /\ st.pending # 0 /\ owner[st.pending] # st.bound[st.pending]
    /\ "pendingacrossreset" \notin Broken

\* Observe the final owner, reset on identity change, then admit each entry
\* against this binding. The transport cannot churn during the apply phase.
\* The reset is lazy (RN_SLOTS_DROP releases the slot in O(1)): a slot going
\* free releases its views only, and its region is emptied when the slot is
\* bound to its next connection (RN_SLOTS_RESET, rn_region_carve).
Unbinds(s) == SeenChange(s) /\ "noreset" \notin Broken
Carves(s) == Unbinds(s) /\ st.owner[s] # 0
Observe ==
    /\ st.phase = "transport"
    /\ \A s \in Slots : st.state[s] # "Disconnected"
    /\ ((\A s \in Slots : st.held[s] = {}) \/ "updatewithheld" \in Broken)
    /\ \E ready \in (IF Stable THEN {st.deferred} ELSE SUBSET st.deferred) :
       st' = [st EXCEPT !.bound = st.owner,
          !.state = [s \in Slots |-> IF st.owner[s] = 0 THEN "Free" ELSE "Connected"],
          !.reason = [s \in Slots |-> "NONE"],
          !.badLifetime = @ \/ (\E s \in Slots : st.held[s] # {}),
          !.received = [s \in Slots |-> IF Carves(s) THEN {} ELSE st.received[s]],
          !.region = [s \in Slots |-> IF Carves(s) THEN {} ELSE st.region[s]],
          !.held = [s \in Slots |-> IF Unbinds(s) THEN {} ELSE st.held[s]],
          !.packetMap = {p \in st.packetMap : p.conn = st.owner[p.slot]},
          !.changed = [s \in Slots |-> "clientid" \in Broken /\ SeenChange(s)],
          !.pending = IF CancelsPending(st.owner) THEN 0 ELSE @,
          !.pendingConn = IF CancelsPending(st.owner) THEN 0 ELSE @,
          !.todo = st.batch \cup ready, !.batch = {},
          !.deferred = @ \ ready, !.phase = "apply"]
    /\ UNCHANGED <<wire, sent, pulse>>

\* The "clientid" mutant tags each entry by the slot's state, not by the
\* connection it was received for: none (discarded) when this update's
\* Observe changed the slot, else the slot's binding.
Admits(e) == LET s == e.packet.slot
            IN st.bound[s] # 0 /\
               IF "clientid" \in Broken THEN ~st.changed[s]
               ELSE ("byslot" \in Broken \/ e.packet.conn = st.bound[s])

Admit(e) ==
    /\ st.phase = "apply" /\ e \in st.todo
    /\ LET s == e.packet.slot
           r == Record(e.packet)
           a == AuditOf(e, st.bound[s])
       IN /\ IF ~Admits(e)
                THEN st' = [st EXCEPT !.todo = @ \ {e}, !.dropped = @ \cup {a}]
                ELSE IF e.kind = "ack"
                THEN st' = [st EXCEPT !.todo = @ \ {e}, !.applied = @ \cup {a},
                       !.acked = IF e.packet \in st.packetMap THEN @ \cup {r} ELSE @]
                ELSE IF r \in st.delivered
                THEN st' = [st EXCEPT !.todo = @ \ {e}, !.applied = @ \cup {a}]
                ELSE IF Cardinality(st.region[s]) < RegionCapacity
                THEN st' = [st EXCEPT !.todo = @ \ {e}, !.applied = @ \cup {a},
                       !.region[s] = @ \cup {r}, !.held[s] = @ \cup {r},
                       !.delivered = @ \cup {r}, !.received[s] = @ \cup {r}]
                ELSE st' = [st EXCEPT !.state[s] = "Exhausted"]
    /\ UNCHANGED <<wire, sent, pulse>>

\* An exhaustion cannot return to the caller as an ignorable error. Next
\* gives Kick exclusive priority, including over the other slot's work.
\* Kick cancels a start pending on its slot (rn_message_object_kick).
Kick(s) ==
    /\ st.state[s] = "Exhausted"
    /\ st' = [st EXCEPT !.state[s] = "Disconnected", !.owner[s] = 0,
          !.bound[s] = 0, !.reason[s] = "MESSAGE_MEMORY_EXHAUSTED",
          !.pending = IF @ = s THEN 0 ELSE @,
          !.pendingConn = IF st.pending = s THEN 0 ELSE @]
    /\ UNCHANGED <<wire, sent, pulse>>

\* The application ends a slot between updates (rocketnet_server_disconnect_client
\* and rocketnet_server_disconnect_all_clients, sdk/rocketnet_api.cpp). A slot
\* the message layer holds Connected goes through Kick's path
\* (rn_message_object_kick, sdk/rocketnet_message_server.cpp): the message
\* layer cancels a start pending on the slot, marks it ended with reason NONE
\* and the transport's disconnect runs through the link; ResetFree resets it at
\* the next update. A slot only the transport holds is disconnected at the
\* transport. Broken "disconnectbypasseskick" ends the slot at the transport
\* only, leaving the message layer Connected until the next Observe.
PublicDisconnect(s) ==
    /\ ~Stable /\ st.phase = "application"
    /\ st.owner[s] # 0 \/ st.state[s] = "Connected"
    /\ IF st.state[s] = "Connected" /\ "disconnectbypasseskick" \notin Broken
          THEN st' = [st EXCEPT !.state[s] = "Disconnected", !.owner[s] = 0,
                      !.bound[s] = 0, !.reason[s] = "NONE",
                      !.pending = IF @ = s THEN 0 ELSE @,
                      !.pendingConn = IF st.pending = s THEN 0 ELSE @]
          ELSE st' = [st EXCEPT !.owner[s] = 0]
    /\ UNCHANGED <<wire, sent, pulse>>

CarryOn(s) ==
    /\ "carryon" \in Broken /\ st.state[s] = "Exhausted"
    /\ st' = [st EXCEPT !.state[s] = "Connected", !.badKick = TRUE]
    /\ UNCHANGED <<wire, sent, pulse>>

\* The lazy reset (rn_message_connection_release,
\* sdk/rocketnet_message_server.cpp): the ended slot is freed in O(1) -- its
\* views released, the start pending on it cancelled -- and its region and
\* received records stay as they were, unreachable: a Free slot is bound to
\* no connection, so Admits discards every entry for it, and the region is
\* emptied by the Observe that binds the slot to its next connection.
ResetFree(s) ==
    /\ st.state[s] = "Disconnected"
    /\ LET cancel == st.pending = s /\ "pendingacrossreset" \notin Broken
       IN st' = [st EXCEPT !.state[s] = "Free", !.held[s] = {},
                 !.pending = IF cancel THEN 0 ELSE @,
                 !.pendingConn = IF cancel THEN 0 ELSE @]
    /\ UNCHANGED <<wire, sent, pulse>>

\* The application's queued-path send (rn_message_object_start, finish and
\* abort): one start pending per object, on a connected slot; a start while
\* one is pending is refused (no step). The pending start outlives an update
\* unless that update resets its slot (ResetCancel, Kick).
StartPending(s) ==
    /\ st.phase = "application" /\ st.state[s] = "Connected" /\ st.pending = 0
    /\ st' = [st EXCEPT !.pending = s, !.pendingConn = st.bound[s]]
    /\ UNCHANGED <<wire, sent, pulse>>

\* A finish ends the pending start whether it is accepted or refused: a
\* refused bytes_written aborts the start (rn_message_object_finish), so the
\* refused and accepted finishes take the same step here.
FinishPending(s) ==
    /\ st.phase = "application" /\ st.pending = s
    /\ st' = [st EXCEPT !.pending = 0, !.pendingConn = 0]
    /\ UNCHANGED <<wire, sent, pulse>>

Release(s, r) ==
    /\ st.phase = "application"
    /\ r \in st.held[s]
    /\ st' = [st EXCEPT !.held[s] = @ \ {r}, !.released = @ \cup {r},
                  !.region[s] = IF "releaseignored" \in Broken THEN @ ELSE @ \ {r}]
    /\ UNCHANGED <<wire, sent, pulse>>

\* The application sees this update's views only after the update returns.
EndUpdate ==
    /\ st.phase = "apply" /\ st.todo = {}
    /\ st' = [st EXCEPT !.phase = "application"]
    /\ UNCHANGED <<wire, sent, pulse>>

\* The release precondition is checked before the transport runs, not after
\* it might mutate the batch underlying the previous update's views.
BeginUpdate ==
    /\ st.phase = "application"
    /\ ((\A s \in Slots : st.held[s] = {}) \/ "updatewithheld" \in Broken)
    /\ st' = [st EXCEPT !.phase = "transport",
               !.badLifetime = @ \/ (\E s \in Slots : st.held[s] # {})]
    /\ UNCHANGED <<wire, sent, pulse>>

\* Stable's application obeys the release-before-update precondition. The
\* general safety model also explores callers that violate it.
RejectUnreleasedUpdate ==
    /\ ~Stable /\ "updatewithheld" \notin Broken
    /\ st.phase = "application" /\ (\E s \in Slots : st.held[s] # {})
    /\ st' = [st EXCEPT !.error = "ROCKETNET_UPDATE_MESSAGE_NOT_RELEASED"]
    /\ UNCHANGED <<wire, sent, pulse>>

Environment == Data!Environment /\ UNCHANGED st
Exhausted == {s \in Slots : st.state[s] = "Exhausted"}
Next == IF st.error # "NONE" THEN FALSE
        ELSE IF Exhausted # {}
        THEN \E s \in Exhausted : Kick(s) \/ CarryOn(s)
        ELSE \/ \E s \in Slots : Connect(s) \/ Disconnect(s) \/ ResetFree(s)
                    \/ PublicDisconnect(s) \/ StartPending(s) \/ FinishPending(s)
             \/ \E s \in Slots, m \in 1..MaxMessages : Request(s, m)
             \/ \E p \in Packets : Send(p) \/ TransportOpen(p)
                    \/ TransportAck(p, TRUE) \/ TransportAck(p, FALSE)
             \/ BeginUpdate \/ Observe \/ RejectUnreleasedUpdate \/ EndUpdate \/ Environment
             \/ \E e \in st.todo : Admit(e)
             \/ \E s \in Slots : \E r \in st.held[s] : Release(s, r)

Spec == Init /\ [][Next]_vars
\* Stable runs publish every deferred transport notification at the next
\* update; safety permits arbitrary deferred subsets. Retries and update
\* boundaries are fair; Link adds eventual delivery for
\* packets sent infinitely often. Release is an application obligation.
Fairness == /\ Data!Fairness
            /\ WF_vars(BeginUpdate) /\ WF_vars(Observe) /\ WF_vars(EndUpdate)
            /\ \A e \in Entry : WF_vars(Admit(e))
            /\ \A p \in Packets : SF_vars(Send(p))
                    /\ SF_vars(TransportAck(p, FALSE))
            /\ \A s \in Slots : WF_vars(Kick(s)) /\ WF_vars(ResetFree(s))
            /\ \A s \in Slots, r \in Records : WF_vars(Release(s, r))
            /\ \A s \in Slots, m \in 1..MaxMessages : SF_vars(Request(s, m))
LiveSpec == Spec /\ Fairness

TypeOK ==
    /\ Data!TypeOK /\ Data!InFlightBounded
    /\ st.owner \in [Slots -> 0..MaxConn] /\ st.bound \in [Slots -> 0..MaxConn]
    /\ st.nextConn \in 1..MaxConn+1
    /\ st.state \in [Slots -> {"Free", "Connected", "Exhausted", "Disconnected"}]
    /\ st.reason \in [Slots -> {"NONE", "MESSAGE_MEMORY_EXHAUSTED"}]
    /\ st.region \in [Slots -> SUBSET Records] /\ st.held \in [Slots -> SUBSET Records]
    /\ st.received \in [Slots -> SUBSET Records]
    /\ st.phase \in {"application", "transport", "apply"}
    /\ st.batch \subseteq Entry /\ st.deferred \subseteq Entry /\ st.todo \subseteq Entry
    /\ st.opened \subseteq Packets /\ st.packetMap \subseteq Packets
    /\ st.requested \subseteq Records /\ st.delivered \subseteq Records
    /\ st.released \subseteq Records /\ st.acked \subseteq Records
    /\ st.applied \subseteq Audit /\ st.dropped \subseteq Audit
    /\ st.changed \in [Slots -> BOOLEAN]
    /\ st.badKick \in BOOLEAN /\ st.badLifetime \in BOOLEAN
    /\ st.error \in {"NONE", "ROCKETNET_UPDATE_MESSAGE_NOT_RELEASED"}
    /\ st.pending \in 0..SlotCount /\ st.pendingConn \in 0..MaxConn

NoCrossConnectionApply == \A a \in st.applied : a.from = a.to
NoOwnEntryDropped == \A d \in st.dropped : d.from # d.to
\* A Free slot is bound to no connection and holds no view, so what its
\* region still holds (the lazy reset) is reachable by nothing; a Connected
\* slot holds records of its bound connection only.
ReusedSlotStartsClean == \A s \in Slots :
    /\ (st.state[s] = "Free" => st.bound[s] = 0 /\ st.held[s] = {})
    /\ (st.state[s] = "Connected" => \A r \in st.region[s] \cup st.received[s] : r.conn = st.bound[s])
ReleasedRecordsReusable == \A s \in Slots :
    /\ st.held[s] \subseteq st.region[s]
    /\ st.region[s] \cap st.released = {}
    /\ Cardinality(st.region[s]) <= RegionCapacity
ExhaustionOnlyLeadsToDisconnect ==
    [][\A s \in Slots : st.state[s] = "Exhausted" =>
         st'.state[s] = "Disconnected" /\ st'.reason[s] = "MESSAGE_MEMORY_EXHAUSTED"]_vars
\* Between updates the message layer reports a slot connected
\* (rocketnet_server_client_connected) and accepts a start on it
\* (rn_message_object_start, sdk/rocketnet_message_server.cpp) from one
\* field, the connection state Connected; a slot the transport has ended
\* (owner 0) is neither.
ReportedConnected(s) == st.state[s] = "Connected"
AcceptsSend(s) == st.state[s] = "Connected"
TransportEndedSlotEndedAtOnce == st.phase = "application" =>
    \A s \in Slots : st.owner[s] = 0 => ~ReportedConnected(s) /\ ~AcceptsSend(s)
UpdateRequiresReleasedViews == ~st.badLifetime
\* No start stays pending across a reset of its slot: a pending start belongs
\* to the connection its slot is bound to, so no start on any slot is refused
\* by one whose connection is gone.
NoPendingAcrossReset ==
    st.pending # 0 => st.pendingConn # 0 /\ st.bound[st.pending] = st.pendingConn
EveryReliableDelivered == \A r \in Records : (r \in st.requested) ~> (r \in st.delivered)
=============================================================================
