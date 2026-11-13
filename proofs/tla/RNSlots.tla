---------------------------- MODULE RNSlots ----------------------------
\* Connection-tagged transport batches, region lifetime and object updates.
\* CHANNELS.md 10.2 and 15.1 supply Observe/Admit; the exhaustion and explicit
\* release rules extend that draft. Views are released before the next object
\* update; an update with a view still held takes the view back, returns
\* ROCKETNET_UPDATE_MESSAGE_NOT_RELEASED and continues
\* (RejectUnreleasedUpdate, rn_message_object_update_begin). Release returns
\* the view only; the region's charge for every delivery of the last update
\* returns when the next update begins (rn_endpoint_begin_update), released
\* or not. A queued-path start is pending per connection (open.pending in
\* rn_message_object_start): starts on different slots coexist.
\*
\* The disconnect reason (rocketnet_server_disconnect_reason): the transport
\* records why each slot's connection ended (ended, the protocol's
\* end_reasons: TIMED_OUT, CLIENT_DISCONNECTED, SERVER_DISCONNECTED, or
\* PROTOCOL_VIOLATION for the integrity limit) and clears it when the slot is
\* next assigned. A
\* message-layer end (Kick, PublicDisconnect) records its own reason in the
\* slot. Observe reads the transport's reason for a slot it held that it
\* finds free, NONE for a slot it binds to a new connection, and keeps every
\* other slot's reason. Broken "dropreadsnone" drops the slot with NONE (the
\* code before the transport carried its reason); "observeclearsreason"
\* clears every reason at each Observe, the rule this model held before.
\*
\* A slot whose connection ends (Disconnect, Kick, PublicDisconnect) is held
\* (freed) until the next Observe: the transport publishes its freed slots
\* at the end of its update (rocketnet_client_slots_publish in
\* rocketnet_server_protocol_update), and Observe follows it before any
\* other assign, so Connect takes no slot freed since the last Observe.
\* Broken "reuseinupdate" lets Connect take it (the code before the hold).
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
\* The integrity limit's end: PROTOCOL_VIOLATION; Broken "integritynone"
\* records NONE (the code before 1.1.2).
IntegrityEnd == IF "integritynone" \in Broken THEN "NONE" ELSE "PROTOCOL_VIOLATION"
TransportEnds == {"TIMED_OUT", "CLIENT_DISCONNECTED", IntegrityEnd}
Reasons == {"NONE", "MESSAGE_MEMORY_EXHAUSTED", "SERVER_DISCONNECTED",
            "TIMED_OUT", "CLIENT_DISCONNECTED", "PROTOCOL_VIOLATION"}
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
             ended |-> [s \in Slots |-> "NONE"],
             region |-> [s \in Slots |-> {}], held |-> [s \in Slots |-> {}],
             received |-> [s \in Slots |-> {}],
             phase |-> "application", batch |-> {}, deferred |-> {}, todo |-> {},
             requested |-> {}, delivered |-> {}, released |-> {}, acked |-> {},
             opened |-> {}, packetMap |-> {}, applied |-> {}, dropped |-> {},
             changed |-> [s \in Slots |-> FALSE],
             freed |-> [s \in Slots |-> FALSE],
             badKick |-> FALSE, badLifetime |-> FALSE, error |-> "NONE",
             pending |-> [s \in Slots |-> FALSE],
             pendingConn |-> [s \in Slots |-> 0]]

\* Transport connection ids never repeat in the bounded lifetime. Several
\* connect/disconnect steps may precede a single Observe, but a slot freed
\* since the last Observe is held: it is not connected again before the next.
\* A new connection has not ended: its slot's transport reason is NONE.
Connect(s) ==
    /\ ~Stable /\ st.phase = "transport" /\ st.owner[s] = 0
    /\ st.state[s] \notin {"Exhausted", "Disconnected"}
    /\ (~st.freed[s] \/ "reuseinupdate" \in Broken)
    /\ st.nextConn <= MaxConn
    /\ st' = [st EXCEPT !.owner[s] = st.nextConn, !.nextConn = @ + 1,
                        !.ended[s] = "NONE"]
    /\ UNCHANGED <<wire, sent, pulse>>

\* The transport ends the slot's connection on its own (the timeout sweep,
\* the client's disconnect packet, the integrity limit) and records why.
Disconnect(s, why) ==
    /\ ~Stable /\ st.phase = "transport" /\ st.owner[s] # 0
    /\ st' = [st EXCEPT !.owner[s] = 0, !.ended[s] = why, !.freed[s] = TRUE]
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
CancelsPending(s) ==
    /\ st.pending[s] /\ st.owner[s] # st.bound[s]
    /\ "pendingacrossreset" \notin Broken

