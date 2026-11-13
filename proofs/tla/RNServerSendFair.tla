------------------------- MODULE RNServerSendFair -------------------------
(***************************************************************************)
(* The server's per-update send pass (sdk/rocketnet_message_server.cpp,   *)
(* rn_message_object_send_packets and rn_message_connection_write) over    *)
(* the transport's shared per-update frame capacity K (send_capacity,     *)
(* sdk/rocketnet_server_transport.cpp), at the loop's own index           *)
(* arithmetic, revision 1.3.                                               *)
(*                                                                         *)
(* The rounds of one flush: slots 0..N-1 of which Conn are connected,      *)
(* send_next_connection (snc), the writer list send_writers (two entries a *)
(* writer: its slot, and the cursor held before its visit, -1 for "past    *)
(* the last connected slot"), its count, the in-place compaction through   *)
(* kept, previous, and the capacity tested before each connected visit.   *)
(* The first round visits every connected slot once in cyclic order from   *)
(* the cursor; each later round visits exactly the slots that wrote a      *)
(* frame in the round before, one frame each, in the same cyclic order; a  *)
(* slot that writes nothing has ended its pass for the flush               *)
(* (rn_endpoint_begin_flush reopens it); the flush ends when a round       *)
(* writes nothing or the capacity is spent.  The cursor is the one a walk  *)
(* over every connected slot in every round would leave: past each slot    *)
(* visited, at a capacity cut the value held before the cut slot's visit.  *)
(*                                                                         *)
(* One visit is rn_message_connection_write to its return: the due test    *)
(* (rn_endpoint_next_packet_bytes), the budget's refusal decided before a  *)
(* frame while it may refuse (write first, rn_endpoint_write_may_be_       *)
(* refused), the frame start that takes one unit of the shared capacity    *)
(* (start_packet), the write, and the abort of a frame the write left      *)
(* empty, which does NOT give the unit back (no transport returns it).     *)
(* A visit that writes nothing ends the slot's pass.                       *)
(*                                                                         *)
(* Three kinds of connected slot:                                          *)
(*   healthy: WorkMax frames or fewer of real data each flush (the         *)
(*            environment picks how many).                                 *)
(*   Dark:    in probe hold (MESSAGES.md m9.3, the timeout off): U and a   *)
(*            later number H on the wire, H the FIFO head past its delay;  *)
(*            the walk skips H and resends U only, every Delay flushes.    *)
(*   Short:   one reliable packet due each flush, which its own send       *)
(*            budget may refuse (the environment picks).                   *)
(*                                                                         *)
(* The code: DueRule = "shared" (a held head stands for U: the due test    *)
(* asks the walk's hold) and Budget = "first" (the refusal is decided      *)
(* before a frame).  The reversed witnesses:                               *)
(*   DueRule = "head":     the v1.1.0 due test reads H alone: due every     *)
(*                         flush, the walk writes nothing, the frame is    *)
(*                         aborted and its unit lost                       *)
(*   Budget = "frame":     the v1.1.1 order: the frame first, the budget   *)
(*                         refuses the write, the frame is aborted         *)
(*   Mode = "fromzero":    every flush starts at slot 0                    *)
(*   Mode = "inplace":     the cursor stays on the slot just visited       *)
(*   Mode = "revisit":     the compaction keeps a slot that wrote nothing  *)
(*   Mode = "cutcursor":   a cut in a later round leaves the cursor where  *)
(*                         the last visit set it                           *)
(*   Mode = "reorder":     later rounds walk the list from its end         *)
(*                                                                         *)
(* What is claimed (MESSAGES.md m1.8): FairService, every slot with        *)
(* writable work is written within ceil(|Conn| / K) flushes; NoUnitWasted, *)
(* every unit charged went to a written frame; OtherClientUnaffected, no   *)
(* healthy slot ends a flush with work left while a unit of that flush     *)
(* went to an aborted frame; DarkTakesOnlyItsProbes; and the rounds:       *)
(* SameOrder, OnlyLastWriters, EndedWritesNothing, CursorAsFullWalk,       *)
(* VisitsBounded.  Writable work is defined from the slot's state, not     *)
(* from the due test.  The writer list is cleared at a flush's start: its  *)
(* entries are read only below count, each written earlier in that flush,  *)
(* so stale entries change no behaviour.                                   *)
(*                                                                         *)
(* Send queues: the slots in Q1 stage on a second send queue with its own  *)
(* capacity K1 a flush (K1 = 0: a queue full for good); the others on the  *)
(* first, with K.  Q1 = {} is the transport with one queue.  The capacity  *)
(* a visit tests is its own queue's (rn_message_send_admit, the transport's *)
(* send_capacity_to): a slot whose queue is spent while the other has room *)
(* is held (Hold): it writes nothing this flush, its pass ends without a   *)
(* visit's effects, and the round goes on; only both spent cuts the flush  *)
(* (Cut).  The cursor then names the cursor held before the first slot     *)
(* held after the flush's first frame, at a cut and at the end alike: the  *)
(* first slot left unserved.  QueueRule = "perqueue" is the code; the      *)
(* reversed witnesses: "mincut" cuts when either queue is spent (the code  *)
(* before queues were judged one by one: the transport's capacity was the *)
(* fewest of any queue), so one spent queue starves the other's slots;     *)
(* "holdfirst" holds also before the flush's first frame, which pins the  *)
(* cursor on a slot of a queue spent for good; "cutwalk" leaves the walk's *)
(* cursor at a cut in a later round, past the held slot, which then waits  *)
(* behind slots already served this flush; each fails FairService.        *)
(* FairService is owed per queue: ceil(|Conn on q| / Kq) flushes, to the   *)
(* slots of a queue with a capacity.                                       *)
(*                                                                         *)
(* Live room: the code reads the capacity live at each visit               *)
(* (rocketnet_server_transport_send_capacity_available): in-place frames   *)
(* started between flushes have spent some of the ring's (they start only  *)
(* while more than send_connected_count is left, rn_message_object_start), *)
(* a staged transport's send room is whatever its in-flight sends left,    *)
(* and that room can grow within the flush as sends complete.  room is the *)
(* first queue's capacity this flush: BeginFlush picks it in RoomMin..K    *)
(* and Grow adds one unit, once, inside the flush.  RoomMin = K (the       *)
(* default) is the fixed capacity; MCRNServerSendFairLive.cfg overrides it *)
(* with 0.  FairService's window is ceil(|Conn on q| / Kq) flushes that    *)
(* had the full capacity from their start: a flush that began with less   *)
(* room serves fewer slots and does not count, even when its room grew     *)
(* back inside it (CountsAllFlushes FALSE).  The reversed                  *)
(* witness MCRNServerSendFairBrokenFixedWindow.cfg counts every flush and  *)
(* fails FairService: the fixed-capacity window is not owed under a live   *)
(* room.                                                                   *)
(***************************************************************************)
EXTENDS Integers, FiniteSets

CONSTANTS N,          \* slots 0..N-1
          K,          \* frames the transport starts per flush (shared)
          Conn,       \* the connected slots
          WorkMax,    \* the most frames a healthy slot offers per flush
          Dark,       \* the slots held by probe pacing
          Short,      \* the slots whose own budget may refuse their packet
          Delay,      \* a dark slot's resend delay for U, in flushes
          DueRule,    \* "shared" (the code) or "head" (v1.1.0)
          Budget,     \* "first" (the code) or "frame" (v1.1.1)
          Mode,       \* "rounds" (the code), "fromzero", "inplace",
                      \* "revisit", "cutcursor", "reorder"
          Q1,         \* the slots on the second send queue ({}: one queue)
          K1,         \* frames the second queue starts per flush
          QueueRule   \* "perqueue" (the code), "mincut", "holdfirst", "cutwalk"

ASSUME N \in Nat \ {0}
ASSUME K \in 1..N
ASSUME Conn \subseteq 0..(N - 1)
ASSUME WorkMax \in Nat \ {0}
ASSUME Dark \subseteq Conn /\ Short \subseteq Conn /\ Dark \cap Short = {}
ASSUME Delay \in Nat \ {0}
ASSUME DueRule \in {"shared", "head"}
ASSUME Budget \in {"first", "frame"}
ASSUME Mode \in {"rounds", "fromzero", "inplace", "revisit", "cutcursor", "reorder"}
ASSUME Q1 \subseteq Conn /\ K1 \in 0..N
ASSUME QueueRule \in {"perqueue", "mincut", "holdfirst", "cutwalk"}

Slots == 0..(N - 1)
Entries == 0..(2 * N - 1)
Healthy == Conn \ (Dark \cup Short)

\* the send queue a slot stages on, and each queue's full capacity a flush
QOf(s) == IF s \in Q1 THEN 1 ELSE 0
Full(q) == IF q = 1 THEN K1 ELSE K
ConnOn(q) == {s \in Conn : QOf(s) = q}

\* the least room the first queue may have at a flush (K: a fixed capacity;
\* overridden in MCRNServerSendFairLive.cfg)
RoomMin == K

\* TRUE: FairService counts every flush, the fixed-capacity window
\* (overridden in the witness MCRNServerSendFairBrokenFixedWindow.cfg)
CountsAllFlushes == FALSE

\* each queue's fairness window: ceil(|Conn on q| / Kq); one queue: ceil(|Conn| / K)
WindowOn(q) == (Cardinality(ConnOn(q)) + Full(q) - 1) \div Full(q)
Window == IF Q1 = {} \/ K1 = 0 THEN WindowOn(0)
          ELSE IF WindowOn(0) > WindowOn(1) THEN WindowOn(0) ELSE WindowOn(1)

\* the slots FairService is owed to: every connected slot of a queue with a capacity
Owed == {s \in Conn : Full(QOf(s)) > 0}

VARIABLES \* the code's state
          pc,        \* "between" flushes, "first" round, "later" rounds
          snc,       \* send_next_connection
          start,     \* the loop's start
          off,       \* the first round's offset
          prev,      \* previous: the connected slot visited last in the first round, -1 before
          w,         \* send_writers[0 .. 2N-1]
          count,     \* the writer list's length
          k,         \* the later round's index into the list
          kept,      \* the compaction's length
          used,      \* units of the (first queue's) capacity charged this flush (frames started)
          usedB,     \* units of the second queue's capacity charged this flush
          held,      \* the cursor before the first slot held after the flush's first frame, -1 for none
          ended,     \* the slots whose pass ended this flush (wrote nothing in a visit)
          room,      \* the first queue's capacity this flush, read live
          grown,     \* the room grew inside this flush
          \* the slots' state (the environment's, per flush)
          work,      \* frames a healthy slot has left; 1 while a short slot's packet is due
          uAge,      \* a dark slot's flushes since U's last send, saturating at Delay
          pays,      \* a short slot's budget pays its packet this flush
          \* ghosts
          visits,    \* slot visits this flush (the development seam's slot_visits)
          wrotePrev, \* the slots that wrote a frame in the round before
          wroteNow,  \* the slots that wrote a frame in this round
          lastPos,   \* this round's last visited slot's cyclic position from start, -1 at a round's start
          ok,        \* every visit and cursor so far kept the round statements
          started,   \* frames started for each slot this flush, to 2
          wrote,     \* frames written for each slot this flush, to 2
          writable,  \* each slot had writable work when the flush began
          waiting,   \* flushes each slot had writable work and none written, to Window
          wasted,    \* units charged this flush to aborted frames
          harmed     \* a healthy slot ended a flush with work left while a unit went to an aborted frame

vars == <<pc, snc, start, off, prev, w, count, k, kept, used, usedB, held, ended, room, grown,
          work, uAge, pays, visits, wrotePrev, wroteNow, lastPos, ok, started, wrote, writable,
          waiting, wasted, harmed>>

Min(a, b) == IF a < b THEN a ELSE b

\* each queue's capacity as a visit reads it now
Cap(q) == IF q = 1 THEN K1 ELSE room

\* U is past its delay: the walk resends it (the one number the hold allows)
UDue(s) == uAge[s] >= Delay

\* what s can write now, from its own state
Writable(s) ==
    CASE s \in Dark  -> UDue(s)
      [] s \in Short -> work[s] > 0 /\ pays[s]
      [] OTHER       -> work[s] > 0

\* rn_endpoint_next_packet_bytes > 0.  A short slot's packet is due paid or
\* not (the due test does not ask the budget).  A dark slot: "shared" asks
\* U's delay; "head" reads H, on the wire past its delay: due every flush.
ReportedDue(s) ==
    CASE s \in Dark  -> IF DueRule = "shared" THEN UDue(s) ELSE TRUE
      [] OTHER       -> work[s] > 0

\* the next cursor after visiting s
NextC(s) == IF Mode = "inplace" THEN s ELSE (s + 1) % N

\* a slot's cyclic position from the loop's start
Pos(s) == (s - start + N) % N

\* the cursor a walk over every connected slot holds just before it visits
\* s in a round: past the connected slot before s in this round's order, or,
\* with none before it, the value the round began with
FullBefore(s, roundStart) ==
    LET before == { t \in Conn : Pos(t) < Pos(s) }
        lastBefore == CHOOSE t \in before : \A u \in before : Pos(u) <= Pos(t)
    IN IF before = {} THEN roundStart ELSE (lastBefore + 1) % N

RoundStart == IF pc = "first" THEN snc ELSE (prev + 1) % N

\* the queues' capacities as rn_message_send_admit reads them
UsedOf(q) == IF q = 1 THEN usedB ELSE used
UsedTotal == used + usedB
Spent(q) == UsedOf(q) >= Cap(q)
AllSpent == Spent(0) /\ (Q1 = {} \/ Spent(1))
\* Cut: no queue takes a frame (admit 0); "mincut" reads the fewest of any queue
CutAt == IF QueueRule = "mincut" THEN Spent(0) \/ (Q1 # {} /\ Spent(1)) ELSE AllSpent
\* Hold: slot i's own queue is spent while the other has room (admit -1)
HoldAt(i) == ~CutAt /\ Spent(QOf(i))

\* the cursor a flush's natural end leaves: the held one first, if any
Settle(h, c) == IF h >= 0 THEN h ELSE c

Init ==
    /\ pc = "between"
    /\ snc = 0 /\ start = 0 /\ off = 0 /\ prev = -1
    /\ w = [e \in Entries |-> 0]
    /\ count = 0 /\ k = 0 /\ kept = 0 /\ used = 0
    /\ usedB = 0 /\ held = -1
    /\ ended = {}
    /\ room = K /\ grown = FALSE
    /\ work = [s \in Slots |-> 0]
    /\ uAge = [s \in Slots |-> IF s \in Dark THEN Delay - 1 ELSE 0]
    /\ pays = [s \in Slots |-> FALSE]
    /\ visits = 0 /\ wrotePrev = {} /\ wroteNow = {} /\ lastPos = -1
    /\ ok = [order |-> TRUE, writers |-> TRUE, cut |-> TRUE, cost |-> TRUE]
    /\ started = [s \in Slots |-> 0]
    /\ wrote = [s \in Slots |-> 0]
    /\ writable = [s \in Slots |-> FALSE]
    /\ waiting = [s \in Slots |-> 0]
    /\ wasted = 0
    /\ harmed = FALSE

\* The flush begins: a dark slot's U is one flush older; the environment
\* offers each healthy slot its frames and decides each short slot's
\* budget; every pass reopens (rn_endpoint_begin_flush).
Aged == [s \in Slots |-> IF s \in Dark THEN Min(uAge[s] + 1, Delay) ELSE 0]
BeginFlush ==
    /\ pc = "between"
    /\ uAge' = Aged
    /\ \E wk \in [Healthy -> 0..WorkMax], p \in [Short -> BOOLEAN] :
          /\ work' = [s \in Slots |-> IF s \in Healthy THEN wk[s]
                                      ELSE IF s \in Short THEN 1 ELSE 0]
          /\ pays' = [s \in Slots |-> IF s \in Short THEN p[s] ELSE FALSE]
          /\ writable' = [s \in Slots |->
                CASE s \in Dark    -> Aged[s] >= Delay
                  [] s \in Short   -> p[s]
                  [] s \in Healthy -> wk[s] > 0
                  [] OTHER         -> FALSE]
    /\ pc' = "first"
    /\ start' = IF Mode = "fromzero" THEN 0 ELSE snc % N
    /\ off' = 0 /\ prev' = -1
    /\ w' = [e \in Entries |-> 0]
    /\ count' = 0 /\ used' = 0
    /\ usedB' = 0 /\ held' = -1
    /\ ended' = {}
    /\ room' \in RoomMin..K /\ grown' = FALSE
    /\ visits' = 0 /\ wrotePrev' = {} /\ wroteNow' = {} /\ lastPos' = -1
    /\ started' = [s \in Slots |-> 0]
    /\ wrote' = [s \in Slots |-> 0]
    /\ wasted' = 0
    /\ UNCHANGED <<snc, k, kept, ok, waiting, harmed>>

\* The flush ends (a capacity cut, or a round with no writer): the transport
\* sends and its count resets.  A slot left waiting counts the flush only
\* when its queue had the full capacity for the whole flush: from its start,
\* not grown to it after the walk passed the slot (CountsAllFlushes: every
\* flush).
Counts(s) == CountsAllFlushes \/ QOf(s) = 1 \/ (room = K /\ ~grown)
EndFlush(sncNext, okNext, wr, wk, ws) ==
    /\ pc' = "between"
    /\ snc' = sncNext
    /\ ok' = okNext
    /\ waiting' = [s \in Slots |->
          IF writable[s] /\ wr[s] = 0
          THEN IF Counts(s) THEN Min(waiting[s] + 1, Window) ELSE waiting[s]
          ELSE 0]
    /\ harmed' = (harmed \/ (ws > 0 /\ \E s \in Healthy : wk[s] > 0))

\* rn_message_connection_write for slot i, with capacity available: its
\* effect on the slot and the capacity, and whether it wrote a frame.
Due(i)     == i \notin ended /\ ReportedDue(i)
Refused(i) == Due(i) /\ i \in Short /\ Budget = "first" /\ ~pays[i]
Frame(i)   == Due(i) /\ ~Refused(i)
Wrote(i)   == Frame(i) /\ Writable(i)
Aborted(i) == Frame(i) /\ ~Writable(i)
Wr1(i)     == [wrote EXCEPT ![i] = IF Wrote(i) THEN Min(@ + 1, 2) ELSE @]
Wk1(i)     == [work EXCEPT ![i] = IF Wrote(i) /\ i \notin Dark THEN @ - 1 ELSE @]
Ws1(i)     == IF Aborted(i) THEN wasted + 1 ELSE wasted
\* the frames started this flush over both queues after the visit
Used1(i)   == IF Frame(i) THEN UsedTotal + 1 ELSE UsedTotal
WriteEffects(i) ==
    /\ used' = IF Frame(i) /\ QOf(i) = 0 THEN used + 1 ELSE used
    /\ usedB' = IF Frame(i) /\ QOf(i) = 1 THEN usedB + 1 ELSE usedB
    /\ started' = [started EXCEPT ![i] = IF Frame(i) THEN Min(@ + 1, 2) ELSE @]
    /\ wrote' = Wr1(i)
    /\ work' = Wk1(i)
    /\ uAge' = [uAge EXCEPT ![i] = IF Wrote(i) /\ i \in Dark THEN 0 ELSE @]
    /\ ended' = IF Wrote(i) THEN ended ELSE ended \cup {i}
    /\ wasted' = Ws1(i)

\* one offset of the first round
FirstVisit ==
    /\ pc = "first"
    /\ UNCHANGED <<room, grown>>
    /\ LET i == (start + off) % N
           isLast == off + 1 = N
       IN
       /\ visits' = visits + 1
       /\ IF i \notin Conn
          THEN \* not connected: continue (no capacity test, no cursor)
               /\ IF isLast
                  THEN IF count = 0
                       THEN /\ EndFlush(Settle(held, IF prev >= 0 THEN NextC(prev) ELSE snc), ok, wrote, work, wasted)
                            /\ UNCHANGED <<off, count, k, kept, wrotePrev, wroteNow, lastPos>>
                       ELSE /\ pc' = "later" /\ k' = 0 /\ kept' = 0
                            /\ wrotePrev' = wroteNow /\ wroteNow' = {} /\ lastPos' = -1
                            /\ UNCHANGED <<snc, ok, off, count, waiting, harmed>>
                  ELSE /\ off' = off + 1
                       /\ UNCHANGED <<pc, snc, ok, count, k, kept, wrotePrev, wroteNow, lastPos,
                                      waiting, harmed>>
               /\ UNCHANGED <<start, prev, w, used, usedB, held, ended, work, uAge, pays, started, wrote,
                              writable, wasted>>
          ELSE IF CutAt
          THEN \* Cut: no queue takes a frame: goto flush, the cursor as last
               \* set (the walk's), or the held one, which comes before it
               /\ EndFlush(Settle(held, snc), [ok EXCEPT !.cut = @ /\ snc = FullBefore(i, RoundStart),
                                                         !.cost = @ /\ visits + 1 <= N + UsedTotal],
                           wrote, work, wasted)
               /\ UNCHANGED <<start, off, prev, w, count, k, kept, used, usedB, held, ended, work, uAge, pays,
                              wrotePrev, wroteNow, lastPos, started, wrote, writable, wasted>>
          ELSE IF HoldAt(i)
          THEN \* Hold: i's own queue is spent, the other has room: i writes
               \* nothing this flush and the round goes on; the cursor before
               \* the first slot held after the flush's first frame is kept
               LET h1 == IF held < 0 /\ (count > 0 \/ QueueRule = "holdfirst") THEN snc ELSE held
                   ok1 == [ok EXCEPT !.order = @ /\ Pos(i) > lastPos,
                                     !.cost = @ /\ visits + 1 <= N + UsedTotal]
               IN
               /\ held' = h1
               /\ prev' = i
               /\ IF isLast
                  THEN IF count = 0
                       THEN /\ EndFlush(Settle(h1, NextC(i)), ok1, wrote, work, wasted)
                            /\ UNCHANGED <<off, k, kept, wrotePrev, wroteNow, lastPos>>
                       ELSE /\ pc' = "later" /\ k' = 0 /\ kept' = 0
                            /\ snc' = NextC(i) /\ ok' = ok1
                            /\ wrotePrev' = wroteNow /\ wroteNow' = {} /\ lastPos' = -1
                            /\ UNCHANGED <<off, waiting, harmed>>
                  ELSE /\ off' = off + 1
                       /\ snc' = NextC(i) /\ ok' = ok1
                       /\ lastPos' = Pos(i)
                       /\ UNCHANGED <<pc, k, kept, wrotePrev, wroteNow, waiting, harmed>>
               /\ UNCHANGED <<start, w, count, used, usedB, ended, work, uAge, pays, started, wrote,
                              writable, wasted>>
          ELSE LET wr == Wrote(i)
                   w1 == IF wr
                         THEN [w EXCEPT ![2 * count] = i,
                                        ![2 * count + 1] = IF prev < 0 THEN -1 ELSE NextC(prev)]
                         ELSE w
                   c1 == IF wr THEN count + 1 ELSE count
                   ok1 == [ok EXCEPT !.order = @ /\ Pos(i) > lastPos,
                                     !.cost = @ /\ visits + 1 <= N + Used1(i)]
               IN
               /\ WriteEffects(i)
               /\ w' = w1
               /\ prev' = i
               /\ IF isLast
                  THEN IF c1 = 0
                       THEN /\ EndFlush(Settle(held, NextC(i)), ok1, Wr1(i), Wk1(i), Ws1(i))
                            /\ count' = c1
                            /\ UNCHANGED <<off, k, kept, wrotePrev, wroteNow, lastPos>>
                       ELSE /\ pc' = "later" /\ k' = 0 /\ kept' = 0 /\ count' = c1
                            /\ snc' = NextC(i) /\ ok' = ok1
                            /\ wrotePrev' = (IF wr THEN wroteNow \cup {i} ELSE wroteNow)
                            /\ wroteNow' = {} /\ lastPos' = -1
                            /\ UNCHANGED <<off, waiting, harmed>>
                  ELSE /\ off' = off + 1 /\ count' = c1
                       /\ snc' = NextC(i) /\ ok' = ok1
                       /\ wroteNow' = (IF wr THEN wroteNow \cup {i} ELSE wroteNow)
                       /\ lastPos' = Pos(i)
                       /\ UNCHANGED <<pc, k, kept, wrotePrev, waiting, harmed>>
               /\ UNCHANGED <<start, pays, writable, held>>

\* one entry of a later round: the writer list, compacted in place
LaterVisit ==
    /\ pc = "later"
    /\ UNCHANGED <<room, grown>>
    /\ LET idx == IF Mode = "reorder" THEN count - 1 - k ELSE k
           i == w[2 * idx]
           stored == w[2 * idx + 1]
           cur == IF stored < 0 THEN NextC(prev) ELSE stored
           walk == IF Mode = "cutcursor" THEN snc ELSE cur
           isLast == k + 1 = count
           okv == [ok EXCEPT !.order = @ /\ Pos(i) > lastPos,
                             !.writers = @ /\ i \in wrotePrev /\ i \notin ended]
       IN
       /\ visits' = visits + 1
       /\ IF CutAt
          THEN \* Cut: goto flush with the stored cursor (the walk's), or the
               \* held one, the first slot left unserved this flush
               /\ EndFlush(IF QueueRule = "cutwalk" THEN walk ELSE Settle(held, walk),
                           [okv EXCEPT !.cut = @ /\ walk = FullBefore(i, RoundStart),
                                       !.cost = @ /\ visits + 1 <= N + UsedTotal],
                           wrote, work, wasted)
               /\ UNCHANGED <<start, off, prev, w, count, k, kept, used, usedB, held, ended, work, uAge, pays,
                              wrotePrev, wroteNow, lastPos, started, wrote, writable, wasted>>
          ELSE IF HoldAt(i)
          THEN \* Hold in a later round: served once this flush already, its
               \* queue is spent now: its pass ends, the cursor past it
               LET ok1 == [okv EXCEPT !.cost = @ /\ visits + 1 <= N + UsedTotal]
               IN
               /\ IF isLast
                  THEN IF kept = 0
                       THEN /\ EndFlush(Settle(held, NextC(prev)), ok1, wrote, work, wasted)
                            /\ count' = 0
                            /\ UNCHANGED <<k, kept, wrotePrev, wroteNow, lastPos>>
                       ELSE /\ count' = kept /\ k' = 0 /\ kept' = 0
                            /\ snc' = NextC(i) /\ ok' = ok1
                            /\ wrotePrev' = wroteNow /\ wroteNow' = {} /\ lastPos' = -1
                            /\ UNCHANGED <<pc, waiting, harmed>>
                  ELSE /\ k' = k + 1
                       /\ snc' = NextC(i) /\ ok' = ok1
                       /\ lastPos' = Pos(i)
                       /\ UNCHANGED <<pc, count, kept, wrotePrev, wroteNow, waiting, harmed>>
               /\ UNCHANGED <<start, off, prev, w, used, usedB, held, ended, work, uAge, pays,
                              started, wrote, writable, wasted>>
          ELSE LET wr == Wrote(i)
                   keep == wr \/ Mode = "revisit"
                   w1 == IF keep THEN [w EXCEPT ![2 * kept + 1] = stored, ![2 * kept] = i] ELSE w
                   kept1 == IF keep THEN kept + 1 ELSE kept
                   wn == IF wr THEN wroteNow \cup {i} ELSE wroteNow
                   ok1 == [okv EXCEPT !.cost = @ /\ visits + 1 <= N + Used1(i)]
               IN
               /\ WriteEffects(i)
               /\ w' = w1
               /\ IF isLast
                  THEN IF kept1 = 0
                       THEN /\ EndFlush(Settle(held, NextC(prev)), ok1, Wr1(i), Wk1(i), Ws1(i))
                            /\ count' = 0
                            /\ UNCHANGED <<k, kept, wrotePrev, wroteNow, lastPos>>
                       ELSE /\ count' = kept1 /\ k' = 0 /\ kept' = 0
                            /\ snc' = NextC(i) /\ ok' = ok1
                            /\ wrotePrev' = wn /\ wroteNow' = {} /\ lastPos' = -1
                            /\ UNCHANGED <<pc, waiting, harmed>>
                  ELSE /\ k' = k + 1 /\ kept' = kept1
                       /\ snc' = NextC(i) /\ ok' = ok1
                       /\ wroteNow' = wn /\ lastPos' = Pos(i)
                       /\ UNCHANGED <<pc, count, wrotePrev, waiting, harmed>>
               /\ UNCHANGED <<start, off, prev, pays, writable, held>>

\* the transport's room grows inside the flush (a staged send completes, its
\* slot retired): the next capacity test reads one more unit, once a flush
Grow ==
    /\ pc \in {"first", "later"}
    /\ room < K /\ ~grown
    /\ room' = room + 1 /\ grown' = TRUE
    /\ UNCHANGED <<pc, snc, start, off, prev, w, count, k, kept, used, usedB, held, ended,
                   work, uAge, pays, visits, wrotePrev, wroteNow, lastPos, ok, started, wrote,
                   writable, waiting, wasted, harmed>>

Next == BeginFlush \/ FirstVisit \/ LaterVisit \/ Grow

Spec == Init /\ [][Next]_vars

-----------------------------------------------------------------------------

TypeOK ==
    /\ pc \in {"between", "first", "later"}
    /\ snc \in Slots /\ start \in Slots /\ off \in 0..(N - 1)
    /\ prev \in -1..(N - 1)
    /\ w \in [Entries -> -1..(N - 1)]
    /\ count \in 0..N /\ kept \in 0..N /\ k \in 0..N
    /\ used \in 0..K /\ usedB \in 0..K1
    /\ held \in -1..(N - 1)
    /\ ended \subseteq Conn
    /\ room \in 0..K /\ grown \in BOOLEAN
    /\ work \in [Slots -> 0..WorkMax]
    /\ uAge \in [Slots -> 0..Delay]
    /\ waiting \in [Slots -> 0..Window]
    /\ wasted \in 0..(K + K1)

\* every slot with writable work at the start of a flush has a frame
\* written within ceil(|Conn on its queue| / its queue's capacity) flushes
\* that had the queue's full capacity (one queue: ceil(|Conn| / K)); a
\* slot of a queue with no capacity is owed nothing
FairService == \A s \in Owed : waiting[s] < WindowOn(QOf(s))

\* every unit of the shared capacity charged went to a written frame
NoUnitWasted == wasted = 0

\* (m1.8) a healthy slot never ends a flush with frames left while a unit
\* of that flush's capacity went to an aborted frame
OtherClientUnaffected == ~harmed

\* a dark slot starts a frame only when its probe resend is due, and at
\* most one a flush
DarkTakesOnlyItsProbes ==
    \A d \in Dark : started[d] <= (IF writable[d] THEN 1 ELSE 0)

\* every round visits its slots in cyclic order from the loop's start, each
\* at most once, the later rounds in the first's order
SameOrder == ok.order

\* a later round visits exactly the slots that wrote in the round before:
\* only those (never one whose pass ended), and every one of them
OnlyLastWriters ==
    /\ ok.writers
    /\ (pc = "later" /\ k = 0) => { w[2 * j] : j \in 0..(count - 1) } = wrotePrev

\* a slot whose pass ended has nothing left it could write this flush
EndedWritesNothing == \A s \in ended : ~Writable(s)

\* the cursor at a capacity cut is the one the walk over every connected
\* slot would hold
CursorAsFullWalk == ok.cut

\* a flush costs at most N visits plus one per frame started
VisitsBounded == ok.cost /\ visits <= N + UsedTotal
=============================================================================
