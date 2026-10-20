--------------------------- MODULE ServerSession ---------------------------
\* The server object's life: create (STOPPED, no transport bound), start,
\* clients connecting into free slots, sending and receiving, leaving by their
\* own disconnect, a timeout, a denial, disconnect_client or
\* disconnect_all_clients, stop (every client disconnected, STOPPED), start
\* again, destroy. The handshake packet by packet is tla/ServerConnection.tla;
\* this module models the slot's life across many clients and the single
\* per-slot Reset(i) between them.
\*
\* Every per-connection field of a slot holds 0 (Init) or the tag of the
\* session that wrote it. Session tags never repeat. Server-wide state is
\* `kept`: the cookie key and server keys, the lifetime counters and the
\* config, kept across slots, stops and starts; and the used-token history,
\* which is per run: each start begins with an empty history (a start builds
\* a new server transport, which creates it), and slot resets and the stop
\* keep it within the run (standard/TRANSPORT.md t13.4). The disconnect
\* reason of each slot is kept through its reset and cleared when the slot's
\* next client is established.
\*
\* A message send to a slot that is not CONNECTED, on a running or a stopped
\* server, is dropped (ServerDropSend): it changes no slot field and nothing
\* of it is held for a later send (standard/MESSAGES.md m1.5). The ghost
\* droppedIn holds the slot state of the drop the last step made, {} after
\* any other step.
\*
\* Reversed witnesses, one constant: Defect = "unresetslot" makes Reset(i)
\* skip the field UnresetField; "stopleavesclient" makes stop skip the last
\* slot; "touchesotherslot" makes Reset(i) also clear slot i+1;
\* "resetclearshistory" makes Reset(i) empty the used-token history;
\* "sendbeforeconnected" lets ServerSend(i) write a slot that is not
\* CONNECTED, a send held for a client not yet connected.
EXTENDS Naturals, FiniteSets

CONSTANTS SlotCount, MaxSessions, MaxTokens, MaxStarts, MaxCreates, Defect, UnresetField

ASSUME Defect \in {"none", "unresetslot", "stopleavesclient", "touchesotherslot",
                   "resetclearshistory", "sendbeforeconnected"}
ASSUME SlotCount \in Nat \ {0} /\ MaxSessions \in Nat \ {0}

\* One field per row of the per-connection reset inventory; on the server
\* token_state is the slot's challenge, cookie and token state.
MappingFields == {"token_state", "client_id", "user_data", "timeouts", "session_keys"}
SendFields == {"packet_sequence", "message_sequence", "sent_packets",
               "reliable_send_queue", "ordering_indices", "send_budget",
               "connection_counters", "ack_receipt_ring"}
RecvFields == {"received_packets", "reliable_receive_queue", "ack_bits",
               "replay_window", "fragment_reassembly", "rtt_estimator",
               "loss_estimator", "connection_counters"}
DisconnectFields == {"pending_disconnect"}
Fields == MappingFields \cup SendFields \cup RecvFields \cup DisconnectFields
ASSUME UnresetField \in Fields

EstablishReads == MappingFields
SendReads == SendFields \cup {"ack_bits", "session_keys", "pending_disconnect", "timeouts"}
RecvReads == RecvFields \cup {"sent_packets", "ack_receipt_ring", "session_keys", "timeouts"}
ASSUME EstablishReads \cup SendReads \cup RecvReads = Fields

Slots == 1..SlotCount
Sessions == 1..MaxSessions
Tags == 0..MaxSessions
Tokens == 1..MaxTokens
ServerStates == {"NONE", "STOPPED", "RUNNING", "STOPPING"}
SlotStates == {"FREE", "HANDSHAKE", "CONNECTED"}
Reasons == {"NONE", "CLIENT_DISCONNECTED", "TIMED_OUT", "DENIED",
            "SERVER_DISCONNECTED", "SERVER_STOPPED"}

InitRec == [f \in Fields |-> 0]
EmptySlot == [st |-> "FREE", sess |-> 0, rec |-> InitRec, reason |-> "NONE"]
FreshSlots == [i \in Slots |-> EmptySlot]
Write(r, fs, s) == [f \in Fields |-> IF f \in fs THEN s ELSE r[f]]
StaleIn(r, fs, s) == {f \in fs : r[f] \notin {0, s}}

