------------------------ MODULE RNDisconnectBurstRing ------------------------
(***************************************************************************)
(* The server transport's ring of deferred disconnect bursts, as laid out  *)
(* in sdk/rocketnet_server_transport.cpp, with the code's own arithmetic.  *)
(* ServerSession.tla counts the waiting seals per session; this model      *)
(* carries the ring those counts live in.                                  *)
(*                                                                         *)
(* The ring: disconnect_bursts[C]. C is the ring capacity                  *)
(* rocketnet_server_disconnect_bursts(max_clients), which is               *)
(* 2 * max_clients, not ROCKETNET_MAX_CLIENTS. Each slot holds its sealed  *)
(* count, with disconnect_burst_head, disconnect_burst_count and           *)
(* disconnect_burst_cursor, the offset from the head of the entry the      *)
(* current pass visits next. A burst is K packets                          *)
(* (ROCKETNET_SERVER_NUM_DISCONNECT_PACKETS). A running server keeps at    *)
(* most max_clients bursts waiting (SteadyCap) and the stop appends one    *)
(* for every connected client, at most max_clients more, so C is           *)
(* 2 * SteadyCap here as in the code. The code has no test of the ring's   *)
(* capacity on any path (the assert in                                     *)
(* rocketnet_server_transport_defer_disconnect_burst is compiled out of a  *)
(* release build); NoOverwrite holds because of the steady cap and the     *)
(* one stop, which this model carries.                                     *)
(*                                                                         *)
(*   Connect   a client connects into a free slot while the server runs:   *)
(*             at most SteadyCap (max_clients) are connected               *)
(*   Defer     rocketnet_server_transport_disconnect_client_impl on a      *)
(*             running server with a burst waiting or an allowance short   *)
(*             of K: while count < SteadyCap                               *)
(*             (disconnect_burst_steady_cap),                              *)
(*             rocketnet_server_transport_defer_disconnect_burst writes    *)
(*             slot (head + count) % C and counts it; otherwise the burst  *)
(*             is dropped and the slot closed locally. Either way the      *)
(*             slot ends. (The immediate burst, sealed whole with no burst *)
(*             waiting, never touches the ring; letting every disconnect   *)
(*             defer only adds behaviours.)                                *)
(*   Stop      rocketnet_server_stop: every connected client is            *)
(*             disconnected with shutdown set                              *)
(*             (rocketnet_server_transport_disconnect_client_shutdown),    *)
(*             which defers its burst with no room test, then the          *)
(*             transport is STOPPING. It runs once: no client connects     *)
(*             and no running disconnect is made after it                  *)
(*   SealStep  one seal in                                                 *)
(*             rocketnet_server_transport_drain_disconnect_bursts: the     *)
(*             cursor restarts at 0 once it reaches count, the incomplete  *)
(*             burst at (head + cursor) % C seals its next packet, the     *)
(*             cursor moves on, and every completed burst at the head      *)
(*             leaves (head moves modulo C, count falls, the cursor drops  *)
(*             with it)                                                    *)
(*   Abandon   the same iteration when the start is refused for its        *)
(*             destination with room left: the burst is marked complete    *)
(*             (sealed = K) and no packet of it is sealed, then the head   *)
(*             pop runs                                                     *)
(*   PassOver  the same iteration when the entry is already complete: the  *)
(*             cursor moves on and the entry is not sealed. A complete     *)
(*             entry away from the head (one abandoned) leaves only once   *)
(*             the pop at the head reaches it                               *)
(*   DropForRoom  rocketnet_server_transport_stop_step (only once         *)
(*             stopping) with one send queue, when the transport           *)
(*             has no send room and is idle (ServerStopSpread AbandonNoRoom):*)
(*             every waiting burst leaves from the head (keys wiped, head  *)
(*             moved on by count modulo C, count 0) and the cursor is      *)
(*             reset to 0; each burst not yet complete is dropped unsent   *)
(*             in part or whole                                            *)
(*                                                                         *)
(* With several send queues (Queues > 1) the code has two more steps:      *)
(*   PassFull  the drain's iteration when the incomplete burst at the      *)
(*             cursor has a full queue while another queue has room: the   *)
(*             cursor moves on and nothing is sealed or popped             *)
(*   AbandonQueue  rocketnet_server_transport_stop_step (only once         *)
(*             stopping) with several queues, the transport idle: every    *)
(*             incomplete waiting burst whose own queue is full (here any  *)
(*             nonempty set of them) is marked complete wherever it sits,  *)
(*             then the complete bursts at the head leave, the cursor      *)
(*             dropping with each; DropForRoom is not this code's step     *)
(* A burst passed for its full queue keeps its seals while bursts behind   *)
(* it are sealed, so with several queues a pass can end with it unsealed   *)
(* (PassFair fails) and sealed counts can rise from the head to the tail   *)
(* (SealedFallFromHead fails).  What the code keeps there: no burst sealed *)
(* twice in a pass, a pass ends with every waiting burst sealed unless it  *)
(* passed one over for its full queue (PassFairUnlessFull), the head is    *)
(* never left complete (HeadNotComplete), and the rest below.              *)
(* ServerStopSpread.tla carries the queues' room itself.                   *)
(*                                                                         *)
(* A drain ends when its allowance is spent or a send frame is refused     *)
(* for a reason other than the destination; either leaves the ring and the *)
(* cursor as they are, so the next update's drain continues the pass. Any  *)
(* interleaving of Defer, SealStep, Abandon, PassOver and PassFull is      *)
(* therefore a run of updates. The ghosts name each burst by the order it  *)
(* was deferred and count the seals it actually received.                  *)
(*                                                                         *)
(* Reversed witnesses (each flag FALSE is a real bug class):               *)
(*   SteadyCapExact the steady cap one late (count <= SteadyCap)           *)
(*   CursorWrapAt   the cursor restarts only past count (cursor > count)   *)
(*   CursorDrops    the cursor is not lowered when the head pops           *)
(*   DropResetsCursor  the drop for room leaves the cursor where it was    *)
(*   AbandonQueuePops  the several-queue stop step marks bursts complete   *)
(*                  and does not pop the head                              *)
(*                                                                         *)
(* A burst deferred during a pass joins it at the tail: the bursts ahead   *)
(* of it may have a redundant packet sealed before its first.  The pass is *)
(* the unit of fairness (PassFair), not the first packet of every burst.   *)
(* An abandoned burst is complete, so the pass does not owe it a seal.     *)
(***************************************************************************)
EXTENDS Integers, FiniteSets

CONSTANTS
    SteadyCap,          \* disconnect_burst_steady_cap = max_clients
    K,                  \* ROCKETNET_SERVER_NUM_DISCONNECT_PACKETS
    Bursts,             \* how many disconnects the instance makes
    SteadyCapExact,     \* a running disconnect defers only at count < SteadyCap
    CursorWrapAt,       \* drain restarts the cursor at cursor >= count
    CursorDrops,        \* drain lowers the cursor when the head pops
    DropResetsCursor,   \* the stop step's drop for room resets the cursor to 0
    Queues,             \* send_queues: 1, or more (PassFull, AbandonQueue)
    AbandonQueuePops    \* the several-queue stop step pops the complete head

ASSUME SteadyCap \in Nat \ {0} /\ K \in Nat \ {0} /\ Bursts \in Nat /\ Queues \in Nat \ {0}
ASSUME \A f \in {SteadyCapExact, CursorWrapAt, CursorDrops, DropResetsCursor, AbandonQueuePops} : f \in BOOLEAN

\* rocketnet_server_disconnect_bursts(max_clients) = 2 * max_clients
C == 2 * SteadyCap

Ring == 0..(C - 1)
NOBURST == 0                    \* ghost id of a slot no live burst holds

VARIABLES
    sealed,         \* sealed[s]: disconnect_bursts[s].sealed
    bid,            \* ghost: bid[s], the burst whose record slot s last received
    head,           \* disconnect_burst_head
    count,          \* disconnect_burst_count
    cursor,         \* disconnect_burst_cursor
    next,           \* ghost: the next burst's id (1-based)
    seals,          \* ghost: seals[b], the packets actually sealed for burst b
    gone,           \* ghost: the bursts that left the ring
    abandoned,      \* ghost: bursts marked complete because the destination was refused
    dropped,        \* ghost: bursts dropped before complete because the transport had no room
    pass,           \* ghost: the bursts sealed in the current pass
    bad,            \* ghost: the first rule a step broke, "" while none
    conn,           \* the connected clients (rocketnet_client_slots_connected)
    stopping,       \* rocketnet_server_stop has run (the transport is STOPPING)
    passedFull,     \* ghost: the bursts the current pass passed over for their full queue
    fullSkip        \* ghost: a pass ended owing a seal after passing one over for its full queue

vars == <<sealed, bid, head, count, cursor, next, seals, gone, abandoned, dropped, pass, bad, conn, stopping, passedFull, fullSkip>>

Ids == 1..Bursts
\* bursts the pass still owes a seal: a complete (abandoned) entry is not one
Need == { b \in Ids : \E p \in 0..(count - 1) : bid[(head + p) % C] = b /\ sealed[(head + p) % C] < K }
At(p) == (head + p) % C

\* where this iteration of the drain looks: the cursor, wrapped at count
Place ==
    LET wrap == IF CursorWrapAt THEN cursor >= count ELSE cursor > count
        cur == IF wrap THEN 0 ELSE cursor
        s == (head + cur) % C
    IN [wrap |-> wrap, cur |-> cur, s |-> s, b |-> bid[s], live |-> cur < count]

Init ==
    /\ sealed = [ s \in Ring |-> 0 ]
    /\ bid = [ s \in Ring |-> NOBURST ]
    /\ head = 0
    /\ count = 0
    /\ cursor = 0
    /\ next = 1
    /\ seals = [ b \in Ids |-> 0 ]
    /\ gone = {}
    /\ abandoned = {}
    /\ dropped = {}
    /\ pass = {}
    /\ bad = ""
    /\ conn = 0
    /\ stopping = FALSE
    /\ passedFull = {}
    /\ fullSkip = FALSE

\* a client connects while the server runs; every connected client will
\* make one disconnect, so the instance keeps an id for each
Connect ==
    /\ ~stopping
    /\ conn < SteadyCap
    /\ next + conn <= Bursts
    /\ conn' = conn + 1
    /\ UNCHANGED <<sealed, bid, head, count, cursor, next, seals, gone, abandoned, dropped, pass, bad, stopping, passedFull, fullSkip>>

\* a running disconnect whose burst must wait: deferred while fewer than
\* SteadyCap wait, else dropped. There is no test against C: an append at
\* count >= C lands on the waiting head, which the ghost bad records.
SteadyHasRoom == IF SteadyCapExact THEN count < SteadyCap ELSE count <= SteadyCap
Defer ==
    /\ ~stopping
    /\ conn > 0
    /\ conn' = conn - 1
    /\ next' = next + 1
    /\ IF SteadyHasRoom
       THEN LET s == (head + count) % C
            IN /\ sealed' = [ sealed EXCEPT ![s] = 0 ]
               /\ bid' = [ bid EXCEPT ![s] = next ]
               /\ count' = count + 1
               /\ bad' = IF bad = "" /\ count >= C THEN "overwrite" ELSE bad
       ELSE UNCHANGED <<sealed, bid, count, bad>>
    /\ UNCHANGED <<head, cursor, seals, gone, abandoned, dropped, pass, stopping, passedFull, fullSkip>>

\* rocketnet_server_stop: each connected client's burst appended in turn at
\* (head + count) % C, with no room test (the shutdown path of
\* disconnect_client_impl). An append at count >= C overwrites.
RECURSIVE StopAppend(_, _, _, _, _, _)
StopAppend(ns, nb, c, id, n, over) ==
    IF n = 0 THEN [ s |-> ns, b |-> nb, c |-> c, over |-> over ]
    ELSE LET x == (head + c) % C
         IN StopAppend([ ns EXCEPT ![x] = 0 ], [ nb EXCEPT ![x] = id ], c + 1, id + 1, n - 1, over \/ c >= C)
Stop ==
    /\ ~stopping
    /\ stopping' = TRUE
    /\ conn' = 0
    /\ next' = next + conn
    /\ LET r == StopAppend(sealed, bid, count, next, conn, FALSE)
       IN /\ sealed' = r.s
          /\ bid' = r.b
          /\ count' = r.c
          /\ bad' = IF bad = "" /\ r.over THEN "overwrite" ELSE bad
    /\ UNCHANGED <<head, cursor, seals, gone, abandoned, dropped, pass, passedFull, fullSkip>>

\* the pop loop at the end of a seal or an abandon: each burst at the head
\* with sealed = K leaves; the cursor drops with each
RECURSIVE Pop(_, _, _, _, _)
Pop(ns, h, c, cur, out) ==
    IF c > 0 /\ ns[h] = K
    THEN Pop(ns, (h + 1) % C, c - 1, IF CursorDrops /\ cur > 0 THEN cur - 1 ELSE cur, out \cup {bid[h]})
    ELSE [ h |-> h, c |-> c, cur |-> cur, out |-> out ]

\* a new pass starts at this iteration while the pass ending owes a seal
Skips(p) == p.wrap /\ ~(Need \subseteq pass)

\* the pass ending owed a seal: a "skipped" pass unless every burst it owes
\* was passed over for its full queue, which only fullSkip records
SkipBad(p) == p.wrap /\ ~(Need \subseteq pass \cup passedFull)
NextFullSkip(p) == fullSkip \/ (Skips(p) /\ ~SkipBad(p))

\* one seal of the burst at the cursor. A complete entry is PassOver, not this.
SealStep ==
    /\ count > 0
    /\ LET p == Place
           s == p.s
           b == p.b
           ns == [ sealed EXCEPT ![s] = @ + 1 ]
       IN /\ sealed[s] < K
          /\ sealed' = ns
          /\ seals' = IF b \in Ids THEN [ seals EXCEPT ![b] = @ + 1 ] ELSE seals
          /\ LET r == Pop(ns, head, count, p.cur + 1, {})
             IN /\ head' = r.h
                /\ count' = r.c
                /\ cursor' = r.cur
                /\ gone' = gone \cup r.out
          /\ pass' = IF p.wrap THEN {b} ELSE pass \cup {b}
          /\ bad' = IF bad # "" THEN bad
                    ELSE IF ~p.live THEN "dead"
                    ELSE IF SkipBad(p) THEN "skipped"
                    ELSE IF ~p.wrap /\ b \in pass THEN "twice"
                    ELSE ""
          /\ fullSkip' = NextFullSkip(p)
          /\ passedFull' = IF p.wrap THEN {} ELSE passedFull
    /\ UNCHANGED <<bid, next, abandoned, dropped, conn, stopping>>

\* rocketnet_server_transport_drain_disconnect_bursts: the destination was
\* refused with room left. The burst is marked complete and no packet is
\* sealed. The head pop then runs, so an abandoned head leaves at once and
\* one abandoned behind the head leaves when it reaches the head.
Abandon ==
    /\ count > 0
    /\ LET p == Place
           s == p.s
           b == p.b
           ns == [ sealed EXCEPT ![s] = K ]
       IN /\ sealed[s] < K
          /\ sealed' = ns
          /\ abandoned' = abandoned \cup {b}
          /\ LET r == Pop(ns, head, count, p.cur + 1, {})
             IN /\ head' = r.h
                /\ count' = r.c
                /\ cursor' = r.cur
                /\ gone' = gone \cup r.out
          /\ pass' = IF p.wrap THEN {} ELSE pass
          /\ bad' = IF bad # "" THEN bad
                    ELSE IF ~p.live THEN "dead"
                    ELSE IF SkipBad(p) THEN "skipped"
                    ELSE ""
          /\ fullSkip' = NextFullSkip(p)
          /\ passedFull' = IF p.wrap THEN {} ELSE passedFull
    /\ UNCHANGED <<bid, next, seals, dropped, conn, stopping>>

\* a complete entry is not sealed again: the cursor moves on, and the pop
\* does not run until a later seal or abandon. The head is never complete
\* (HeadNotComplete), so this is never the first iteration of a pass.
PassOver ==
    /\ count > 0
    /\ LET p == Place
       IN /\ sealed[p.s] = K
          /\ cursor' = p.cur + 1
          /\ UNCHANGED <<sealed, bid, head, count, next, seals, gone, abandoned, dropped, pass, bad, conn, stopping, passedFull, fullSkip>>

\* rocketnet_server_transport_drain_disconnect_bursts with several send
\* queues: the incomplete burst at the cursor has a full queue while another
\* queue has room (the room itself is ServerStopSpread's; here either may
\* happen). The cursor moves on; nothing is sealed and the pop does not run.
\* It can be the first iteration of a pass (the head's queue full), so it
\* starts the new pass as a seal does.
PassFull ==
    /\ Queues > 1
    /\ count > 0
    /\ LET p == Place
       IN /\ p.live
          /\ sealed[p.s] < K
          /\ cursor' = p.cur + 1
          /\ pass' = IF p.wrap THEN {} ELSE pass
          /\ bad' = IF bad # "" THEN bad
                    ELSE IF SkipBad(p) THEN "skipped"
                    ELSE ""
          /\ fullSkip' = NextFullSkip(p)
          /\ passedFull' = IF p.wrap THEN {p.b} ELSE passedFull \cup {p.b}
          /\ UNCHANGED <<sealed, bid, head, count, next, seals, gone, abandoned, dropped, conn, stopping>>

\* rocketnet_server_transport_stop_step with several send queues, the
\* transport idle: each incomplete waiting burst whose queue is full (any
\* nonempty set of them here) is marked complete where it sits and counted,
\* then the complete bursts at the head leave, the cursor dropping with each.
\* AbandonQueuePops = FALSE leaves out that pop loop.
AbandonQueue ==
    /\ Queues > 1
    /\ stopping
    /\ count > 0
    /\ \E F \in SUBSET { q \in 0..(count - 1) : sealed[At(q)] < K } :
          /\ F # {}
          /\ LET ns == [ s \in Ring |-> IF \E q \in F : At(q) = s THEN K ELSE sealed[s] ]
                 r == Pop(ns, head, count, cursor, {})
             IN /\ sealed' = ns
                /\ abandoned' = abandoned \cup { bid[At(q)] : q \in F }
                /\ IF AbandonQueuePops
                   THEN /\ head' = r.h
                        /\ count' = r.c
                        /\ cursor' = r.cur
                        /\ gone' = gone \cup r.out
                   ELSE UNCHANGED <<head, count, cursor, gone>>
    /\ UNCHANGED <<bid, next, seals, dropped, pass, bad, conn, stopping, passedFull, fullSkip>>

\* rocketnet_server_transport_stop_step with no send room and the transport
\* idle (ServerStopSpread AbandonNoRoom): the loop pops every waiting burst
\* from the head, wiping its key (the sealed counts stay, as the code leaves
\* them), so head moves on by count modulo C and count is 0; then the cursor
\* is reset. Each burst not yet complete is dropped; one already complete
\* (abandoned behind the head) leaves as it would have. The stop step
\* returns at once unless the transport is STOPPING. With several send
\* queues the stop step takes AbandonQueue instead.
DropForRoom ==
    /\ Queues = 1
    /\ stopping
    /\ count > 0
    /\ LET live == { bid[At(p)] : p \in 0..(count - 1) }
           short == { bid[At(p)] : p \in { q \in 0..(count - 1) : sealed[At(q)] < K } }
       IN /\ head' = (head + count) % C
          /\ count' = 0
          /\ cursor' = IF DropResetsCursor THEN 0 ELSE cursor
          /\ gone' = gone \cup live
          /\ dropped' = dropped \cup short
          /\ pass' = {}
          /\ passedFull' = {}
    /\ UNCHANGED <<sealed, bid, next, seals, abandoned, bad, conn, stopping, fullSkip>>

Next == Connect \/ Defer \/ Stop \/ SealStep \/ Abandon \/ PassOver \/ PassFull \/ AbandonQueue \/ DropForRoom

Spec == Init /\ [][Next]_vars

----------------------------------------------------------------------------

TypeOK ==
    /\ sealed \in [ Ring -> Nat ]
    /\ head \in Ring
    /\ count \in 0..C
    /\ cursor \in Nat
    /\ next \in 1..(Bursts + 1)
    /\ abandoned \subseteq 0..Bursts
    /\ dropped \subseteq Ids
    /\ conn \in 0..SteadyCap
    /\ stopping \in BOOLEAN
    /\ passedFull \subseteq 0..Bursts
    /\ fullSkip \in BOOLEAN

\* a deferred burst never overwrites a waiting one
NoOverwrite == bad # "overwrite"

\* a running server keeps at most SteadyCap bursts waiting, so the stop's
\* appends (at most SteadyCap) fit the other half of the ring
SteadyWithinCap == ~stopping => count <= SteadyCap

\* a seal is made only for a waiting burst, never for a slot past the tail
SealOnlyLive == bad # "dead"

\* one packet per burst still waiting per pass: no burst is sealed twice in
\* a pass, and a pass ends only when every burst that is not yet complete
\* was sealed in it. An abandoned burst is complete, so it is not owed a
\* seal and passing over it is not a second seal. One send queue only: with
\* several, a burst passed over for its full queue can end a pass unsealed
\* (MCRNDisconnectBurstRingQueuesPassFair.cfg).
PassFair == bad \notin {"skipped", "twice"} /\ ~fullSkip

\* what the code keeps with any number of queues: no burst sealed twice in a
\* pass, and a pass ends owing a seal only to bursts it passed over for
\* their full queue (passedFull), never to one whose queue had room
PassFairUnlessFull == bad \notin {"skipped", "twice"}

\* the head is never left complete: every step that completes a burst pops
\* the complete bursts at the head
HeadNotComplete == count = 0 \/ sealed[head] < K

\* among bursts still waiting, sealed counts never rise from the head to
\* the tail. A live entry already at K was abandoned and sits behind the
\* head; the head itself is never left complete. One send queue only: with
\* several, a burst passed for its full queue falls behind the bursts after
\* it (MCRNDisconnectBurstRingQueuesSealedFall.cfg).
SealedFallFromHead ==
    /\ HeadNotComplete
    /\ \A p, q \in 0..(count - 1) :
          (p < q /\ sealed[At(p)] < K /\ sealed[At(q)] < K)
              => sealed[At(p)] >= sealed[At(q)]

\* the cursor names a waiting burst or the end of the pass
CursorInRange == cursor <= count

\* a waiting burst that was not abandoned holds its own seals, fewer than K
\* with one send queue. With several, a burst sealed whole can wait behind a
\* head passed over for its full queue, since only the head pops
\* (sdk/rocketnet_server_transport.cpp, the drain's pop loop), so it holds
\* exactly K. An abandoned one is marked complete and was sent fewer than K.
\* A burst that left ended exactly one way: sealed whole (exactly K sent),
\* abandoned for its destination, or dropped for room (each fewer than K
\* sent).
ExactSeals ==
    /\ \A p \in 0..(count - 1) : LET b == bid[At(p)] IN
          /\ b \in Ids
          /\ b \notin dropped
          /\ IF b \in abandoned
             THEN sealed[At(p)] = K /\ seals[b] < K
             ELSE /\ sealed[At(p)] = seals[b]
                  /\ (seals[b] < K \/ (Queues > 1 /\ seals[b] = K))
    /\ \A b \in gone : b \in Ids
          /\ ~(b \in abandoned /\ b \in dropped)
          /\ IF b \in abandoned \/ b \in dropped THEN seals[b] < K ELSE seals[b] = K
    /\ dropped \subseteq gone

=============================================================================
