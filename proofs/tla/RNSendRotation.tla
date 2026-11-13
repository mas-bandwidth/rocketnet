--------------------------- MODULE RNSendRotation ---------------------------
(***************************************************************************)
(* The server's send loop across connections and the endpoint's write     *)
(* pass within one flush: rn_message_object_send_packets and              *)
(* rn_message_connection_write (sdk/rocketnet_message_server.cpp) over    *)
(* rn_endpoint_next_packet_bytes, rn_endpoint_write_may_be_refused,        *)
(* rn_endpoint_write_packet, rn_endpoint_packet_sent,                      *)
(* rn_endpoint_packet_failed, rn_endpoint_begin_flush and                  *)
(* rn_endpoint_begin_update (sdk/rocketnet_endpoint.cpp), against the      *)
(* portable transport's per-flush send capacity                            *)
(* (sdk/rocketnet_server_transport.cpp): a frame start takes a slot of     *)
(* payload_ring.num_packets, an abort does not return it, and the ring's   *)
(* count resets when the transport sends.  Since an abort never returns   *)
(* its slot, a frame is started only for a packet that will be written:   *)
(* the window's refusal is decided before the frame (the due test asks    *)
(* rn_window_send_would_number, RNWindow WouldNumber), and a packet the    *)
(* budget may refuse is written before its frame is started.  One abort   *)
(* the due test cannot rule out stays: the ordered path answers due for a *)
(* walk that then finds nothing to write (vain: rn_endpoint_fifo_due      *)
(* answers 1 after 64 acked steps, "let the write walk it", or the        *)
(* connection has ended: exhausted, a channel error, a reliable message   *)
(* past its timeout), so rn_ordered_write returns 0 into a started frame, *)
(* the frame is aborted and the reliable path ends                        *)
(* (rn_endpoint_write_packet, rn_endpoint_end_reliable): at most one such *)
(* abort per connection per flush (AbortsBounded), and none for a refusal *)
(* the code decides before a frame (NoRefusalAbort).                      *)
(*                                                                         *)
(* RNOutgoingBuild models one connection's build and RNServerSendFair the *)
(* same rounds at the loop's index arithmetic against the transport's     *)
(* capacity (send_writers, count, kept, previous, the stored cursors, the *)
(* send queues); this module carries the rounds with each connection's    *)
(* write pass: the start index send_next_connection % N,                  *)
(* a first round over every connection, then rounds over the connections  *)
(* that wrote in the round before (the writer list) while any wrote, one  *)
(* frame per connection per round, the capacity test before each visit,   *)
(* and the cursor as a walk over every connection would set it: (i + 1)   *)
(* % N after each visit, at a capacity cut the cursor held before the cut *)
(* connection's visit, and at the end past the last connection.           *)
(*                                                                         *)
(* A connection: its pass phase (RN_ENDPOINT_WRITE_IDLE, _RELIABLE,       *)
(* _UNRELIABLE, _DONE), new reliable messages ready, resends due, queued  *)
(* unreliable records, whether its window is stalled, whether the window  *)
(* can number the ready message (fits: an ack that moves U' clears the    *)
(* stall without making room when it does not), whether its send budget   *)
(* pays this flush, and the budget's reliable_waiting.  Work, the window  *)
(* and the budget change only between flushes (the environment): nothing *)
(* else runs while send_packets does.  One visit of a connection is one   *)
(* action: rn_message_connection_write runs to its return with nothing    *)
(* interleaved.  packets_per_update is not modelled (taken as never       *)
(* reached).  Every connection is connected.                              *)
(*                                                                         *)
(* Reversed witnesses (each flag FALSE is a bug class):                    *)
(*   KeepDone    a pass that ended reopens on the next next_packet_bytes, *)
(*               within the same flush                                    *)
(*   StallGate   rn_endpoint_ordered_due answers yes to ready messages of *)
(*               a stalled window                                         *)
(*   FitsGate    rn_endpoint_ordered_due answers yes to a ready message   *)
(*               the window cannot number (an ack cleared the stall       *)
(*               without making room)                                     *)
(*   WriteFirst  a packet the budget may refuse is written into a frame   *)
(*               already started (the frame is aborted when it refuses)   *)
(*   CursorNext  send_next_connection is set to i, not (i + 1) % N         *)
(*   CutCursor   at a capacity cut in a later round the cursor is left    *)
(*               where the last writer visited set it                     *)
(*   VainEnds    a vain ordered write leaves the reliable path open, so   *)
(*               the next plan starts and aborts another frame            *)
(* EnvVain lets the environment make a connection's ordered path vain.    *)
(***************************************************************************)
EXTENDS Integers, FiniteSets

CONSTANTS
    N,          \* connections
    Cap,        \* the transport's frame starts per flush (send_capacity)
    MaxFlush,   \* the bounded exploration's flushes
    EnvStall,   \* the environment may stall a window
    EnvShortAck,\* the environment may clear a stall without making room
    EnvRefuse,  \* the environment may refuse a connection's budget
    EnvVain,    \* the environment may make a connection's ordered path vain
    KeepDone, StallGate, FitsGate, WriteFirst, CursorNext, CutCursor, VainEnds

ASSUME N \in Nat \ {0} /\ Cap \in Nat \ {0} /\ MaxFlush \in Nat
ASSUME \A f \in {EnvStall, EnvShortAck, EnvRefuse, EnvVain, KeepDone, StallGate, FitsGate,
                 WriteFirst, CursorNext, CutCursor, VainEnds} : f \in BOOLEAN

Conns == 0..(N - 1)
Phases == {"idle", "rel", "unr", "done"}

VARIABLES
    conn,       \* conn[c]: [phase, ready, resend, unr, stalled, fits, pays, waiting, vain]
    pc,         \* "between" flushes or in the send "loop"
    start,      \* the loop's start index
    off,        \* the loop's offset in this round
    later,      \* a round after the first: only the writer list is visited
    listed,     \* the writer list: the connections that wrote in the round before
    wrote,      \* the connections that wrote in this round
    used,       \* payload_ring.num_packets: frames started this flush
    cursor,     \* send_next_connection
    flushes,    \* flushes begun
    aborts,     \* ghost: frames started and aborted for c this flush (to 2)
    badAborts,  \* ghost: of those, the ones not for a vain ordered write (to 2)
    served,     \* ghost: c had a frame written this flush
    due,        \* ghost: c had writable work when the flush began
    starve,     \* ghost: consecutive flushes c was due and not served
    stuck,      \* ghost: a flush ended with capacity left and work writable
    visits,     \* ghost: connection slots visited this flush (the code's slot_visits)
    fullCursor, \* ghost: the cursor a walk over every connection in every round sets
    skipOk      \* ghost: every slot the writer list skipped would have written nothing

vars == << conn, pc, start, off, later, listed, wrote, used, cursor, flushes, aborts, badAborts,
           served, due, starve, stuck, visits, fullCursor, skipOk >>

-----------------------------------------------------------------------------
(* The endpoint, one connection record e at a time.                       *)

\* rn_endpoint_ordered_due: new messages are due while the window is not
\* stalled and can number the ready one (rn_window_send_would_number);
\* resends due; or a vain walk, which the test answers due without knowing
\* the write will find nothing (rn_endpoint_fifo_due's 64 acked steps, or an
\* ended connection)
OrderedDueBase(e) == e.resend > 0 \/ (e.ready > 0 /\ (~StallGate \/ ~e.stalled) /\ (~FitsGate \/ e.fits))
OrderedDue(e) == e.vain \/ OrderedDueBase(e)

\* rn_endpoint_end_reliable
EndReliable(e) ==
    IF e.waiting THEN [ e EXCEPT !.unr = 0, !.phase = "done" ]
    ELSE [ e EXCEPT !.phase = "unr" ]

\* the pass is over: KeepDone FALSE sends it back to idle, reopening it
EndPass(e) == [ e EXCEPT !.phase = IF KeepDone THEN "done" ELSE "idle" ]

\* rn_window_send_stall_unless_numbered in rn_endpoint_next_packet_bytes: a
\* ready message the window cannot number stalls the window before a frame
StallUnlessNumbered(e) ==
    IF FitsGate /\ e.phase = "rel" /\ e.ready > 0 /\ ~e.stalled /\ ~e.fits
    THEN [ e EXCEPT !.stalled = TRUE ] ELSE e

\* rn_endpoint_next_packet_bytes: << e', kind >>, kind "none" for 0
NPB(e) ==
    LET e0 == IF e.phase = "idle" THEN [ e EXCEPT !.phase = "rel", !.waiting = FALSE ] ELSE e
        e1 == StallUnlessNumbered(e0)
        e2 == IF e1.phase = "rel" /\ ~OrderedDue(e1) THEN EndReliable(e1) ELSE e1
    IN IF e2.phase = "rel" THEN << e2, "ord" >>
       ELSE IF e2.phase = "unr" /\ e2.unr > 0 THEN << e2, "unr" >>
       ELSE << EndPass(e2), "none" >>

\* rn_endpoint_write_packet: << e', written >>.  An ordered packet the
\* budget refuses sets reliable_waiting; one with nothing to number (a
\* stalled window, or a ready message that does not fit, and no resend; or
\* a vain walk) writes nothing and stalls the window; either ends the
\* reliable path (VainEnds FALSE: a vain write leaves it open).  An
\* unreliable packet the budget refuses drops every queued record and ends
\* the pass.
WriteOrd(e) ==
    IF ~e.pays THEN << EndReliable([ e EXCEPT !.waiting = TRUE ]), FALSE >>
    ELSE IF e.resend > 0 THEN << [ e EXCEPT !.resend = e.resend - 1 ], TRUE >>
    ELSE IF e.ready > 0 /\ ~e.stalled /\ e.fits THEN << [ e EXCEPT !.ready = e.ready - 1 ], TRUE >>
    ELSE IF e.vain /\ ~VainEnds THEN << e, FALSE >>
    ELSE << EndReliable([ e EXCEPT !.stalled = e.stalled \/ e.ready > 0 ]), FALSE >>

WriteUnr(e) ==
    IF ~e.pays THEN << [ e EXCEPT !.unr = 0, !.phase = "done" ], FALSE >>
    ELSE << [ e EXCEPT !.unr = e.unr - 1 ], TRUE >>

Write(e, kind) == IF kind = "ord" THEN WriteOrd(e) ELSE WriteUnr(e)

\* rn_endpoint_packet_failed with nothing written:
\* no frame was started for the plan
FailedNoFrame(e, kind) ==
    IF kind = "ord" THEN EndReliable(e) ELSE [ e EXCEPT !.phase = "done" ]

\* rn_endpoint_write_may_be_refused: the budget may refuse it.  In the code
\* this is a bucket below one maximum packet; here, a budget that does not
\* pay this flush (one that pays is written in place, one that does not is
\* written first and refused).
\* proofs: WriteFirst (sdk/rocketnet_message_server.cpp
\* rn_message_connection_write)
MayBeRefused(e) == ~e.pays

\* rn_message_connection_write: plan; a packet the budget may refuse is
\* written first and a frame is started only when it was written; else
\* start a frame (NULL once the capacity is spent), write, finish or abort;
\* the loop ends on the first written frame or when the pass answers 0.
\* Returns << e', used', aborted frames, written, aborted frames not for a
\* vain ordered write >>.
RECURSIVE ConnWrite(_, _, _, _, _)
ConnWrite(e, u, ab, bad, fuel) ==
    LET r == NPB(e)
        e1 == r[1]
        kind == r[2]
    IN IF kind = "none" \/ fuel = 0 THEN << e1, u, ab, FALSE, bad >>
       ELSE IF WriteFirst /\ MayBeRefused(e1)
       THEN \* written first: a refusal takes no frame
            LET w == Write(e1, kind)
            IN IF w[2] THEN << w[1], u + 1, ab, TRUE, bad >>
               ELSE ConnWrite(w[1], u, ab, bad, fuel - 1)
       ELSE IF u >= Cap THEN ConnWrite(FailedNoFrame(e1, kind), u, ab, bad, fuel - 1)
       ELSE LET w == Write(e1, kind)
                vain == kind = "ord" /\ e1.vain /\ e1.pays /\ ~OrderedDueBase(e1)
            IN IF w[2] THEN << w[1], u + 1, ab, TRUE, bad >>
               ELSE ConnWrite(w[1], u + 1, ab + 1, IF vain THEN bad ELSE bad + 1, fuel - 1)

-----------------------------------------------------------------------------

\* what the code would write for c at the start of a flush, given its phase
Writable(e) ==
    /\ e.pays
    /\ \/ e.unr > 0 /\ e.phase # "done"
       \/ e.phase \in {"idle", "rel"} /\ (e.resend > 0 \/ (e.ready > 0 /\ ~e.stalled /\ e.fits))

Next1(c) == (c + 1) % N
Prev1(c) == (c + N - 1) % N

\* the cursor a visit of i leaves: (i + 1) % N, or i under CursorNext FALSE
After(i) == IF CursorNext THEN Next1(i) ELSE i

Init ==
    /\ conn \in [ Conns -> [ phase : {"idle"}, ready : 0..1, resend : 0..1, unr : 0..1,
                             stalled : IF EnvStall THEN BOOLEAN ELSE {FALSE},
                             fits : IF EnvShortAck THEN BOOLEAN ELSE {TRUE},
                             pays : IF EnvRefuse THEN BOOLEAN ELSE {TRUE},
                             waiting : {FALSE},
                             vain : IF EnvVain THEN BOOLEAN ELSE {FALSE} ] ]
    /\ pc = "between"
    /\ start = 0 /\ off = 0 /\ later = FALSE /\ listed = {} /\ wrote = {} /\ used = 0
    /\ cursor = 0
    /\ flushes = 0
    /\ aborts = [ c \in Conns |-> 0 ]
    /\ badAborts = [ c \in Conns |-> 0 ]
    /\ served = [ c \in Conns |-> FALSE ]
    /\ due = [ c \in Conns |-> FALSE ]
    /\ starve = [ c \in Conns |-> 0 ]
    /\ stuck = FALSE
    /\ visits = 0
    /\ fullCursor = 0
    /\ skipOk = TRUE

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
    /\ later' = FALSE
    /\ listed' = {}
    /\ wrote' = {}
    /\ used' = 0
    /\ flushes' = flushes + 1
    /\ aborts' = [ c \in Conns |-> 0 ]
    /\ badAborts' = [ c \in Conns |-> 0 ]
    /\ served' = [ c \in Conns |-> FALSE ]
    /\ visits' = 0
    /\ fullCursor' = cursor
    /\ UNCHANGED << cursor, starve, stuck, skipOk >>

\* the flush ends (the goto flush, or a round with no writer): the
\* transport sends and its ring count resets
EndFlushVars(sv) ==
    /\ pc' = "between"
    /\ starve' = [ c \in Conns |-> IF sv[c] \/ ~due[c] THEN 0 ELSE starve[c] + 1 ]

\* the end of a round: another over this round's writers, or the flush ends
\* with the cursor past the last connection (rn_message_object_send_packets
\* after its rounds)
EndRound(w, sv, fc) ==
    IF w = {}
    THEN /\ EndFlushVars(sv)
         /\ cursor' = After(Prev1(start))
         /\ fullCursor' = fc
         /\ UNCHANGED << off, later, listed, wrote >>
    ELSE /\ off' = 0
         /\ later' = TRUE
         /\ listed' = w
         /\ wrote' = {}
         /\ fullCursor' = fc
         /\ UNCHANGED << pc, starve >>

\* one offset of a round: the first round visits every connection; a later
\* round visits only the writer list, in the same cyclic order
\* rn_message_object_send_packets
Visit ==
    /\ pc = "loop"
    /\ LET i == (start + off) % N
           last == off + 1 = N
       IN
       IF later /\ i \notin listed
       THEN \* not on the writer list: the code never visits it; a walk over
            \* every connection would, and must find nothing to write
            /\ skipOk' = (skipOk /\ NPB(conn[i])[2] = "none")
            /\ IF last
               THEN /\ EndRound(wrote, served, After(i))
                    /\ stuck' = (stuck \/ (wrote = {} /\ used < Cap /\ \E c \in Conns : Writable(conn[c])))
                    /\ IF wrote = {} THEN TRUE ELSE UNCHANGED cursor
                    /\ UNCHANGED << conn, start, used, flushes, aborts, badAborts, served, due, visits >>
               ELSE /\ off' = off + 1
                    /\ fullCursor' = After(i)
                    /\ UNCHANGED << conn, pc, start, later, listed, wrote, used, cursor, flushes, aborts, badAborts,
                                    served, due, starve, stuck, visits >>
       ELSE IF used >= Cap
       THEN \* send_capacity_available <= 0: goto flush.  The cursor is the
            \* one held before this connection's visit: in the first round the
            \* last visit's, in a later round the stored one
            /\ EndFlushVars(served)
            /\ cursor' = IF later /\ CutCursor THEN After(Prev1(i)) ELSE cursor
            /\ visits' = visits + 1
            /\ UNCHANGED << conn, start, off, later, listed, wrote, used, flushes, aborts, badAborts, served, due,
                            stuck, fullCursor, skipOk >>
       ELSE LET r == ConnWrite(conn[i], used, 0, 0, 4)
                w == IF r[4] THEN wrote \cup {i} ELSE wrote
                sv == [ served EXCEPT ![i] = served[i] \/ r[4] ]
            IN /\ conn' = [ conn EXCEPT ![i] = r[1] ]
               /\ used' = r[2]
               /\ aborts' = [ aborts EXCEPT ![i] = IF aborts[i] + r[3] > 2 THEN 2 ELSE aborts[i] + r[3] ]
               /\ badAborts' = [ badAborts EXCEPT ![i] = IF badAborts[i] + r[5] > 2 THEN 2 ELSE badAborts[i] + r[5] ]
               /\ served' = sv
               /\ visits' = visits + 1
               /\ IF last
                  THEN /\ EndRound(w, sv, After(i))
                       /\ stuck' = (stuck \/ (w = {} /\ r[2] < Cap /\ \E c \in Conns : Writable(conn'[c])))
                       /\ IF w = {} THEN TRUE ELSE cursor' = After(i)
                       /\ UNCHANGED << start >>
                  ELSE /\ off' = off + 1
                       /\ wrote' = w
                       /\ cursor' = After(i)
                       /\ fullCursor' = After(i)
                       /\ UNCHANGED << pc, start, later, listed, starve, stuck >>
               /\ UNCHANGED << flushes, due, skipOk >>

\* rn_endpoint_begin_update: the pass reopens
BeginUpdate ==
    /\ pc = "between"
    /\ conn' = [ c \in Conns |-> [ conn[c] EXCEPT !.phase = "idle" ] ]
    /\ UNCHANGED << pc, start, off, later, listed, wrote, used, cursor, flushes, aborts, badAborts, served, due,
                    starve, stuck, visits, fullCursor, skipOk >>

\* the environment between flushes: messages sent, resends coming due, an
\* ack clearing the stall (with or without the room the ready message needs)
\* or the window filling, the budget refilling or not, a long acked prefix
\* in a resend FIFO or the connection ending (vain)
Env(c) ==
    /\ pc = "between"
    /\ \E rd \in 0..1, rs \in 0..1, ur \in 0..1,
          st \in (IF EnvStall THEN BOOLEAN ELSE {FALSE}),
          ft \in (IF EnvShortAck THEN BOOLEAN ELSE {TRUE}),
          py \in (IF EnvRefuse THEN BOOLEAN ELSE {TRUE}),
          vn \in (IF EnvVain THEN BOOLEAN ELSE {FALSE}) :
          conn' = [ conn EXCEPT ![c] = [ @ EXCEPT !.ready = rd, !.resend = rs, !.unr = ur,
                                                   !.stalled = st, !.fits = ft, !.pays = py,
                                                   !.vain = vn ] ]
    /\ UNCHANGED << pc, start, off, later, listed, wrote, used, cursor, flushes, aborts, badAborts, served, due,
                    starve, stuck, visits, fullCursor, skipOk >>

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
    /\ listed \subseteq Conns /\ wrote \subseteq Conns

\* no frame of the capacity is started and aborted in the send pass for a
\* refusal the code decides before a frame (the budget's, the window's):
\* the only abort is a vain ordered write's
NoRefusalAbort == \A c \in Conns : badAborts[c] = 0

\* a vain ordered write aborts at most one frame per connection per flush:
\* it ends the reliable path, which does not reopen within the flush
AbortsBounded == \A c \in Conns : aborts[c] <= 1

\* the writer list's premise: a connection off the list in a later round
\* has ended its pass until the next flush
UnlistedPassEnded ==
    (pc = "loop" /\ later) => \A c \in Conns \ listed : conn[c].phase = "done"

\* every slot the writer list skips is one a walk over every connection
\* would have found nothing to write in
SkippedWritesNothing == skipOk

\* the cursor between flushes is the one a walk over every connection in
\* every round leaves
CursorAsFullWalk == pc = "between" => cursor = fullCursor

\* a flush visits each slot once, then one slot per frame started
VisitsBounded == visits <= N + used

\* a flush ends with its capacity spent or nothing writable left
ServedWhenCapacity == ~stuck

\* a connection with writable work at each flush is served within N flushes
BoundedWait == \A c \in Conns : starve[c] < N
=============================================================================
