--------------------------- MODULE ClientSession ---------------------------
\* The client object's life across many connections: create, connect(token),
\* the handshake abstracted to its three stages, connected, the paths out of a
\* connection (the client's disconnect, a timeout, the server's disconnect, a
\* denial, a bad token), disconnect back to DISCONNECTED, connect again, and
\* destroy. The handshake packet by packet is tla/ClientConnection.tla; this
\* module models what that one does not: the per-connection state written by
\* one connection and the single Reset that clears it before the next.
\*
\* Every per-connection field holds 0 (Init) or the tag of the session that
\* wrote it. Session tags never repeat, also across destroy and create, so a
\* field holding a tag other than the current session's is state of an earlier
\* connection. The fields deliberately kept across connections are `kept` (the
\* lifetime counters and the config) and the disconnect reason.
\*
\* Reversed witnesses, one constant: Defect = "unreset" makes Reset skip the
\* field UnresetField; Defect = "connectanywhere" lets connect succeed from
\* every state.
EXTENDS Naturals, FiniteSets

CONSTANTS MaxSessions, MaxCreates, Defect, UnresetField

ASSUME Defect \in {"none", "unreset", "connectanywhere"}
ASSUME MaxSessions \in Nat \ {0} /\ MaxCreates \in Nat \ {0}

\* One field per row of the per-connection reset inventory.
TokenFields == {"token_state", "client_id", "user_data", "timeouts"}
KeyFields == {"session_keys"}
SendFields == {"packet_sequence", "message_sequence", "sent_packets",
               "reliable_send_queue", "ordering_indices", "send_budget",
               "connection_counters", "ack_receipt_ring"}
RecvFields == {"received_packets", "reliable_receive_queue", "ack_bits",
               "replay_window", "fragment_reassembly", "rtt_estimator",
               "loss_estimator", "connection_counters"}
DisconnectFields == {"pending_disconnect"}
Fields == TokenFields \cup KeyFields \cup SendFields \cup RecvFields \cup DisconnectFields
ASSUME UnresetField \in Fields

\* What each step of a connection reads. Every field is read by some step, so
\* a field left over from an earlier session is observed by the next one.
HandshakeReads == TokenFields \cup KeyFields
SendReads == SendFields \cup {"ack_bits", "session_keys", "pending_disconnect", "timeouts"}
RecvReads == RecvFields \cup {"sent_packets", "ack_receipt_ring", "session_keys", "timeouts"}
ASSUME HandshakeReads \cup SendReads \cup RecvReads = Fields

HandshakeStates == {"REQUESTING_COOKIE", "SENDING_CONNECTION_REQUEST",
                    "SENDING_CONNECTION_RESPONSE"}
Errors == {"CONNECTION_DENIED", "COOKIE_REQUEST_TIMED_OUT",
           "CONNECTION_REQUEST_TIMED_OUT", "CONNECTION_RESPONSE_TIMED_OUT",
           "CONNECTION_TIMED_OUT", "CONNECT_TOKEN_EXPIRED", "INVALID_CONNECT_TOKEN"}
Live == HandshakeStates \cup {"CONNECTED"}
States == {"NONE", "DISCONNECTED"} \cup Live \cup Errors
Reasons == {"NONE", "CLIENT_DISCONNECTED", "SERVER_DISCONNECTED"} \cup Errors
Tokens == {"valid", "invalid", "expired"}
Sessions == 1..MaxSessions
Tags == 0..MaxSessions

NextStage(s) == IF s = "REQUESTING_COOKIE" THEN "SENDING_CONNECTION_REQUEST"
                ELSE IF s = "SENDING_CONNECTION_REQUEST" THEN "SENDING_CONNECTION_RESPONSE"
                ELSE "CONNECTED"
TimedOut(s) == IF s = "REQUESTING_COOKIE" THEN "COOKIE_REQUEST_TIMED_OUT"
               ELSE IF s = "SENDING_CONNECTION_REQUEST" THEN "CONNECTION_REQUEST_TIMED_OUT"
               ELSE IF s = "SENDING_CONNECTION_RESPONSE" THEN "CONNECTION_RESPONSE_TIMED_OUT"
               ELSE "CONNECTION_TIMED_OUT"

InitConn == [f \in Fields |-> 0]
Write(c, fs, s) == [f \in Fields |-> IF f \in fs THEN s ELSE c[f]]
\* The one reset function: every per-connection field back to Init.
Reset(c) == [f \in Fields |-> IF Defect = "unreset" /\ f = UnresetField THEN c[f] ELSE 0]
StaleIn(c, fs, s) == {f \in fs : c[f] \notin {0, s}}

VARIABLES st, cur, conn, reason, kept, link, nextSess, creates,
          lastEnd, connected, observed, crossDelivered, connectFrom, queuedIn
\* st, cur, conn, reason, kept: the client object (cur is the session tag of
\*   the connection in progress, 0 outside one)
\* link: the session tags of server packets in flight to this client (lossy)
\* nextSess, creates: the environment's counters
\* lastEnd, connected, observed, crossDelivered, connectFrom, queuedIn: ghosts
vars == <<st, cur, conn, reason, kept, link, nextSess, creates,
          lastEnd, connected, observed, crossDelivered, connectFrom, queuedIn>>