\* The reason Observe leaves in a slot: the transport's for a slot it held
\* and finds free, NONE for a slot bound to a new connection, else kept.
ObservedReason(s) ==
    IF SeenChange(s)
    THEN IF st.owner[s] = 0
         THEN IF "dropreadsnone" \in Broken THEN "NONE" ELSE st.ended[s]
         ELSE "NONE"
    ELSE IF "observeclearsreason" \in Broken THEN "NONE" ELSE st.reason[s]

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
    /\ \E ready \in (IF Stable THEN {st.deferred} ELSE SUBSET st.deferred) :
       st' = [st EXCEPT !.bound = st.owner,
          !.state = [s \in Slots |-> IF st.owner[s] = 0 THEN "Free" ELSE "Connected"],
          !.reason = [s \in Slots |-> ObservedReason(s)],
          !.badLifetime = @ \/ (\E s \in Slots : st.held[s] # {}),
          !.received = [s \in Slots |-> IF Carves(s) THEN {} ELSE st.received[s]],
          !.region = [s \in Slots |-> IF Carves(s) THEN {} ELSE st.region[s]],
          !.held = [s \in Slots |-> IF Unbinds(s) THEN {} ELSE st.held[s]],
          !.packetMap = {p \in st.packetMap : p.conn = st.owner[p.slot]},
          !.changed = [s \in Slots |-> "clientid" \in Broken /\ SeenChange(s)],
          !.freed = [s \in Slots |-> FALSE],
          !.pending = [s \in Slots |-> IF CancelsPending(s) THEN FALSE ELSE st.pending[s]],
          !.pendingConn = [s \in Slots |-> IF CancelsPending(s) THEN 0 ELSE st.pendingConn[s]],
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
\* Kick cancels a start pending on its slot (rn_message_object_kick); the
\* transport's disconnect through the link records SERVER_DISCONNECTED there.
Kick(s) ==
    /\ st.state[s] = "Exhausted"
    /\ st' = [st EXCEPT !.state[s] = "Disconnected", !.owner[s] = 0,
          !.bound[s] = 0, !.reason[s] = "MESSAGE_MEMORY_EXHAUSTED",
          !.ended[s] = "SERVER_DISCONNECTED", !.freed[s] = TRUE,
          !.pending[s] = FALSE, !.pendingConn[s] = 0]
    /\ UNCHANGED <<wire, sent, pulse>>

\* The application ends a slot between updates (rocketnet_server_disconnect_client
\* and rocketnet_server_disconnect_all_clients, sdk/rocketnet_api.cpp). A slot
\* the message layer holds Connected goes through Kick's path
\* (rn_message_object_kick, sdk/rocketnet_message_server.cpp): the message
\* layer cancels a start pending on the slot, marks it ended with reason
\* SERVER_DISCONNECTED and the transport's disconnect runs through the link
\* (which records SERVER_DISCONNECTED as the transport's reason); ResetFree
\* resets it at the next update. A slot only the transport holds is
\* disconnected at the transport, which records the same reason. Broken
\* "disconnectbypasseskick" ends the slot at the transport only, leaving the
\* message layer Connected until the next Observe.
PublicDisconnect(s) ==
    /\ ~Stable /\ st.phase = "application"
    /\ st.owner[s] # 0 \/ st.state[s] = "Connected"
    /\ IF st.state[s] = "Connected" /\ "disconnectbypasseskick" \notin Broken
          THEN st' = [st EXCEPT !.state[s] = "Disconnected", !.owner[s] = 0,
                      !.bound[s] = 0, !.reason[s] = "SERVER_DISCONNECTED",
                      !.ended[s] = "SERVER_DISCONNECTED", !.freed[s] = TRUE,
                      !.pending[s] = FALSE, !.pendingConn[s] = 0]
          ELSE st' = [st EXCEPT !.owner[s] = 0, !.ended[s] = "SERVER_DISCONNECTED",
                      !.freed[s] = (st.owner[s] # 0) \/ @]
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
    /\ LET cancel == st.pending[s] /\ "pendingacrossreset" \notin Broken
       IN st' = [st EXCEPT !.state[s] = "Free", !.held[s] = {},
                 !.pending[s] = IF cancel THEN FALSE ELSE @,
                 !.pendingConn[s] = IF cancel THEN 0 ELSE @]
    /\ UNCHANGED <<wire, sent, pulse>>

\* The application's queued-path send (rn_message_object_start, finish and
\* abort): one start pending per connection (open.pending), on a connected
\* slot; a second start on the same slot while one is pending there is
\* refused (no step). Starts on other slots are independent. The pending
\* start outlives an update unless that update resets its slot (ResetCancel,
\* Kick).
StartPending(s) ==
    /\ st.phase = "application" /\ st.state[s] = "Connected" /\ ~st.pending[s]
    /\ st' = [st EXCEPT !.pending[s] = TRUE, !.pendingConn[s] = st.bound[s]]
    /\ UNCHANGED <<wire, sent, pulse>>

