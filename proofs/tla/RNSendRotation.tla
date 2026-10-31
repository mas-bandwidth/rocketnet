--------------------------- MODULE RNSendRotation ---------------------------
(***************************************************************************)
(* The server's send loop across connections and the endpoint's write     *)
(* pass within one flush: rn_message_object_send_packets and              *)
(* rn_message_connection_write (sdk/rocketnet_message_server.cpp) over    *)
(* rn_endpoint_next_packet_bytes, rn_endpoint_write_packet,               *)
(* rn_endpoint_packet_sent, rn_endpoint_packet_failed,                     *)
(* rn_endpoint_begin_flush and rn_endpoint_begin_update                    *)
(* (sdk/rocketnet_endpoint.cpp), against the portable transport's         *)
(* per-flush send capacity (sdk/rocketnet_server_transport.cpp): a frame  *)
(* start takes a slot of payload_ring.num_packets, an abort does not      *)
(* return it, and the ring's count resets when the transport sends.       *)
(*                                                                         *)
(* RNOutgoingBuild models one connection's build and RNServerSendFair the *)
(* rotation abstractly (K clients served per update); this module carries *)
(* the loop's own arithmetic: the start index send_next_connection % N,   *)
(* rounds while any connection progresses, one frame per connection per  *)
(* round, the capacity test before each connection and the cursor set to  *)
(* (i + 1) % N after it.                                                   *)
(*                                                                         *)
(* A connection: its pass phase (RN_ENDPOINT_WRITE_IDLE, _RELIABLE,       *)
(* _UNRELIABLE, _DONE), new reliable messages ready, resends due, queued  *)
(* unreliable records, whether its window is stalled, whether its send    *)
(* budget pays this flush, and the budget's reliable_waiting.  Work and   *)
(* the stall and the budget change only between flushes (the environment)*)
(* : nothing else runs while send_packets does.  One visit of a           *)
(* connection is one action: rn_message_connection_write runs to its      *)
(* return with nothing interleaved.  packets_per_update is not modelled   *)
(* (taken as never reached).                                               *)
(*                                                                         *)
(* Reversed witnesses (each flag FALSE is a bug class):                    *)
(*   KeepDone    a pass that ended reopens on the next next_packet_bytes, *)
(*               within the same flush                                    *)
(*   StallGate   rn_endpoint_ordered_due answers yes to ready messages of *)
(*               a stalled window                                         *)
(*   CursorNext  send_next_connection is set to i, not (i + 1) % N         *)
(***************************************************************************)
EXTENDS Integers, FiniteSets

CONSTANTS
    N,          \* connections
    Cap,        \* the transport's frame starts per flush (send_capacity)
    MaxFlush,   \* the bounded exploration's flushes
    EnvStall,   \* the environment may stall a window
    EnvRefuse,  \* the environment may refuse a connection's budget
    KeepDone, StallGate, CursorNext

ASSUME N \in Nat \ {0} /\ Cap \in Nat \ {0} /\ MaxFlush \in Nat
ASSUME \A f \in {EnvStall, EnvRefuse, KeepDone, StallGate, CursorNext} : f \in BOOLEAN

Conns == 0..(N - 1)
Phases == {"idle", "rel", "unr", "done"}

VARIABLES
    conn,       \* conn[c]: [phase, ready, resend, unr, stalled, pays, waiting]
    pc,         \* "between" flushes or in the send "loop"
    start,      \* the loop's start index
    off,        \* the loop's offset in this round
    progress,   \* a connection progressed this round
    used,       \* payload_ring.num_packets: frames started this flush
    cursor,     \* send_next_connection
    flushes,    \* flushes begun
    aborts,     \* ghost: frames started and aborted for c this flush (to 2)
    served,     \* ghost: c had a frame written this flush
    due,        \* ghost: c had writable work when the flush began
    starve,     \* ghost: consecutive flushes c was due and not served
    stuck,      \* ghost: a flush ended with capacity left and work writable
    paidAbort   \* ghost: a frame was aborted for a connection its budget pays

vars == << conn, pc, start, off, progress, used, cursor, flushes, aborts, served, due, starve, stuck, paidAbort >>

-----------------------------------------------------------------------------
(* The endpoint, one connection record e at a time.                       *)

\* rn_endpoint_ordered_due
OrderedDue(e) == e.resend > 0 \/ (e.ready > 0 /\ (~StallGate \/ ~e.stalled))

\* rn_endpoint_end_reliable
EndReliable(e) ==
    IF e.waiting THEN [ e EXCEPT !.unr = 0, !.phase = "done" ]
    ELSE [ e EXCEPT !.phase = "unr" ]

\* the pass is over: KeepDone FALSE sends it back to idle, reopening it
EndPass(e) == [ e EXCEPT !.phase = IF KeepDone THEN "done" ELSE "idle" ]

\* rn_endpoint_next_packet_bytes: << e', kind >>,
\* kind "none" for 0
NPB(e) ==
    LET e1 == IF e.phase = "idle" THEN [ e EXCEPT !.phase = "rel", !.waiting = FALSE ] ELSE e
        e2 == IF e1.phase = "rel" /\ ~OrderedDue(e1) THEN EndReliable(e1) ELSE e1
    IN IF e2.phase = "rel" THEN << e2, "ord" >>
       ELSE IF e2.phase = "unr" /\ e2.unr > 0 THEN << e2, "unr" >>
       ELSE << EndPass(e2), "none" >>

\* rn_endpoint_write_packet then packet_sent or the
\* object's abort: << e', written >>.  An ordered packet the budget refuses
\* sets reliable_waiting; one with nothing to number (a stalled window and
\* no resend) writes nothing; either ends the reliable path.  An unreliable
\* packet the budget refuses drops every queued record and ends the pass.
WriteOrd(e) ==
    IF ~e.pays THEN << EndReliable([ e EXCEPT !.waiting = TRUE ]), FALSE >>
    ELSE IF e.resend > 0 THEN << [ e EXCEPT !.resend = e.resend - 1 ], TRUE >>
    ELSE IF e.ready > 0 /\ ~e.stalled THEN << [ e EXCEPT !.ready = e.ready - 1 ], TRUE >>
    ELSE << EndReliable(e), FALSE >>

WriteUnr(e) ==
    IF ~e.pays THEN << [ e EXCEPT !.unr = 0, !.phase = "done" ], FALSE >>
    ELSE << [ e EXCEPT !.unr = e.unr - 1 ], TRUE >>

\* rn_endpoint_packet_failed with nothing written:
\* no frame was started for the plan
FailedNoFrame(e, kind) ==
    IF kind = "ord" THEN EndReliable(e) ELSE [ e EXCEPT !.phase = "done" ]

\* rn_message_connection_write: plan, start a
\* frame (NULL once the capacity is spent), write, finish or abort; the loop
\* ends on the first written frame or when the pass answers 0.
\* Returns << e', used', aborted frames, written >>.
RECURSIVE ConnWrite(_, _, _, _)
ConnWrite(e, u, ab, fuel) ==
    LET r == NPB(e)
        e1 == r[1]
        kind == r[2]
    IN IF kind = "none" \/ fuel = 0 THEN << e1, u, ab, FALSE >>
       ELSE IF u >= Cap THEN ConnWrite(FailedNoFrame(e1, kind), u, ab, fuel - 1)
       ELSE LET w == IF kind = "ord" THEN WriteOrd(e1) ELSE WriteUnr(e1)
            IN IF w[2] THEN << w[1], u + 1, ab, TRUE >>
               ELSE ConnWrite(w[1], u + 1, ab + 1, fuel - 1)

-----------------------------------------------------------------------------

\* what the code would write for c at the start of a flush, given its phase
Writable(e) ==
    /\ e.pays
    /\ \/ e.unr > 0 /\ e.phase # "done"
       \/ e.phase \in {"idle", "rel"} /\ (e.resend > 0 \/ (e.ready > 0 /\ ~e.stalled))

Init ==
    /\ conn \in [ Conns -> [ phase : {"idle"}, ready : 0..1, resend : 0..1, unr : 0..1,
                             stalled : IF EnvStall THEN BOOLEAN ELSE {FALSE},
                             pays : IF EnvRefuse THEN BOOLEAN ELSE {TRUE},
                             waiting : {FALSE} ] ]
    /\ pc = "between"
    /\ start = 0 /\ off = 0 /\ progress = FALSE /\ used = 0
    /\ cursor = 0
    /\ flushes = 0
    /\ aborts = [ c \in Conns |-> 0 ]
    /\ served = [ c \in Conns |-> FALSE ]
    /\ due = [ c \in Conns |-> FALSE ]
    /\ starve = [ c \in Conns |-> 0 ]
    /\ stuck = FALSE
    /\ paidAbort = FALSE

\* rn_message_object_send_packets begins: every
\* connection's rn_endpoint_begin_flush, then the
\* start index from the cursor
BeginFlush ==
    /\ pc = "between"
    /\ flushes < MaxFlush
    /\ LET opened == [ c \in Conns |->
                        IF KeepDone /\ conn[c].phase = "done" THEN [ conn[c] EXCEPT !.phase = "idle" ] ELSE conn[c] ]
       IN /\ conn' = opened
          /\ due' = [ c \in Conns |-> Writable(opened[c]) ]
    /\ pc' = "loop"
    /\ start' = cursor % N
    /\ off' = 0
    /\ progress' = FALSE
    /\ used' = 0
    /\ flushes' = flushes + 1
    /\ aborts' = [ c \in Conns |-> 0 ]
    /\ served' = [ c \in Conns |-> FALSE ]
    /\ UNCHANGED << cursor, starve, stuck, paidAbort >>

\* the flush ends (the goto flush, or a round with no progress): the
\* transport sends and its ring count resets
EndFlushVars(sv) ==
    /\ pc' = "between"
    /\ starve' = [ c \in Conns |-> IF sv[c] \/ ~due[c] THEN 0 ELSE starve[c] + 1 ]

\* one connection of a round
Visit ==
    /\ pc = "loop"
    /\ LET i == (start + off) % N IN
       IF used >= Cap
       THEN \* send_capacity_available <= 0: goto flush
            /\ EndFlushVars(served)
            /\ served' = served
            /\ stuck' = stuck
            /\ UNCHANGED << conn, start, off, progress, used, cursor, flushes, aborts, due, paidAbort >>
       ELSE LET r == ConnWrite(conn[i], used, 0, 4)
                prog == progress \/ r[4]
                last == off + 1 = N
            IN /\ conn' = [ conn EXCEPT ![i] = r[1] ]
               /\ used' = r[2]
               /\ aborts' = [ aborts EXCEPT ![i] = IF aborts[i] + r[3] > 2 THEN 2 ELSE aborts[i] + r[3] ]
               /\ served' = [ served EXCEPT ![i] = served[i] \/ r[4] ]
               /\ paidAbort' = (paidAbort \/ (r[3] > 0 /\ conn[i].pays))
               /\ cursor' = IF CursorNext THEN (i + 1) % N ELSE i
               /\ IF last /\ ~prog
                  THEN /\ EndFlushVars(served')
                       /\ stuck' = (stuck \/ (r[2] < Cap /\ \E c \in Conns : Writable(conn'[c])))
                       /\ UNCHANGED << start, off, progress >>
                  ELSE /\ off' = IF last THEN 0 ELSE off + 1
                       /\ progress' = IF last THEN FALSE ELSE prog
                       /\ UNCHANGED << pc, starve, stuck, start >>
               /\ UNCHANGED << flushes, due >>

\* rn_endpoint_begin_update: the pass reopens
BeginUpdate ==
    /\ pc = "between"
    /\ conn' = [ c \in Conns |-> [ conn[c] EXCEPT !.phase = "idle" ] ]
    /\ UNCHANGED << pc, start, off, progress, used, cursor, flushes, aborts, served, due, starve, stuck, paidAbort >>

\* the environment between flushes: messages sent, resends coming due, an
\* ack clearing the stall or the window filling, the budget refilling or not
Env(c) ==
    /\ pc = "between"
    /\ \E rd \in 0..1, rs \in 0..1, ur \in 0..1,
          st \in (IF EnvStall THEN BOOLEAN ELSE {FALSE}),
          py \in (IF EnvRefuse THEN BOOLEAN ELSE {TRUE}) :
          conn' = [ conn EXCEPT ![c] = [ @ EXCEPT !.ready = rd, !.resend = rs, !.unr = ur,
                                                   !.stalled = st, !.pays = py ] ]
    /\ UNCHANGED << pc, start, off, progress, used, cursor, flushes, aborts, served, due, starve, stuck, paidAbort >>

Next ==
    \/ BeginFlush
    \/ Visit
    \/ BeginUpdate
    \/ \E c \in Conns : Env(c)

Spec == Init /\ [][Next]_vars

-----------------------------------------------------------------------------

TypeOK ==
    /\ \A c \in Conns : conn[c].phase \in Phases
    /\ pc \in {"between", "loop"}
    /\ used \in 0..Cap
    /\ cursor \in Conns

\* capacity goes to frames that are written: a connection whose budget pays
\* never has a frame aborted (a stalled window asks for none)
NoAbortWhilePaying == ~paidAbort

\* a connection the budget refuses aborts at most one frame a flush: its
\* ended pass stays ended
AbortsBounded == \A c \in Conns : aborts[c] <= 1

\* a flush ends with its capacity spent or nothing writable left
ServedWhenCapacity == ~stuck

\* a connection with writable work at each flush is served within N flushes
BoundedWait == \A c \in Conns : starve[c] < N
=============================================================================