ghosts == <<observed, crossDelivered, connectFrom, queuedIn>>

Init ==
    /\ st = "NONE" /\ cur = 0 /\ conn = InitConn /\ reason = "NONE"
    /\ kept = [lifetime |-> 0, config |-> 0]
    /\ link = {} /\ nextSess = 1 /\ creates = 0
    /\ lastEnd = "NONE" /\ connected = 0
    /\ observed = {} /\ crossDelivered = {} /\ connectFrom = {} /\ queuedIn = {}

\* rocketnet_client_create: no token, DISCONNECTED, a fresh object.
Create ==
    /\ st = "NONE" /\ creates < MaxCreates
    /\ st' = "DISCONNECTED" /\ cur' = 0 /\ conn' = InitConn /\ reason' = "NONE"
    /\ kept' = [lifetime |-> 0, config |-> 1]
    /\ creates' = creates + 1 /\ lastEnd' = "NONE" /\ connected' = 0
    /\ UNCHANGED <<link, nextSess, ghosts>>

\* Destroy from any state. Packets in flight outlive the object.
Destroy ==
    /\ st # "NONE"
    /\ st' = "NONE" /\ cur' = 0 /\ conn' = InitConn /\ reason' = "NONE"
    /\ kept' = [lifetime |-> 0, config |-> 0]
    /\ lastEnd' = "NONE" /\ connected' = 0
    /\ UNCHANGED <<link, nextSess, creates, ghosts>>

\* Every path out of a connection: Reset, the reason the path names, and
\* the reason recorded for ReasonKeptUntilNextConnection.
End(c, to, why) ==
    /\ st' = to /\ cur' = 0 /\ conn' = Reset(c)
    /\ reason' = why /\ lastEnd' = why
    /\ UNCHANGED <<kept, link, nextSess, creates, connected, ghosts>>

\* connect(token): refused with a status code unless DISCONNECTED (a refused
\* call changes nothing, a stutter). A valid token starts a new session; an
\* invalid or expired one goes straight to its error state.
Connect(tok) ==
    /\ st # "NONE"
    /\ st = "DISCONNECTED" \/ Defect = "connectanywhere"
    /\ IF tok = "valid"
       THEN /\ nextSess <= MaxSessions
            /\ st' = "REQUESTING_COOKIE" /\ cur' = nextSess /\ nextSess' = nextSess + 1
            /\ conn' = Write(conn, TokenFields, nextSess)
            /\ connectFrom' = connectFrom \cup {st}
            /\ UNCHANGED <<reason, kept, link, creates, lastEnd, connected,
                           observed, crossDelivered, queuedIn>>
       ELSE /\ connectFrom' = connectFrom \cup {st}
            /\ LET to == IF tok = "invalid" THEN "INVALID_CONNECT_TOKEN"
                         ELSE "CONNECT_TOKEN_EXPIRED"
               IN /\ st' = to /\ cur' = 0 /\ conn' = Reset(conn)
                  /\ reason' = to /\ lastEnd' = to
            /\ UNCHANGED <<kept, link, nextSess, creates, connected,
                           observed, crossDelivered, queuedIn>>

\* The handshake abstracted to its stages: the server's reply to the stage
\* arrives. The challenge derives the session keys; the establishing
\* keep-alive moves to CONNECTED, which clears the disconnect reason.
HandshakeStep ==
    /\ st \in HandshakeStates
    /\ observed' = observed \cup StaleIn(conn, HandshakeReads, cur)
    /\ st' = NextStage(st)
    /\ IF NextStage(st) = "CONNECTED"
       THEN /\ reason' = "NONE"
            /\ kept' = [kept EXCEPT !.lifetime = @ + 1]
            /\ connected' = connected + 1
            /\ UNCHANGED conn
       ELSE /\ conn' = IF NextStage(st) = "SENDING_CONNECTION_RESPONSE"
                       THEN Write(conn, KeyFields, cur) ELSE conn
            /\ UNCHANGED <<reason, kept, connected>>
    /\ UNCHANGED <<cur, link, nextSess, creates, lastEnd,
                   crossDelivered, connectFrom, queuedIn>>

Deny == st \in HandshakeStates /\ End(conn, "CONNECTION_DENIED", "CONNECTION_DENIED")

\* The stage deadlines and the connected timeout.
Timer == st \in Live /\ End(conn, TimedOut(st), TimedOut(st))

ServerDisconnect == st = "CONNECTED" /\ End(conn, "DISCONNECTED", "SERVER_DISCONNECTED")

\* The client's disconnect from the handshake or connected: the disconnect
\* burst is written, then the one Reset.
ClientDisconnect ==
    /\ st \in Live
    /\ End(Write(conn, DisconnectFields, cur), "DISCONNECTED", "CLIENT_DISCONNECTED")