\* A finish ends the pending start whether it is accepted or refused: a
\* refused bytes_written aborts the start (rn_message_object_finish), so the
\* refused and accepted finishes take the same step here.
FinishPending(s) ==
    /\ st.phase = "application" /\ st.pending[s]
    /\ st' = [st EXCEPT !.pending[s] = FALSE, !.pendingConn[s] = 0]
    /\ UNCHANGED <<wire, sent, pulse>>

\* rn_message_connection_release_view (sdk/rocketnet_message_server.cpp): the held view
\* is marked released and the held count drops; the region is not touched.
\* Broken "releaseignored" records the release but leaves the view held.
Release(s, r) ==
    /\ st.phase = "application"
    /\ r \in st.held[s]
    /\ st' = [st EXCEPT !.held[s] = IF "releaseignored" \in Broken THEN @ ELSE @ \ {r},
                  !.released = @ \cup {r}]
    /\ UNCHANGED <<wire, sent, pulse>>

\* The application sees this update's views only after the update returns.
EndUpdate ==
    /\ st.phase = "apply" /\ st.todo = {}
    /\ st' = [st EXCEPT !.phase = "application"]
    /\ UNCHANGED <<wire, sent, pulse>>

\* rn_message_object_update_begin, before the transport runs: every
\* connection's deliveries of the last update expire and their receive
\* descriptors return to the region (rn_endpoint_begin_update), whether or
\* not the application released them. Broken "noexpire" keeps the charge.
Expired(s) == IF "noexpire" \in Broken THEN st.region[s] ELSE {}

\* The update with every view released: result ROCKETNET_OK.
BeginUpdate ==
    /\ st.phase = "application"
    /\ \A s \in Slots : st.held[s] = {}
    /\ st' = [st EXCEPT !.phase = "transport", !.error = "NONE",
               !.region = [s \in Slots |-> Expired(s)]]
    /\ UNCHANGED <<wire, sent, pulse>>