\* The one per-slot reset function: the slot free, every per-connection
\* field Init, the reason the ending path names kept.
Clean(sl, why) ==
    [st |-> "FREE", sess |-> 0, reason |-> why,
     rec |-> [f \in Fields |-> IF Defect = "unresetslot" /\ f = UnresetField
                               THEN sl.rec[f] ELSE 0]]
Reset(v, i, why) ==
    IF Defect = "touchesotherslot" /\ i + 1 \in Slots
    THEN [v EXCEPT ![i] = Clean(v[i], why), ![i + 1] = Clean(v[i + 1], v[i + 1].reason)]
    ELSE [v EXCEPT ![i] = Clean(v[i], why)]

VARIABLES state, slot, kept, draining, link, nextSess, home, tokOf, starts, creates,
          lastEnd, established, estTokens, doubleTok, observed, crossDelivered,
          collateral, silentEnd, startFrom, queuedIn, priorTokens, readmitted, droppedIn
\* state, slot, kept, draining: the server object (draining is a
\*   disconnect_all_clients in progress)
\* link: the session tags of client packets in flight to the server (lossy)
\* nextSess, home, tokOf, starts, creates: the environment (home[s] is the
\*   slot session s connected into, tokOf[s] the token it presented)
\* the rest: ghosts (estTokens: the tokens established in this run;
\*   priorTokens: those established in earlier runs of this object;
\*   readmitted: a token of an earlier run was established again)
vars == <<state, slot, kept, draining, link, nextSess, home, tokOf, starts, creates,
          lastEnd, established, estTokens, doubleTok, observed, crossDelivered,
          collateral, silentEnd, startFrom, queuedIn, priorTokens, readmitted, droppedIn>>
env == <<link, nextSess, home, tokOf, starts, creates>>
ghosts == <<estTokens, doubleTok, observed, crossDelivered, collateral, silentEnd, startFrom, queuedIn,
            priorTokens, readmitted>>

NoKept == [used |-> {}, lifetime |-> 0, keys |-> 0, config |-> 0]

Init ==
    /\ state = "NONE" /\ slot = FreshSlots /\ kept = NoKept /\ draining = FALSE
    /\ link = {} /\ nextSess = 1 /\ home = [s \in Sessions |-> 0]
    /\ tokOf = [s \in Sessions |-> 0] /\ starts = 0 /\ creates = 0
    /\ lastEnd = [i \in Slots |-> "NONE"] /\ established = 0 /\ estTokens = {}
    /\ doubleTok = FALSE /\ observed = {} /\ crossDelivered = {} /\ collateral = {}
    /\ silentEnd = FALSE /\ startFrom = {} /\ queuedIn = {}
    /\ priorTokens = {} /\ readmitted = FALSE /\ droppedIn = {}

\* Every path out of a slot's connection: the one Reset(i). sendDisc says
\* whether the path sends the disconnect burst to a connected client. The
\* reset keeps the used-token history (rocketnet_server_connection_reset,
\* sdk/rocketnet_connection_reset.cpp:61, touches no history); the defect
\* "resetclearshistory" empties it.
EndSlot(v, i, why, sendDisc) ==
    LET w == Reset(v, i, why)
    IN /\ slot' = w
       /\ kept' = IF Defect = "resetclearshistory" THEN [kept EXCEPT !.used = {}] ELSE kept
       /\ lastEnd' = [lastEnd EXCEPT ![i] = why]
       /\ collateral' = collateral \cup {j \in Slots \ {i} : w[j] # slot[j]}
       /\ silentEnd' = (silentEnd \/ (~sendDisc /\ slot[i].st = "CONNECTED"
                                       /\ why \in {"SERVER_DISCONNECTED", "SERVER_STOPPED"}))

\* rocketnet_server_create: STOPPED, no transport bound, a fresh object
\* with no used-token history (sdk/rocketnet_api.cpp:1111; the history
\* exists only with a running transport).
Create ==
    /\ state = "NONE" /\ creates < MaxCreates
    /\ state' = "STOPPED" /\ slot' = FreshSlots /\ draining' = FALSE
    /\ kept' = [used |-> {}, lifetime |-> 0, keys |-> 1, config |-> 1]
    /\ creates' = creates + 1 /\ starts' = 0
    /\ lastEnd' = [i \in Slots |-> "NONE"] /\ established' = 0 /\ estTokens' = {}
    /\ priorTokens' = {}
    /\ UNCHANGED <<link, nextSess, home, tokOf, doubleTok, observed, crossDelivered,
                   collateral, silentEnd, startFrom, queuedIn, readmitted>>

