--------------------------- MODULE ClientSession ---------------------------
\* The client object's life across many connections: create, connect(token),
\* the handshake abstracted to its three stages, connected, the paths out of a
\* connection (the client's disconnect, a timeout, the server's disconnect, a
\* denial, a bad token), disconnect back to DISCONNECTED, connect again, and
\* destroy, and the message layer's end of a CONNECTED connection. The
\* handshake packet by packet is tla/ClientConnection.tla; this
\* module models what that one does not: the per-connection state written by
\* one connection and the single Reset that clears it before the next.
\*
\* Every per-connection field holds 0 (Init) or the tag of the session that
\* wrote it. Session tags never repeat, also across destroy and create, so a
\* field holding a tag other than the current session's is state of an earlier
\* connection. The one field deliberately kept across connections is `kept`
\* (the config). The handle keeps no disconnect reason of its own: the public
\* state names how the connection ended (sdk/rocketnet_api.cpp:1590-1599), and
\* its one lifetime count (not-connected drops, sdk/rocketnet_api.cpp:567) is not
\* modelled.
\*
\* A message send outside CONNECTED is dropped (DropSend): it changes no
\* per-connection field and nothing of it is held for a later send
\* (standard/MESSAGES.md m1.5). The ghost droppedIn holds the state of the
\* drop the last step made, {} after any other step, so it adds at most one
\* copy of each state.
\*
\* The public endpoint keeps its old transport generation after a terminal
\* state. A retry requests stop without waiting; worker completion is separate.
\* The next connect finalizes a stopped generation before reusing the block.
\* Defect instances reverse reset, live refusal, send, completion, and key
\* isolation guards to make each invariant produce a concrete witness.
\*
\* st is the public state, rocketnet_client_state: the server's disconnect
\* ends a connection in SERVER_DISCONNECTED, and each message-layer end
\* (MessageEnd) in its own error state, error states like the others: the
\* thirteen negative values of enum rocketnet_client_state_t.
\* StateReported: the state change callback runs at the end of every public
\* call that changes st and reports the change once, from the value it last
\* reported (reported) to st; lastReport is the step's report, <<>> when the
\* step reports none. Create and Destroy report nothing: the callback belongs
\* to the object. The transportstate defect reports the transport's own state,
\* which reads DISCONNECTED where the public state reads SERVER_DISCONNECTED.
\* The silentexhaust defect is rocketnet_client_start_message without its
\* report (sdk/rocketnet_api.cpp:1715-1716 deleted): a start whose memory
\* exhaustion ends the connection reports nothing.
EXTENDS Naturals, FiniteSets

CONSTANTS MaxSessions, MaxCreates, Defect, UnresetField, StuckWorker

ASSUME Defect \in {"none", "unreset", "connectanywhere", "queuebeforeconnected", "destroysilent", "reusepending", "skipstop", "reusekeys", "completionrefused", "transportstate", "silentexhaust"}
ASSUME MaxSessions \in Nat \ {0} /\ MaxCreates \in Nat \ {0}
ASSUME StuckWorker \in BOOLEAN

\* One field per row of the per-connection reset inventory.
TokenFields == {"token_state", "client_id", "user_data", "timeouts"}
KeyFields == {"session_keys"}
SendFields == {"packet_sequence", "message_sequence", "sent_packets",
               "reliable_send_queue", "ordering_indices", "send_budget",
               "connection_counters", "ack_receipt_ring"}