\* The update with a view still held (the application's misuse): it counts
\* MESSAGE_NOT_RELEASED, asserts in a debug build, takes every view back
\* (held = 0 in rn_message_object_update_begin), runs the update and returns
\* ROCKETNET_UPDATE_MESSAGE_NOT_RELEASED. No view is carried into the
\* transport phase. Stable's application never takes it. Broken
\* "updatewithheld" carries the views into the transport phase.
\* Misuse: whether the application may call an update with a view held. Every
\* safety control explores it but the two larger ones (MCRNSlotsTwoSlots,
\* MCRNSlotsExhaustion), whose state space the continued update after the
\* misuse takes past the 110-second budget; they override it to FALSE
\* (MCNoMisuse), so their application releases before each update, as the
\* stable one does.
Misuse == TRUE
RejectUnreleasedUpdate ==
    /\ ~Stable /\ Misuse
    /\ st.phase = "application" /\ (\E s \in Slots : st.held[s] # {})
    /\ LET keep == "updatewithheld" \in Broken
       IN st' = [st EXCEPT !.phase = "transport",
                 !.error = "ROCKETNET_UPDATE_MESSAGE_NOT_RELEASED",
                 !.held = IF keep THEN @ ELSE [s \in Slots |-> {}],
                 !.badLifetime = @ \/ keep,
                 !.region = [s \in Slots |-> Expired(s)]]
    /\ UNCHANGED <<wire, sent, pulse>>

Environment == Data!Environment /\ UNCHANGED st
Exhausted == {s \in Slots : st.state[s] = "Exhausted"}
Next == IF Exhausted # {}
        THEN \E s \in Exhausted : Kick(s) \/ CarryOn(s)
        ELSE \/ \E s \in Slots : Connect(s) \/ (\E why \in TransportEnds : Disconnect(s, why)) \/ ResetFree(s)
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
    /\ st.reason \in [Slots -> Reasons]
    /\ st.ended \in [Slots -> Reasons]
    /\ st.region \in [Slots -> SUBSET Records] /\ st.held \in [Slots -> SUBSET Records]
    /\ st.received \in [Slots -> SUBSET Records]
    /\ st.phase \in {"application", "transport", "apply"}
    /\ st.batch \subseteq Entry /\ st.deferred \subseteq Entry /\ st.todo \subseteq Entry
    /\ st.opened \subseteq Packets /\ st.packetMap \subseteq Packets
    /\ st.requested \subseteq Records /\ st.delivered \subseteq Records
    /\ st.released \subseteq Records /\ st.acked \subseteq Records
    /\ st.applied \subseteq Audit /\ st.dropped \subseteq Audit
    /\ st.changed \in [Slots -> BOOLEAN]
    /\ st.freed \in [Slots -> BOOLEAN]
    /\ st.badKick \in BOOLEAN /\ st.badLifetime \in BOOLEAN
    /\ st.error \in {"NONE", "ROCKETNET_UPDATE_MESSAGE_NOT_RELEASED"}
    /\ st.pending \in [Slots -> BOOLEAN] /\ st.pendingConn \in [Slots -> 0..MaxConn]

NoCrossConnectionApply == \A a \in st.applied : a.from = a.to
NoOwnEntryDropped == \A d \in st.dropped : d.from # d.to
\* A Free slot is bound to no connection and holds no view, so what its
\* region still holds (the lazy reset) is reachable by nothing; a Connected
\* slot holds records of its bound connection only.
ReusedSlotStartsClean == \A s \in Slots :
    /\ (st.state[s] = "Free" => st.bound[s] = 0 /\ st.held[s] = {})
    /\ (st.state[s] = "Connected" => \A r \in st.region[s] \cup st.received[s] : r.conn = st.bound[s])
\* A held view is charged to the region; the charge of every delivery of the
\* last update, released or taken back, is returned before the transport
\* runs (MCRNSlotsBrokenNoExpire expects TLC to violate it).
ReleasedRecordsReusable == \A s \in Slots :
    /\ st.held[s] \subseteq st.region[s]
    /\ (st.phase = "transport" => st.region[s] = {})
    /\ Cardinality(st.region[s]) <= RegionCapacity
\* A released view is no longer held (rn_message_connection_release_view marks it
\* released and drops the held count). MCRNSlotsBrokenReleaseIgnored expects
\* TLC to violate it.
ReleaseReturnsView == \A s \in Slots : st.held[s] \cap st.released = {}
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
\* No view is carried into an update's transport phase: an update with a view
\* still held takes it back (RejectUnreleasedUpdate). The name is kept from
\* when the model ended at that misuse.
UpdateRequiresReleasedViews == ~st.badLifetime
\* No start stays pending across a reset of its slot: a pending start belongs
\* to the connection its slot is bound to, so no later start on that slot is
\* refused by one whose connection is gone.
NoPendingAcrossReset == \A s \in Slots :
    st.pending[s] => st.pendingConn[s] # 0 /\ st.bound[s] = st.pendingConn[s]
\* A reachability probe, not a safety claim: MCRNSlotsTwoPending expects TLC
\* to violate it, showing starts pending on two slots at once (the code's one
\* start per connection, not one per object).
NotBothPending == ~(\A s \in Slots : st.pending[s])
EveryReliableDelivered == \A r \in Records : (r \in st.requested) ~> (r \in st.delivered)
\* A connection the message layer holds has not ended: its reason is NONE.
ConnectedReadsNone == \A s \in Slots : st.state[s] = "Connected" => st.reason[s] = "NONE"
\* A slot Observe finds dropped reads the reason the transport's ending path
\* recorded (rocketnet_server_disconnect_reason after a timeout, the client's
\* disconnect, or a disconnect at the transport alone).
\* MCRNSlotsBrokenDropReadsNone expects TLC to violate it.
DropReadsTransportEnd ==
    [][st.phase = "transport" /\ st'.phase = "apply" =>
         \A s \in Slots : st.bound[s] # 0 /\ st.owner[s] = 0 => st'.reason[s] = st.ended[s]]_vars
\* A slot's reason changes only when its connection ends (Kick,
\* PublicDisconnect) or when Observe changes its binding: it is kept until
\* the slot's next connection. MCRNSlotsBrokenObserveClearsReason expects TLC
\* to violate it.
ReasonKeptUntilNextConnection ==
    [][\A s \in Slots : st'.reason[s] # st.reason[s] =>
         st'.bound[s] # st.bound[s] \/ st'.state[s] = "Disconnected"]_vars
\* Every end is seen: Observe finds each slot whose connection ended since the
\* last Observe still free, so after that update the slot reads not
\* connected with the end's reason, never the NONE of a next connection
\* (rocketnet_server_disconnect_reason). MCRNSlotsBrokenReuseInUpdate expects
\* TLC to violate it.
EveryEndSeen ==
    [][st.phase = "transport" /\ st'.phase = "apply" =>
         \A s \in Slots : st.freed[s] => st.owner[s] = 0]_vars
\* Every transport end is named: a slot Observe finds dropped reads a reason
\* other than NONE. MCRNSlotsBrokenIntegrityNone expects TLC to violate it.
TransportEndNamed ==
    [][st.phase = "transport" /\ st'.phase = "apply" =>
         \A s \in Slots : st.bound[s] # 0 /\ st.owner[s] = 0 => st'.reason[s] # "NONE"]_vars
=============================================================================