\* Destroy from any state (sdk/rocketnet_api.cpp:1198, which stops first,
\* destroying the history with the transport). Client packets in flight
\* outlive the object.
Destroy ==
    /\ state # "NONE"
    /\ state' = "NONE" /\ slot' = FreshSlots /\ kept' = NoKept /\ draining' = FALSE
    /\ lastEnd' = [i \in Slots |-> "NONE"] /\ established' = 0 /\ estTokens' = {}
    /\ priorTokens' = {}
    /\ UNCHANGED <<env, doubleTok, observed, crossDelivered, collateral, silentEnd, startFrom, queuedIn,
                   readmitted>>

\* start: refused with a status code unless STOPPED (a stutter). A start
\* builds a new message server and server transport (sdk/rocketnet_api.cpp:1149,
\* sdk/rocketnet_message_server.cpp:1035), and the transport creates an empty
\* used-token history (sdk/rocketnet_server_transport.cpp:1439): the run
\* begins with a fresh history; the previous run's tokens become priorTokens.
Start ==
    /\ state = "STOPPED" /\ starts < MaxStarts
    /\ state' = "RUNNING" /\ starts' = starts + 1
    /\ startFrom' = startFrom \cup {state}
    /\ kept' = [kept EXCEPT !.used = {}]
    /\ estTokens' = {} /\ priorTokens' = priorTokens \cup estTokens
    /\ UNCHANGED <<slot, draining, link, nextSess, home, tokOf, creates, lastEnd,
                   established, doubleTok, observed, crossDelivered,
                   collateral, silentEnd, queuedIn, readmitted>>

\* stop: no client is accepted from here; every slot is disconnected by
\* DrainStep, then StopComplete returns to STOPPED. The code destroys the
\* history with the transport at the end of stop (sdk/rocketnet_api.cpp:1187,
\* sdk/rocketnet_server_transport.cpp:1513); no step reads it while STOPPED,
\* so the model keeps it until the next Start empties it.
Stop ==
    /\ state = "RUNNING"
    /\ state' = "STOPPING" /\ draining' = FALSE
    /\ UNCHANGED <<slot, kept, env, lastEnd, established, ghosts>>

\* disconnect_all_clients: every occupied slot is disconnected by DrainStep
\* before another client is accepted.
DisconnectAll ==
    /\ state = "RUNNING" /\ ~draining
    /\ \E i \in Slots : slot[i].st # "FREE"
    /\ draining' = TRUE
    /\ UNCHANGED <<state, slot, kept, env, lastEnd, established, ghosts>>

\* One slot of a stop or a disconnect_all_clients: the disconnect burst to a
\* connected client, then Reset(i).
DrainStep(i) ==
    /\ droppedIn' = {}
    /\ state = "STOPPING" \/ (state = "RUNNING" /\ draining)
    /\ slot[i].st # "FREE"
    /\ ~(Defect = "stopleavesclient" /\ state = "STOPPING" /\ i = SlotCount)
    /\ EndSlot([slot EXCEPT ![i].rec = Write(@, DisconnectFields, slot[i].sess)], i,
               IF state = "STOPPING" THEN "SERVER_STOPPED" ELSE "SERVER_DISCONNECTED", TRUE)
    /\ UNCHANGED <<state, draining, env, established, estTokens, doubleTok,
                   observed, crossDelivered, startFrom, queuedIn, priorTokens, readmitted>>

StopComplete ==
    /\ droppedIn' = {}
    /\ state = "STOPPING"
    /\ \A i \in Slots : slot[i].st = "FREE" \/ (Defect = "stopleavesclient" /\ i = SlotCount)
    /\ state' = "STOPPED"
    /\ UNCHANGED <<slot, kept, draining, env, lastEnd, established, ghosts>>

DrainDone ==
    /\ droppedIn' = {}
    /\ state = "RUNNING" /\ draining
    /\ \A i \in Slots : slot[i].st = "FREE"
    /\ draining' = FALSE
    /\ UNCHANGED <<state, slot, kept, env, lastEnd, established, ghosts>>