RecvFields == {"received_packets", "reliable_receive_queue", "ack_bits",
               "replay_window", "rtt_estimator",
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
\* The message layer's own ends of a CONNECTED connection, each read back as
\* its own public state (sdk/rocketnet_api.cpp:1590-1599).
MessageErrors == {"RELIABLE_TIMED_OUT", "PROTOCOL_VIOLATION", "CONFIG_MISMATCH",
                  "MESSAGE_MEMORY_EXHAUSTED", "MESSAGE_NOT_RECEIVED"}
Errors == {"CONNECTION_DENIED", "COOKIE_REQUEST_TIMED_OUT",
           "CONNECTION_REQUEST_TIMED_OUT", "CONNECTION_RESPONSE_TIMED_OUT",
           "CONNECTION_TIMED_OUT", "CONNECT_TOKEN_EXPIRED", "INVALID_CONNECT_TOKEN",
           "SERVER_DISCONNECTED"} \cup MessageErrors
RestartStates == {"DISCONNECTED"} \cup Errors
Live == HandshakeStates \cup {"CONNECTED"}
States == {"NONE", "DISCONNECTED"} \cup Live \cup Errors
\* What connect is handed: a string that is not one base64 token, a token the
\* transport accepts, one it refuses as invalid or as expired, and a token
\* whose connection cannot be created (socket, thread, memory).
Tokens == {"malformed", "valid", "invalid", "expired", "createfails"}
\* The calls a message-layer end reaches the public state through: a start
\* the message memory cannot hold, and the update.
MessageEnds == {<<"send", "MESSAGE_MEMORY_EXHAUSTED">>} \cup ({"update"} \X MessageErrors)
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

VARIABLES st, cur, conn, kept, link, nextSess, creates,
          observed, crossDelivered, connectFrom, queuedIn, droppedIn,
          silentEnd, owner, ownerGen, pending, refusedPending, badReuse,
          reported, lastReport
\* st, cur, conn, kept: the client object (cur is the session tag of
\*   the connection in progress, 0 outside one)
\* link: the session tags of server packets in flight to this client (lossy)
\* nextSess, creates: the environment's counters
\* observed, crossDelivered, connectFrom, queuedIn, droppedIn, silentEnd: ghosts
\* reported, lastReport: the state change callback (StateReported)
vars == <<st, cur, conn, kept, link, nextSess, creates,
          observed, crossDelivered, connectFrom, queuedIn, droppedIn,
          silentEnd, owner, ownerGen, pending, refusedPending, badReuse,
          reported, lastReport>>
ghosts == <<observed, crossDelivered, connectFrom, queuedIn, silentEnd>>

Init ==
    /\ st = "NONE" /\ cur = 0 /\ conn = InitConn
    /\ kept = [config |-> 0]
    /\ link = {} /\ nextSess = 1 /\ creates = 0
    /\ observed = {} /\ crossDelivered = {} /\ connectFrom = {} /\ queuedIn = {}
    /\ droppedIn = {} /\ silentEnd = FALSE
    /\ owner = "none" /\ ownerGen = 0 /\ pending = FALSE
    /\ refusedPending = FALSE /\ badReuse = FALSE
    /\ reported = "NONE" /\ lastReport = <<>>

\* rocketnet_client_create: no token, DISCONNECTED, a fresh object.
Create ==
    /\ st = "NONE" /\ creates < MaxCreates
    /\ st' = "DISCONNECTED" /\ cur' = 0 /\ conn' = InitConn
    /\ kept' = [config |-> 1]
    /\ creates' = creates + 1
    /\ owner' = "none" /\ ownerGen' = 0 /\ pending' = FALSE
    /\ UNCHANGED <<link, nextSess, ghosts, refusedPending, badReuse>>

\* Destroy only after worker ownership ends. Packets in flight can outlive it.
Destroy ==
    /\ st # "NONE"
    /\ owner \in {"none", "stopped"} \/ Defect = "destroysilent"
    /\ st' = "NONE" /\ cur' = 0 /\ conn' = InitConn
    /\ kept' = [config |-> 0]
    /\ silentEnd' = (silentEnd \/ (st \in {"SENDING_CONNECTION_RESPONSE", "CONNECTED"} /\ Defect = "destroysilent"))
    /\ badReuse' = (badReuse \/ (owner \in {"running", "stopping"}))
    /\ owner' = "none" /\ ownerGen' = 0 /\ pending' = FALSE
    /\ refusedPending' = FALSE
    /\ UNCHANGED <<link, nextSess, creates, observed, crossDelivered, connectFrom, queuedIn>>

\* Every path out of a connection: Reset, and the state the path names.
End(c, to, stop) ==
    /\ st' = to /\ cur' = 0 /\ conn' = Reset(c)
    /\ owner' = IF stop THEN "stopping" ELSE owner
    /\ pending' = stop
    /\ UNCHANGED <<kept, link, nextSess, creates, ghosts,
                   ownerGen, refusedPending, badReuse>>

\* A retry from a terminal state with an old worker requests nonwaiting stop.
\* The refused call cannot reset or reuse the connection block. A stopped
\* generation is joined/finalized synchronously inside the next Connect.
RetryPending ==
    /\ st \in RestartStates /\ owner = "running"
    /\ owner' = IF Defect = "skipstop" THEN "running" ELSE "stopping"
    /\ pending' = TRUE /\ refusedPending' = TRUE
    /\ UNCHANGED <<st, cur, conn, kept, link, nextSess, creates,
                   ghosts, ownerGen, badReuse>>

WorkerComplete ==
    /\ owner = "stopping" /\ ~StuckWorker
    /\ owner' = "stopped"
    /\ UNCHANGED <<st, cur, conn, kept, link, nextSess, creates,
                   ghosts, ownerGen, pending, refusedPending, badReuse>>

\* connect(token), rn_client_connect (sdk/rocketnet_api.cpp:1322-1391), with
\* its five outcomes. A live state is refused first (1326-1332). A string that
\* is not one base64 token is refused next, before the terminal attempt or
\* its worker is touched (1338-1343): nothing changes, whatever the old worker
\* is doing. Otherwise a terminal state may retry only when it owns no worker
\* or the old worker has completed (RetryPending otherwise, 1359-1368), and
\* finalization joins the stopped generation; then the connection is created
\* (1373-1390): a valid token starts the handshake, a token the transport
\* refuses ends in INVALID_CONNECT_TOKEN or CONNECT_TOKEN_EXPIRED, and any
\* other create failure leaves DISCONNECTED.
Connect(tok) ==
    /\ st # "NONE"
    /\ st \in RestartStates \/ Defect = "connectanywhere"
    /\ IF tok = "malformed"
       THEN UNCHANGED <<st, cur, conn, kept, link, nextSess, creates, ghosts,
                        owner, ownerGen, pending, refusedPending, badReuse>>
       ELSE
       /\ (IF Defect = "completionrefused" THEN owner = "none"
           ELSE owner \in {"none", "stopped"}) \/ Defect \in {"connectanywhere", "reusepending"}
       /\ badReuse' = (badReuse \/ (owner \in {"running", "stopping"}))
       /\ pending' = FALSE
       /\ refusedPending' = FALSE
       /\ connectFrom' = connectFrom \cup {st}
       /\ IF tok = "valid"
          THEN /\ nextSess <= MaxSessions
               /\ st' = "REQUESTING_COOKIE" /\ cur' = nextSess /\ nextSess' = nextSess + 1
               /\ conn' = Write(conn, TokenFields, nextSess)
               /\ owner' = "running" /\ ownerGen' = nextSess
               /\ UNCHANGED <<kept, link, creates,
                              observed, crossDelivered, queuedIn, silentEnd>>
          ELSE /\ st' = CASE tok = "invalid" -> "INVALID_CONNECT_TOKEN"
                          [] tok = "expired" -> "CONNECT_TOKEN_EXPIRED"
                          [] OTHER           -> "DISCONNECTED"
               /\ cur' = 0 /\ conn' = Reset(conn)
               /\ owner' = "none" /\ ownerGen' = 0
               /\ UNCHANGED <<kept, link, nextSess, creates,
                              observed, crossDelivered, queuedIn, silentEnd>>

\* The handshake abstracted to its stages: the server's reply to the stage
\* arrives. The challenge derives the session keys; the establishing
\* keep-alive moves to CONNECTED.
HandshakeStep ==
    /\ st \in HandshakeStates
    /\ droppedIn' = {}
    /\ observed' = observed \cup StaleIn(conn, HandshakeReads, cur)
    /\ st' = NextStage(st)
    /\ conn' = IF NextStage(st) = "SENDING_CONNECTION_RESPONSE"
                THEN Write(conn, KeyFields, IF Defect = "reusekeys" /\ cur > 1 THEN cur - 1 ELSE cur) ELSE conn
    /\ UNCHANGED <<cur, kept, link, nextSess, creates,
                   crossDelivered, connectFrom, queuedIn, silentEnd,
                   owner, ownerGen, pending, refusedPending, badReuse>>

Deny == st \in HandshakeStates /\ End(conn, "CONNECTION_DENIED", FALSE)

\* The token expires while the handshake is still running.
TokenExpires == st \in HandshakeStates /\ End(conn, "CONNECT_TOKEN_EXPIRED", FALSE)

\* The stage deadlines and the connected timeout.
Timer == st \in Live /\ End(conn, TimedOut(st), FALSE) /\ droppedIn' = {}

ServerDisconnect == st = "CONNECTED" /\ End(conn, "SERVER_DISCONNECTED", FALSE)

\* The client's disconnect from the handshake or connected: the disconnect
\* burst is written, then the one Reset.
ClientDisconnect ==
    /\ st \in Live
    /\ End(Write(conn, DisconnectFields, cur), "DISCONNECTED", TRUE)

\* Disconnect from an error state: the connection already ended; the Reset
\* runs again and changes nothing.
DisconnectFromError ==
    /\ st \in Errors
    /\ st' = "DISCONNECTED" /\ cur' = 0 /\ conn' = Reset(conn)
    /\ owner' = IF owner = "running" THEN "stopping" ELSE owner
    /\ pending' = (owner # "none")
    /\ UNCHANGED <<kept, link, nextSess, creates, ghosts,
                   ownerGen, refusedPending, badReuse>>

\* Message start/finish: only while CONNECTED. The defect lets a send in a
\* handshake state write the send state, held for after CONNECTED.
Send ==
    /\ st = "CONNECTED" \/ (Defect = "queuebeforeconnected" /\ st \in HandshakeStates)
    /\ observed' = observed \cup StaleIn(conn, SendReads, cur)
    /\ conn' = Write(conn, SendFields, cur)
    /\ queuedIn' = queuedIn \cup {st}
    /\ UNCHANGED <<st, cur, kept, link, nextSess, creates,
                   crossDelivered, connectFrom, silentEnd,
                   owner, ownerGen, pending, refusedPending, badReuse>>

\* Message start outside CONNECTED (rocketnet_client_start_message returns
\* NULL, counted in NOT_CONNECTED_DROPS): dropped, never queued. No
\* per-connection field changes; the state is recorded in the ghost.
DropSend ==
    /\ st \notin {"NONE", "CONNECTED"}
    /\ droppedIn' = {st}
    /\ UNCHANGED <<st, cur, conn, kept, link, nextSess, creates,
                   observed, crossDelivered, connectFrom, queuedIn, silentEnd,
                   owner, ownerGen, pending, refusedPending, badReuse>>

\* The server side of any session ever started may still have packets in
\* flight, including sessions that ended and sessions of a destroyed object.
ServerSend(s) ==
    /\ s < nextSess /\ s \notin link
    /\ link' = link \cup {s}
    /\ UNCHANGED <<st, cur, conn, kept, nextSess, creates,
                   ghosts, owner, ownerGen, pending, refusedPending, badReuse>>

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
    /\ UNCHANGED <<st, cur, kept, nextSess, creates,
                   connectFrom, queuedIn, silentEnd, owner,
                   ownerGen, pending, refusedPending, badReuse>>

Lose(s) ==
    /\ s \in link
    /\ link' = link \ {s}
    /\ UNCHANGED <<st, cur, conn, kept, nextSess, creates,
                   ghosts, owner, ownerGen, pending, refusedPending, badReuse>>

\* The state the callback reports: the public state, or with the
\* transportstate defect the transport's, which has no SERVER_DISCONNECTED.
ReportedView(s) == IF Defect = "transportstate" /\ s = "SERVER_DISCONNECTED" THEN "DISCONNECTED" ELSE s

\* StateReported: at the end of the step the callback reports a changed state
\* once, from the value it last reported; Create and Destroy report nothing.
StateReported ==
    IF st = "NONE" \/ st' = "NONE"
    THEN /\ reported' = IF st' = "NONE" THEN "NONE" ELSE st'
         /\ lastReport' = <<>>
    ELSE /\ reported' = ReportedView(st')
         /\ lastReport' = IF ReportedView(st') # reported THEN <<reported, ReportedView(st')>> ELSE <<>>

