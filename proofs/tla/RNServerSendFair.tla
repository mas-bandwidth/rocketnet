------------------------- MODULE RNServerSendFair -------------------------
(***************************************************************************)
(* The server's per-update send rotation (sdk/rocketnet_message_server.cpp, *)
(* rn_message_object_send_packets): the transport offers K payload frames   *)
(* per update (send_capacity, sdk/rocketnet_server_transport.cpp), and the  *)
(* send pass starts at the connection after the last one served, so the     *)
(* clients served first rotate across updates. The rule this model states   *)
(* is bounded service: with N connected clients and a cap of K frames per   *)
(* update, every client is served within ceil(N / K) updates.               *)
(*                                                                         *)
(* Tick serves the next K clients in cyclic order and moves the cursor     *)
(* after the last of them. Mode = "fromzero" is the reversed witness: the  *)
(* pass starts at index 1 every update, so with K < N the low indices      *)
(* consume the capacity and a higher index starves. Mode = "inplace" is    *)
(* the second reversed witness: the first client keeps the cursor and      *)
(* consumes the whole cap, so every later client starves.                  *)
(*                                                                         *)
(* Per-client scope (MESSAGES.md m1.8, revision 1.1): Dark is a set of     *)
(* clients whose path is dark or that never acknowledge, so they always    *)
(* have resends due.  The rule gives each connection one frame per round   *)
(* (rn_message_object_send_packets), so a dark client takes no more of the *)
(* cap than any other (OtherClientUnaffected).  Mode = "darkhogs" is the   *)
(* reversed witness: a dark client with resends due takes the whole cap   *)
(* each update, a per-client allowance the one-frame-per-round rule rules *)
(* out.                                                                    *)
(*                                                                         *)
(* The model covers only the service order; it does not model queueing,    *)
(* retransmission or the payload content. A client is served when a frame  *)
(* is started for it, and a full ring defers that client's sends to the    *)
(* next update rather than dropping its reliability state.                 *)
(*                                                                         *)
(* Revision 1.2, the rounds of one flush (Mode = "rounds" and its reversed *)
(* witnesses), at the code's own index arithmetic: slots 0..N-1 of which   *)
(* Conn are connected, send_next_connection, the writer list               *)
(* send_writers (two entries a writer: its slot, and the cursor held       *)
(* before its visit, -1 for "past the last connected slot"), its count,    *)
(* the in-place compaction through kept, previous, and the transport's     *)
(* capacity K tested before each visit.  A flush is a sequence of rounds:  *)
(* the first visits every connected slot once in cyclic order from the     *)
(* cursor; each later round visits exactly the slots that wrote a frame in *)
(* the round before, one frame each, in the same cyclic order; a slot that *)
(* writes nothing has ended its pass for the flush; the flush ends when a  *)
(* round writes nothing or the capacity is spent.  The cursor is the one a *)
(* walk over every connected slot in every round would leave (the v1.1.1   *)
(* loop): past each slot visited, at a capacity cut the value held before  *)
(* the cut slot's visit.  A flush costs at most N + frames visits.  A      *)
(* slot's work is the frames it has due at the flush's start; it writes    *)
(* one frame a visit while any is left, and nothing once none is.          *)
(*   Mode = "revisit":   the compaction keeps a slot that wrote nothing    *)
(*   Mode = "cutcursor": a cut in a later round leaves the cursor where    *)
(*                       the last visit set it                             *)
(*   Mode = "reorder":   later rounds walk the list from its end           *)
(***************************************************************************)
EXTENDS Naturals

CONSTANTS N,          \* connected clients, 1..N (rounds: slots 0..N-1)
          K,          \* payload frames the transport starts per update
          Mode,       \* "fair", "fromzero", "inplace", "darkhogs";
                      \* "rounds", "revisit", "cutcursor", "reorder"
          MaxTicks,   \* the bounded exploration's last update
          Dark,       \* the clients that never acknowledge: resends always due
          Conn,       \* rounds: the connected slots, a subset of 0..N-1
          WorkMax     \* rounds: the most frames a slot has due at a flush

RoundModes == {"rounds", "revisit", "cutcursor", "reorder"}

ASSUME N \in Nat \ {0}
ASSUME K \in 1..N
ASSUME Mode \in {"fair", "fromzero", "inplace", "darkhogs"} \cup RoundModes
ASSUME MaxTicks \in Nat \ {0}
ASSUME Dark \subseteq 1..N
ASSUME Mode = "darkhogs" => Dark # {}
ASSUME Conn \subseteq 0..(N - 1)
ASSUME WorkMax \in Nat

Clients == 1..N
Slots == 0..(N - 1)
Entries == 0..(2 * N - 1)

Window == (N + K - 1) \div K

VARIABLES now,       \* the update number, from 0
          cursor,    \* the first client the next fair pass offers a frame
          last,      \* the last update each client was served, 0 before the first
          \* revision 1.2, the code's state
          pc,        \* "between" flushes, "first" round, "later" rounds
          snc,       \* send_next_connection
          start,     \* the loop's start
          off,       \* the first round's offset
          prev,      \* previous: the connected slot visited last in the first round, -1 before
          w,         \* send_writers[0 .. 2N-1]
          count,     \* the writer list's length
          k,         \* the later round's index into the list
          kept,      \* the compaction's length
          used,      \* frames started this flush (the transport's capacity count)
          work,      \* frames each slot has due
          \* revision 1.2, ghosts
          visits,    \* slot visits this flush (the development seam's slot_visits)
          wrotePrev, \* the slots that wrote a frame in the round before
          wroteNow,  \* the slots that wrote a frame in this round
          ended,     \* the slots that wrote nothing in a round of this flush
          lastPos,   \* this round's last visited slot's cyclic position from start, -1 at a round's start
          ok         \* every visit and cursor so far kept the statements

vars == <<now, cursor, last, pc, snc, start, off, prev, w, count, k, kept, used, work,
          visits, wrotePrev, wroteNow, ended, lastPos, ok>>
roundVars == <<pc, snc, start, off, prev, w, count, k, kept, used, work,
               visits, wrotePrev, wroteNow, ended, lastPos, ok>>

Constraint == now <= MaxTicks

First == IF Mode = "fromzero" THEN 1 ELSE cursor

Count == IF Mode = "inplace" THEN 1 ELSE K

Served == IF Mode = "darkhogs"
          THEN { CHOOSE d \in Dark : TRUE }
          ELSE { ((First + j - 1) % N) + 1 : j \in 0..Count-1 }

NextCursor ==
    IF Mode = "fair"
    THEN ((First + K - 1) % N) + 1
    ELSE First

Init ==
    /\ now = 0
    /\ cursor = 1
    /\ last = [ c \in Clients |-> 0 ]
    /\ pc = "between"
    /\ snc = 0
    /\ start = 0
    /\ off = 0
    /\ prev = -1
    /\ w = [ e \in Entries |-> 0 ]
    /\ count = 0
    /\ k = 0
    /\ kept = 0
    /\ used = 0
    /\ work = [ s \in Slots |-> 0 ]
    /\ visits = 0
    /\ wrotePrev = {}
    /\ wroteNow = {}
    /\ ended = {}
    /\ lastPos = -1
    /\ ok = [ order |-> TRUE, writers |-> TRUE, cut |-> TRUE, cost |-> TRUE ]

\* one update: the pass serves Served, then the cursor names the next first
Tick ==
    /\ Mode \notin RoundModes
    /\ now < MaxTicks
    /\ now' = now + 1
    /\ last' = [ c \in Clients |-> IF c \in Served THEN now' ELSE last[c] ]
    /\ cursor' = NextCursor
    /\ UNCHANGED roundVars

-----------------------------------------------------------------------------
(* Revision 1.2: one flush of rn_message_object_send_packets.             *)

\* a slot's cyclic position from the loop's start
Pos(s) == (s - start + N) % N

\* the cursor a walk over every connected slot holds just before it visits
\* s in a round (the v1.1.1 loop): past the connected slot before s in this
\* round's order, or, with none before it, the value the round began with
\* (the flush's cursor in the first round, past previous in a later one)
FullBefore(s, roundStart) ==
    LET before == { t \in Conn : Pos(t) < Pos(s) }
        lastBefore == CHOOSE t \in before : \A u \in before : Pos(u) <= Pos(t)
    IN IF before = {} THEN roundStart ELSE (lastBefore + 1) % N

\* the round's starting cursor, as the full walk holds it
RoundStart == IF pc = "first" THEN snc ELSE (prev + 1) % N

\* the environment between flushes: the frames each connected slot has due
\* (a dark client always has frames due)
BeginFlush ==
    /\ Mode \in RoundModes
    /\ pc = "between"
    /\ now < MaxTicks
    /\ \E wk \in [ Slots -> 0..WorkMax ] :
          work' = [ s \in Slots |-> IF s \notin Conn THEN 0
                                    ELSE IF (s + 1) \in Dark THEN WorkMax ELSE wk[s] ]
    /\ now' = now + 1
    /\ pc' = "first"
    /\ start' = snc % N
    /\ off' = 0
    /\ prev' = -1
    /\ count' = 0
    /\ used' = 0
    /\ visits' = 0
    /\ wrotePrev' = {}
    /\ wroteNow' = {}
    /\ ended' = {}
    /\ lastPos' = -1
    /\ UNCHANGED <<cursor, last, snc, w, k, kept, ok>>

\* the flush ends: the cursor past previous when the rounds ran out
EndFlush(sncNext, okNext) ==
    /\ pc' = "between"
    /\ snc' = sncNext
    /\ ok' = okNext

\* one offset of the first round
FirstVisit ==
    /\ pc = "first"
    /\ LET i == (start + off) % N
           isLast == off + 1 = N
       IN
       /\ visits' = visits + 1
       /\ IF i \notin Conn
          THEN \* not connected: continue (no capacity test, no cursor)
               /\ IF isLast
                  THEN IF count = 0
                       THEN /\ EndFlush(IF prev >= 0 THEN (prev + 1) % N ELSE snc, ok)
                            /\ UNCHANGED <<off, count, k, kept, wrotePrev, wroteNow, lastPos>>
                       ELSE /\ pc' = "later" /\ k' = 0 /\ kept' = 0
                            /\ wrotePrev' = wroteNow /\ wroteNow' = {} /\ lastPos' = -1
                            /\ UNCHANGED <<snc, ok, off, count>>
                  ELSE /\ off' = off + 1
                       /\ UNCHANGED <<pc, snc, ok, count, k, kept, wrotePrev, wroteNow, lastPos>>
               /\ UNCHANGED <<now, cursor, last, start, prev, w, used, work, ended>>
          ELSE IF used >= K
          THEN \* send_capacity_available <= 0: goto flush, the cursor as last set
               /\ EndFlush(snc, [ ok EXCEPT !.cut = @ /\ snc = FullBefore(i, RoundStart),
                                            !.cost = @ /\ visits + 1 <= N + used ])
               /\ UNCHANGED <<now, cursor, last, start, off, prev, w, count, k, kept, used, work,
                              wrotePrev, wroteNow, ended, lastPos>>
          ELSE LET wrote == work[i] > 0
                   w1 == IF wrote
                         THEN [ w EXCEPT ![2 * count] = i,
                                         ![2 * count + 1] = IF prev < 0 THEN -1 ELSE (prev + 1) % N ]
                         ELSE w
                   c1 == IF wrote THEN count + 1 ELSE count
                   ok1 == [ ok EXCEPT !.order = @ /\ Pos(i) > lastPos,
                                      !.cost = @ /\ visits + 1 <= N + used + (IF wrote THEN 1 ELSE 0) ]
               IN
               /\ work' = [ work EXCEPT ![i] = IF wrote THEN @ - 1 ELSE @ ]
               /\ used' = IF wrote THEN used + 1 ELSE used
               /\ w' = w1
               /\ ended' = IF wrote THEN ended ELSE ended \cup {i}
               /\ prev' = i
               /\ IF isLast
                  THEN IF c1 = 0
                       THEN /\ EndFlush((i + 1) % N, ok1)
                            /\ count' = c1
                            /\ UNCHANGED <<off, k, kept, wrotePrev, wroteNow, lastPos>>
                       ELSE /\ pc' = "later" /\ k' = 0 /\ kept' = 0 /\ count' = c1
                            /\ snc' = (i + 1) % N /\ ok' = ok1
                            /\ wrotePrev' = (IF wrote THEN wroteNow \cup {i} ELSE wroteNow)
                            /\ wroteNow' = {} /\ lastPos' = -1
                            /\ UNCHANGED off
                  ELSE /\ off' = off + 1 /\ count' = c1
                       /\ snc' = (i + 1) % N /\ ok' = ok1
                       /\ wroteNow' = (IF wrote THEN wroteNow \cup {i} ELSE wroteNow)
                       /\ lastPos' = Pos(i)
                       /\ UNCHANGED <<pc, k, kept, wrotePrev>>
               /\ UNCHANGED <<now, cursor, last, start>>

\* one entry of a later round: the writer list, compacted in place
LaterVisit ==
    /\ pc = "later"
    /\ LET idx == IF Mode = "reorder" THEN count - 1 - k ELSE k
           i == w[2 * idx]
           stored == w[2 * idx + 1]
           cur == IF stored < 0 THEN (prev + 1) % N ELSE stored
           isLast == k + 1 = count
           okv == [ ok EXCEPT !.order = @ /\ Pos(i) > lastPos,
                              !.writers = @ /\ i \in wrotePrev /\ i \notin ended ]
       IN
       /\ visits' = visits + 1
       /\ IF used >= K
          THEN \* goto flush with the stored cursor
               /\ EndFlush(IF Mode = "cutcursor" THEN snc ELSE cur,
                           [ okv EXCEPT !.cut = @ /\ (IF Mode = "cutcursor" THEN snc ELSE cur)
                                                       = FullBefore(i, RoundStart),
                                        !.cost = @ /\ visits + 1 <= N + used ])
               /\ UNCHANGED <<now, cursor, last, start, off, prev, w, count, k, kept, used, work,
                              wrotePrev, wroteNow, ended, lastPos>>
          ELSE LET wrote == work[i] > 0
                   keep == wrote \/ Mode = "revisit"
                   w1 == IF keep THEN [ w EXCEPT ![2 * kept + 1] = stored, ![2 * kept] = i ] ELSE w
                   kept1 == IF keep THEN kept + 1 ELSE kept
                   wn == IF wrote THEN wroteNow \cup {i} ELSE wroteNow
                   ok1 == [ okv EXCEPT !.cost = @ /\ visits + 1 <= N + used + (IF wrote THEN 1 ELSE 0) ]
               IN
               /\ work' = [ work EXCEPT ![i] = IF wrote THEN @ - 1 ELSE @ ]
               /\ used' = IF wrote THEN used + 1 ELSE used
               /\ w' = w1
               /\ ended' = IF wrote THEN ended ELSE ended \cup {i}
               /\ IF isLast
                  THEN IF kept1 = 0
                       THEN /\ EndFlush((prev + 1) % N, ok1)
                            /\ count' = 0
                            /\ UNCHANGED <<k, kept, wrotePrev, wroteNow, lastPos>>
                       ELSE /\ count' = kept1 /\ k' = 0 /\ kept' = 0
                            /\ snc' = (i + 1) % N /\ ok' = ok1
                            /\ wrotePrev' = wn /\ wroteNow' = {} /\ lastPos' = -1
                            /\ UNCHANGED pc
                  ELSE /\ k' = k + 1 /\ kept' = kept1
                       /\ snc' = (i + 1) % N /\ ok' = ok1
                       /\ wroteNow' = wn /\ lastPos' = Pos(i)
                       /\ UNCHANGED <<pc, count, wrotePrev>>
               /\ UNCHANGED <<now, cursor, last, start, off, prev>>

Next == Tick \/ BeginFlush \/ FirstVisit \/ LaterVisit \/ UNCHANGED vars

Spec == Init /\ [][Next]_vars

-----------------------------------------------------------------------------

TypeOK ==
    /\ now \in 0..MaxTicks
    /\ cursor \in Clients
    /\ last \in [ Clients -> 0..MaxTicks ]
    /\ pc \in {"between", "first", "later"}
    /\ snc \in Slots
    /\ count \in 0..N /\ kept \in 0..N /\ used \in 0..K
    /\ w \in [ Entries -> -1..(N - 1) ]

\* under a cap of K frames per update and N clients, every client is served
\* within ceil(N / K) updates
FairService ==
    \A c \in Clients : now - last[c] < Window

\* a client that is dark or never acknowledges leaves every other client's
\* service as it was: each is still served within ceil(N / K) updates
OtherClientUnaffected ==
    \A c \in Clients \ Dark : now - last[c] < Window

\* revision 1.2.  Every round visits its slots in cyclic order from the
\* loop's start, each at most once, the later rounds in the first's order
SameOrder == ok.order

\* a later round visits exactly the slots that wrote in the round before:
\* only those (never one whose pass ended), and every one of them
OnlyLastWriters ==
    /\ ok.writers
    /\ (pc = "later" /\ k = 0) => { w[2 * j] : j \in 0..(count - 1) } = wrotePrev

\* a slot that wrote nothing has no frame left to write this flush
EndedWritesNothing == \A s \in ended : work[s] = 0

\* the cursor at a capacity cut is the one the walk over every connected
\* slot would hold
CursorAsFullWalk == ok.cut

\* a flush costs at most N visits plus one per frame written
VisitsBounded == ok.cost /\ visits <= N + used
=============================================================================