\* A client's request with token t opens a mapping into free slot i: a new
\* session, the slot's token, client id, user data, timeouts and session keys
\* written. A token in the used-token history is refused (every request in
\* this model comes from its own address, so a used token is a replay;
\* rocketnet_used_token_history_check, sdk/rocketnet_server_protocol.cpp:508).
ClientConnects(i, t) ==
    /\ state = "RUNNING" /\ ~draining /\ slot[i].st = "FREE"
    /\ nextSess <= MaxSessions /\ t \notin kept.used
    /\ slot' = [slot EXCEPT ![i].st = "HANDSHAKE", ![i].sess = nextSess,
                            ![i].rec = Write(@, MappingFields, nextSess)]
    /\ home' = [home EXCEPT ![nextSess] = i]
    /\ tokOf' = [tokOf EXCEPT ![nextSess] = t]
    /\ nextSess' = nextSess + 1
    /\ UNCHANGED <<state, kept, draining, link, starts, creates, lastEnd, established,
                   ghosts>>

\* The response opens: the token is bound in the used-token history
\* (rocketnet_used_token_history_record, sdk/rocketnet_server_protocol.cpp:743),
\* the slot is established, and its disconnect reason is cleared.
Establish(i) ==
    /\ droppedIn' = {}
    /\ state = "RUNNING" /\ slot[i].st = "HANDSHAKE"
    /\ LET s == slot[i].sess
           t == tokOf[s]
       IN /\ t \notin kept.used
          /\ slot' = [slot EXCEPT ![i].st = "CONNECTED", ![i].reason = "NONE"]
          /\ kept' = [kept EXCEPT !.used = @ \cup {t}, !.lifetime = @ + 1]
          /\ established' = established + 1
          /\ estTokens' = estTokens \cup {t}
          /\ doubleTok' = (doubleTok \/ t \in estTokens)
          /\ readmitted' = (readmitted \/ t \in priorTokens)
          /\ observed' = observed \cup {[slot |-> i, field |-> f] :
                                         f \in StaleIn(slot[i].rec, EstablishReads, s)}
    /\ UNCHANGED <<state, draining, env, lastEnd, crossDelivered, collateral, silentEnd, startFrom, queuedIn,
                   priorTokens>>

\* A denial during the handshake (the token taken meanwhile, no slot, a
\* refused response): no disconnect burst, a denied packet.
Deny(i) ==
    /\ droppedIn' = {}
    /\ slot[i].st = "HANDSHAKE"
    /\ EndSlot(slot, i, "DENIED", FALSE)
    /\ UNCHANGED <<state, draining, env, established, estTokens, doubleTok,
                   observed, crossDelivered, startFrom, queuedIn, priorTokens, readmitted>>

SlotTimeout(i) ==
    /\ droppedIn' = {}
    /\ slot[i].st \in {"HANDSHAKE", "CONNECTED"}
    /\ EndSlot(slot, i, "TIMED_OUT", FALSE)
    /\ UNCHANGED <<state, draining, env, established, estTokens, doubleTok,
                   observed, crossDelivered, startFrom, queuedIn, priorTokens, readmitted>>

\* The client's own disconnect packet arrives.
ClientLeaves(i) ==
    /\ slot[i].st = "CONNECTED"
    /\ EndSlot(slot, i, "CLIENT_DISCONNECTED", FALSE)
    /\ UNCHANGED <<state, draining, env, established, estTokens, doubleTok,
                   observed, crossDelivered, startFrom, queuedIn, priorTokens, readmitted>>

\* disconnect_client(i): the disconnect burst, then Reset(i).
DisconnectClient(i) ==
    /\ state = "RUNNING" /\ slot[i].st = "CONNECTED"
    /\ EndSlot([slot EXCEPT ![i].rec = Write(@, DisconnectFields, slot[i].sess)], i,
               "SERVER_DISCONNECTED", TRUE)
    /\ UNCHANGED <<state, draining, env, established, estTokens, doubleTok,
                   observed, crossDelivered, startFrom, queuedIn, priorTokens, readmitted>>

\* Message start/finish to slot i: only while it is connected. The defect
\* lets a send to a FREE or HANDSHAKE slot write the slot's send state.
ServerSend(i) ==
    /\ state = "RUNNING" /\ (slot[i].st = "CONNECTED" \/ Defect = "sendbeforeconnected")
    /\ LET s == slot[i].sess
       IN /\ observed' = observed \cup {[slot |-> i, field |-> f] :
                                         f \in StaleIn(slot[i].rec, SendReads, s)}
          /\ slot' = [slot EXCEPT ![i].rec = Write(@, SendFields, s)]
    /\ queuedIn' = queuedIn \cup {slot[i].st}
    /\ UNCHANGED <<state, kept, draining, env, lastEnd, established, estTokens, doubleTok,
                   crossDelivered, collateral, silentEnd, startFrom, priorTokens, readmitted>>