\* The message layer ends a CONNECTED connection (rn_message_object_kick in
\* sdk/rocketnet_message_server.cpp): the connection object stays, its
\* transport reads DISCONNECTED, and rocketnet_client_state reads the reason's
\* own error state (sdk/rocketnet_api.cpp:1590-1599). The worker keeps
\* running, as after ServerDisconnect. The call that reaches it reports it:
\* a start the message memory cannot hold (rocketnet_client_start_message,
\* 1711-1716) with MESSAGE_MEMORY_EXHAUSTED, or the update (rocketnet_client_update,
\* 1488-1509) with any of the five. With the silentexhaust defect the start
\* reports nothing.
MessageEnd(call, to) ==
    /\ st = "CONNECTED"
    /\ End(conn, to, FALSE)
    /\ droppedIn' = {}
    /\ IF call = "send" /\ Defect = "silentexhaust"
       THEN UNCHANGED reported /\ lastReport' = <<>>
       ELSE StateReported

\* Every step but a drop clears droppedIn (the two fair actions also say so
\* themselves, so that their fairness conditions name a whole step). Every
\* step but MessageEnd carries StateReported here; MessageEnd carries its own.
Next ==
    \/ /\ \/ /\ \/ Create \/ Destroy
                \/ \E tok \in Tokens : Connect(tok)
                \/ RetryPending \/ WorkerComplete
                \/ HandshakeStep \/ Deny \/ TokenExpires \/ Timer \/ ServerDisconnect
                \/ ClientDisconnect \/ DisconnectFromError \/ Send
                \/ \E s \in Sessions : ServerSend(s) \/ Deliver(s) \/ Lose(s)
             /\ droppedIn' = {}
          \/ DropSend
       /\ StateReported
    \/ \E e \in MessageEnds : MessageEnd(e[1], e[2])