\* Disconnect from an error state: the connection already ended, so the
\* reason it set is kept; the Reset runs again and changes nothing.
DisconnectFromError ==
    /\ st \in Errors
    /\ st' = "DISCONNECTED" /\ cur' = 0 /\ conn' = Reset(conn)
    /\ UNCHANGED <<reason, kept, link, nextSess, creates, lastEnd, connected, ghosts>>

\* Message start/finish: only while CONNECTED (refused otherwise, a stutter).
Send ==
    /\ st = "CONNECTED"
    /\ observed' = observed \cup StaleIn(conn, SendReads, cur)
    /\ conn' = Write(conn, SendFields, cur)
    /\ queuedIn' = queuedIn \cup {st}
    /\ UNCHANGED <<st, cur, reason, kept, link, nextSess, creates, lastEnd,
                   connected, crossDelivered, connectFrom>>

\* The server side of any session ever started may still have packets in
\* flight, including sessions that ended and sessions of a destroyed object.
ServerSend(s) ==
    /\ s < nextSess /\ s \notin link
    /\ link' = link \cup {s}
    /\ UNCHANGED <<st, cur, conn, reason, kept, nextSess, creates, lastEnd,
                   connected, ghosts>>

\* A packet arrives. It opens only under the session keys it was sealed
\* with; one that does not open, or arrives outside a session, is discarded.
Deliver(s) ==
    /\ s \in link
    /\ link' = link \ {s}
    /\ IF st \in {"SENDING_CONNECTION_RESPONSE", "CONNECTED"} /\ conn["session_keys"] = s
       THEN /\ crossDelivered' = IF s # cur
                                 THEN crossDelivered \cup {[from |-> s, to |-> cur]}
                                 ELSE crossDelivered
            /\ observed' = observed \cup StaleIn(conn, RecvReads, cur)
            /\ conn' = Write(conn, RecvFields, cur)
       ELSE UNCHANGED <<conn, observed, crossDelivered>>
    /\ UNCHANGED <<st, cur, reason, kept, nextSess, creates, lastEnd,
                   connected, connectFrom, queuedIn>>

Lose(s) ==
    /\ s \in link
    /\ link' = link \ {s}
    /\ UNCHANGED <<st, cur, conn, reason, kept, nextSess, creates, lastEnd,
                   connected, ghosts>>

Next ==
    \/ Create \/ Destroy
    \/ \E tok \in Tokens : Connect(tok)
    \/ HandshakeStep \/ Deny \/ Timer \/ ServerDisconnect
    \/ ClientDisconnect \/ DisconnectFromError \/ Send
    \/ \E s \in Sessions : ServerSend(s) \/ Deliver(s) \/ Lose(s)

Spec == Init /\ [][Next]_vars
\* Fair delivery of the server's handshake replies, and the timers.
Fairness == WF_vars(HandshakeStep) /\ WF_vars(Timer)
LiveSpec == Spec /\ Fairness

TypeOK ==
    /\ st \in States /\ cur \in Tags /\ conn \in [Fields -> Tags]
    /\ reason \in Reasons /\ lastEnd \in Reasons
    /\ kept \in [lifetime : 0..MaxSessions, config : 0..1]
    /\ link \subseteq Sessions /\ nextSess \in 1..MaxSessions + 1
    /\ creates \in 0..MaxCreates /\ connected \in 0..MaxSessions
    /\ observed \subseteq Fields
    /\ crossDelivered \subseteq [from : Sessions, to : Tags]
    /\ connectFrom \subseteq States /\ queuedIn \subseteq States

\* A session is in progress exactly in the live states.
CurrentOnlyWhileLive == (st \in Live) <=> (cur # 0)

ConnectOnlyFromDisconnected == connectFrom \subseteq {"DISCONNECTED"}

NoSendUnlessConnected ==
    /\ queuedIn \subseteq {"CONNECTED"}
    /\ st # "CONNECTED" => \A f \in SendFields \ RecvFields : conn[f] = 0

\* Outside a connection every per-connection field is Init; the kept fields
\* are unchanged by every reset: the lifetime counter counts every
\* connection the object made, and the config stays.
ResetIsInit ==
    /\ st \notin Live => conn = InitConn
    /\ kept.lifetime = connected
    /\ st # "NONE" => kept.config = 1

\* The record holds nothing but Init and the current session's writes.
RecordIsCurrent == \A f \in Fields : conn[f] \in {0, cur}

\* A later session observes nothing of an earlier one: no packet of session
\* s opened by session s' # s, and no step of a session reads a field another
\* session wrote (a stale ack bit or sent entry, a stale fragment slot, a
\* replay window that would refuse the new session's first sequences).
ReconnectObservesNothing == observed = {} /\ crossDelivered = {}

ReasonKeptUntilNextConnection ==
    reason = IF st = "CONNECTED" THEN "NONE" ELSE lastEnd

\* A connect reaches CONNECTED or an error state, unless the application
\* disconnects or destroys the client first.
ConnectReachesConnectedOrError ==
    (st \in HandshakeStates) ~> (st \in {"CONNECTED", "DISCONNECTED", "NONE"} \cup Errors)
=============================================================================