\* Message start to slot i when it is not CONNECTED, or on a stopped server
\* (rocketnet_server_start_message returns NULL, counted in
\* NOT_CONNECTED_DROPS): dropped, never queued. No slot field changes; the
\* slot's state is recorded in the ghost. stop is one call, so the
\* application never sends while STOPPING.
ServerDropSend(i) ==
    /\ \/ state = "STOPPED"
       \/ state = "RUNNING" /\ slot[i].st # "CONNECTED"
    /\ droppedIn' = {slot[i].st}
    /\ UNCHANGED <<state, slot, kept, draining, env, lastEnd, established, estTokens, doubleTok,
                   observed, crossDelivered, collateral, silentEnd, startFrom, queuedIn,
                   priorTokens, readmitted>>

\* The client of any session ever started may still send, also after its
\* connection ended.
ClientSend(s) ==
    /\ s < nextSess /\ s \notin link
    /\ link' = link \cup {s}
    /\ UNCHANGED <<state, slot, kept, draining, nextSess, home, tokOf, starts, creates,
                   lastEnd, established, ghosts>>

\* A client packet arrives at the slot its session connected into (the same
\* address reconnecting is the worst case). It opens only under the session
\* keys it was sealed with, and only into a connected slot; otherwise it is
\* discarded.
Deliver(s) ==
    /\ s \in link
    /\ link' = link \ {s}
    /\ LET i == home[s]
       IN IF state \in {"RUNNING", "STOPPING"} /\ i # 0
             /\ slot[i].st = "CONNECTED" /\ slot[i].rec["session_keys"] = s
          THEN /\ crossDelivered' = IF s # slot[i].sess
                                    THEN crossDelivered \cup {[from |-> s, to |-> slot[i].sess]}
                                    ELSE crossDelivered
               /\ observed' = observed \cup {[slot |-> i, field |-> f] :
                                              f \in StaleIn(slot[i].rec, RecvReads, slot[i].sess)}
               /\ slot' = [slot EXCEPT ![i].rec = Write(@, RecvFields, slot[i].sess)]
          ELSE UNCHANGED <<slot, observed, crossDelivered>>
    /\ UNCHANGED <<state, kept, draining, nextSess, home, tokOf, starts, creates, lastEnd,
                   established, estTokens, doubleTok, collateral, silentEnd,
                   startFrom, queuedIn, priorTokens, readmitted>>

Lose(s) ==
    /\ s \in link
    /\ link' = link \ {s}
    /\ UNCHANGED <<state, slot, kept, draining, nextSess, home, tokOf, starts, creates,
                   lastEnd, established, ghosts>>

\* Every step but a drop clears droppedIn (the fair actions also say so
\* themselves, so that their fairness conditions name a whole step).
Next ==
    \/ /\ \/ Create \/ Destroy \/ Start \/ Stop \/ DisconnectAll \/ StopComplete \/ DrainDone
          \/ \E i \in Slots : \/ DrainStep(i) \/ Establish(i) \/ Deny(i) \/ SlotTimeout(i)
                              \/ ClientLeaves(i) \/ DisconnectClient(i) \/ ServerSend(i)
                              \/ \E t \in Tokens : ClientConnects(i, t)
          \/ \E s \in Sessions : ClientSend(s) \/ Deliver(s) \/ Lose(s)
       /\ droppedIn' = {}
    \/ \E i \in Slots : ServerDropSend(i)

Spec == Init /\ [][Next]_vars
\* Fair delivery of the handshake's response, the timers, and the update
\* that drains a stop or a disconnect_all_clients.
Fairness ==
    /\ \A i \in Slots : WF_vars(DrainStep(i)) /\ WF_vars(Establish(i) \/ Deny(i) \/ SlotTimeout(i))
    /\ WF_vars(StopComplete) /\ WF_vars(DrainDone)
LiveSpec == Spec /\ Fairness