Spec == Init /\ [][Next]_vars
\* Fair delivery of the server's handshake replies, and the timers; each fair
\* step carries its report, so that the condition names a whole step.
Fairness == WF_vars(HandshakeStep /\ StateReported) /\ WF_vars(Timer /\ StateReported)
LiveSpec == Spec /\ Fairness

TypeOK ==
    /\ st \in States /\ cur \in Tags /\ conn \in [Fields -> Tags]
    /\ kept \in [config : 0..1]
    /\ link \subseteq Sessions /\ nextSess \in 1..MaxSessions + 1
    /\ creates \in 0..MaxCreates
    /\ observed \subseteq Fields
    /\ crossDelivered \subseteq [from : Sessions, to : Tags]
    /\ connectFrom \subseteq States /\ queuedIn \subseteq States
    /\ droppedIn \subseteq States /\ silentEnd \in BOOLEAN
    /\ owner \in {"none", "running", "stopping", "stopped"}
    /\ ownerGen \in Tags /\ pending \in BOOLEAN
    /\ refusedPending \in BOOLEAN /\ badReuse \in BOOLEAN
    /\ reported \in States
    /\ lastReport \in {<<>>} \cup (States \X States)

\* A session is in progress exactly in the live states.
CurrentOnlyWhileLive == (st \in Live) <=> (cur # 0)

ConnectOnlyFromTerminal == connectFrom \subseteq RestartStates
NoReuseBeforeComplete == ~badReuse
PendingRefuses == pending => owner \in {"stopping", "stopped"}
PendingRefusalRetained == refusedPending => (pending /\ owner \in {"stopping", "stopped"})
OwnerGeneration == (owner = "none") <=> (ownerGen = 0)
StuckRetainsOwner == (StuckWorker /\ pending) => owner = "stopping"
TerminalRetryEnabled ==
    (st \in RestartStates /\ nextSess <= MaxSessions) =>
      (IF owner = "running" THEN ENABLED RetryPending
       ELSE IF owner = "stopping" THEN pending
       ELSE ENABLED Connect("valid"))

NoSendUnlessConnected ==
    /\ queuedIn \subseteq {"CONNECTED"}
    /\ st # "CONNECTED" => \A f \in SendFields \ RecvFields : conn[f] = 0

\* A drop happens only outside CONNECTED and leaves nothing: outside CONNECTED
\* no send state is written, so nothing dropped is sent once CONNECTED.
DropLeavesNothing ==
    /\ droppedIn \cap {"NONE", "CONNECTED"} = {}
    /\ st # "CONNECTED" => \A f \in SendFields \ RecvFields : conn[f] = 0

\* Outside a connection every per-connection field is Init; the kept config
\* is unchanged by every reset.
ResetIsInit ==
    /\ st \notin Live => conn = InitConn
    /\ st # "NONE" => kept.config = 1

\* The record holds nothing but Init and the current session's writes.
RecordIsCurrent == \A f \in Fields : conn[f] \in {0, cur}

\* A later session observes nothing of an earlier one: no packet of session
\* s opened by session s' # s, and no step of a session reads a field another
\* session wrote (a stale ack bit or sent entry, a replay window
\* that would refuse the new session's first sequences).
ReconnectObservesNothing == observed = {} /\ crossDelivered = {}

DestroyDisconnectsLive == ~silentEnd

\* The callback reports exactly the public state's changes: the value it
\* last reported is the state a poll reads, and a report is a change.
CallbackReportsPublicState ==
    /\ reported = st
    /\ lastReport # <<>> => lastReport[1] # lastReport[2] /\ lastReport[2] = st

\* A connect reaches CONNECTED or an error state, unless the application
\* disconnects or destroys the client first.
ConnectReachesConnectedOrError ==
    (st \in HandshakeStates) ~> (st \in {"CONNECTED", "DISCONNECTED", "NONE"} \cup Errors)
=============================================================================