TypeOK ==
    /\ state \in ServerStates /\ draining \in BOOLEAN
    /\ slot \in [Slots -> [st : SlotStates, sess : Tags, rec : [Fields -> Tags], reason : Reasons]]
    /\ kept \in [used : SUBSET Tokens, lifetime : 0..MaxSessions, keys : 0..1, config : 0..1]
    /\ link \subseteq Sessions /\ nextSess \in 1..MaxSessions + 1
    /\ home \in [Sessions -> 0..SlotCount] /\ tokOf \in [Sessions -> 0..MaxTokens]
    /\ starts \in 0..MaxStarts /\ creates \in 0..MaxCreates
    /\ lastEnd \in [Slots -> Reasons] /\ established \in 0..MaxSessions
    /\ estTokens \subseteq Tokens /\ doubleTok \in BOOLEAN
    /\ observed \subseteq [slot : Slots, field : Fields]
    /\ crossDelivered \subseteq [from : Sessions, to : Tags]
    /\ collateral \subseteq Slots /\ silentEnd \in BOOLEAN
    /\ startFrom \subseteq ServerStates /\ queuedIn \subseteq SlotStates
    /\ priorTokens \subseteq Tokens /\ readmitted \in BOOLEAN
    /\ droppedIn \subseteq SlotStates

\* An occupied slot holds its session; a free one holds none.
SlotOwnsItsSession == \A i \in Slots :
    (slot[i].st = "FREE") <=> (slot[i].sess = 0)
    /\ slot[i].st # "FREE" => home[slot[i].sess] = i

StartOnlyFromStopped == startFrom \subseteq {"STOPPED"}

NoSendUnlessConnected ==
    /\ queuedIn \subseteq {"CONNECTED"}
    /\ \A i \in Slots : slot[i].st # "CONNECTED" =>
           \A f \in SendFields \ RecvFields : slot[i].rec[f] = 0

\* A drop happens only to a slot that is not CONNECTED and leaves nothing: a
\* slot that is not CONNECTED holds no send state, so nothing dropped is sent
\* to the client that connects into it.
DropLeavesNothing ==
    /\ droppedIn \subseteq {"FREE", "HANDSHAKE"}
    /\ \A i \in Slots : slot[i].st # "CONNECTED" =>
           \A f \in SendFields \ RecvFields : slot[i].rec[f] = 0

\* A free slot's per-connection fields are all Init; the used-token history
\* holds every token established in this run (kept by every reset, fresh at
\* each start), the lifetime counter every connection of the object, and the
\* keys and config stay across resets, stops and starts.
ResetIsInit ==
    /\ \A i \in Slots : slot[i].st = "FREE" => slot[i].rec = InitRec
    /\ kept.used = estTokens /\ kept.lifetime = established
    /\ state # "NONE" => kept.keys = 1 /\ kept.config = 1

\* A slot's record holds nothing but Init and its current session's writes.
RecordIsCurrent == \A i \in Slots : \A f \in Fields : slot[i].rec[f] \in {0, slot[i].sess}

\* A later session in a slot observes nothing of an earlier one: no packet of
\* session s opened by another session, and no step reads a field another
\* session wrote (a stale ack, a stale fragment, a replay window that would
\* refuse the new session's first sequences).
SlotReuseObservesNothing == observed = {} /\ crossDelivered = {}

NoClientWhileStopped == state \in {"STOPPED", "NONE"} => \A i \in Slots : slot[i].st = "FREE"

\* After a stop every slot is free and Init, and every connected client the
\* server ended (stop, disconnect_client, disconnect_all_clients) was sent
\* its disconnect.
StopDisconnectsAll ==
    /\ state = "STOPPED" => \A i \in Slots : slot[i].st = "FREE" /\ slot[i].rec = InitRec
    /\ ~silentEnd

ReasonKeptUntilNextConnection == \A i \in Slots :
    slot[i].reason = IF slot[i].st = "CONNECTED" THEN "NONE" ELSE lastEnd[i]

ResetTouchesOnlyItsSlot == collateral = {}

\* One token admits one connection per run: the used-token history is kept
\* across slot resets within the run and is fresh at each start.
TokenAdmitsOnce == ~doubleTok

\* A reachability witness, not a rule: a token established before a stop is
\* never established after the next start. MCServerSessionReadmitAfterStart
\* expects TLC to violate it, proving the fresh history at start is reached.
NoReadmitAfterStart == ~readmitted

\* A connect reaches CONNECTED or is ended; a stop reaches STOPPED.
HandshakeResolves == \A i \in Slots : (slot[i].st = "HANDSHAKE") ~> (slot[i].st # "HANDSHAKE")
StopReachesStopped == (state = "STOPPING") ~> (state \in {"STOPPED", "NONE"})
=============================================================================
