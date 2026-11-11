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
(* most max_clients bursts waiting; ServerStopSpread models that cap and   *)
(* the stop filling the other half. This model checks the ring at          *)
(* capacity C.                                                             *)
(*                                                                         *)
(*   Defer     rocketnet_server_transport_disconnect_client_impl with a    *)
(*             burst waiting or an allowance short of K: with room,        *)
(*             rocketnet_server_transport_defer_disconnect_burst writes    *)
(*             slot (head + count) % C and counts it; with the ring full   *)
(*             the burst is dropped and the slot closed locally            *)
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
(*   DropForRoom  rocketnet_server_transport_stop_step when the transport  *)
(*             has no send room and is idle (ServerStopSpread AbandonNoRoom):*)
(*             every waiting burst leaves from the head (keys wiped, head  *)
(*             moved on by count modulo C, count 0) and the cursor is      *)
(*             reset to 0; each burst not yet complete is dropped unsent   *)
(*             in part or whole                                            *)
(*                                                                         *)
(* A drain ends when its allowance is spent or a send frame is refused     *)
(* for a reason other than the destination; either leaves the ring and the *)
(* cursor as they are, so the next update's drain continues the pass. Any  *)
(* interleaving of Defer, SealStep, Abandon and PassOver is therefore a    *)
(* run of updates. The ghosts name each burst by the order it was deferred *)
(* and count the seals it actually received.                               *)
(*                                                                         *)
(* Reversed witnesses (each flag FALSE is a real bug class):               *)
(*   FullTestExact  the room test one late (count <= C)                    *)
(*   CursorWrapAt   the cursor restarts only past count (cursor > count)   *)
(*   CursorDrops    the cursor is not lowered when the head pops           *)
(*   DropResetsCursor  the drop for room leaves the cursor where it was    *)
(*                                                                         *)
(* A burst deferred during a pass joins it at the tail: the bursts ahead   *)
(* of it may have a redundant packet sealed before its first.  The pass is *)
(* the unit of fairness (PassFair), not the first packet of every burst.   *)
(* An abandoned burst is complete, so the pass does not owe it a seal.     *)
(***************************************************************************)
EXTENDS Integers, FiniteSets

CONSTANTS
    C,                  \* rocketnet_server_disconnect_bursts(max_clients) = 2 * max_clients
    K,                  \* ROCKETNET_SERVER_NUM_DISCONNECT_PACKETS
    Bursts,             \* how many disconnects the instance makes
    FullTestExact,      \* disconnect_client_impl defers only at count < C
    CursorWrapAt,       \* drain restarts the cursor at cursor >= count
    CursorDrops,        \* drain lowers the cursor when the head pops
    DropResetsCursor    \* the stop step's drop for room resets the cursor to 0

ASSUME C \in Nat \ {0} /\ K \in Nat \ {0} /\ Bursts \in Nat
ASSUME \A f \in {FullTestExact, CursorWrapAt, CursorDrops, DropResetsCursor} : f \in BOOLEAN

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
    bad             \* ghost: the first rule a step broke, "" while none

vars == <<sealed, bid, head, count, cursor, next, seals, gone, abandoned, dropped, pass, bad>>

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

\* a disconnect whose burst must wait (or the ring is full and it is dropped)
Defer ==
    /\ next <= Bursts
    /\ next' = next + 1
    /\ IF IF FullTestExact THEN count < C ELSE count <= C
       THEN LET s == (head + count) % C
            IN /\ sealed' = [ sealed EXCEPT ![s] = 0 ]
               /\ bid' = [ bid EXCEPT ![s] = next ]
               /\ count' = count + 1
               /\ bad' = IF bad = "" /\ count >= C THEN "overwrite" ELSE bad
       ELSE UNCHANGED <<sealed, bid, count, bad>>
    /\ UNCHANGED <<head, cursor, seals, gone, abandoned, dropped, pass>>

\* the pop loop at the end of a seal or an abandon: each burst at the head
\* with sealed = K leaves; the cursor drops with each
RECURSIVE Pop(_, _, _, _, _)
Pop(ns, h, c, cur, out) ==
    IF c > 0 /\ ns[h] = K
    THEN Pop(ns, (h + 1) % C, c - 1, IF CursorDrops /\ cur > 0 THEN cur - 1 ELSE cur, out \cup {bid[h]})
    ELSE [ h |-> h, c |-> c, cur |-> cur, out |-> out ]

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
                    ELSE IF p.wrap /\ ~(Need \subseteq pass) THEN "skipped"
                    ELSE IF ~p.wrap /\ b \in pass THEN "twice"
                    ELSE ""
    /\ UNCHANGED <<bid, next, abandoned, dropped>>

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
                    ELSE IF p.wrap /\ ~(Need \subseteq pass) THEN "skipped"
                    ELSE ""
    /\ UNCHANGED <<bid, next, seals, dropped>>

\* a complete entry is not sealed again: the cursor moves on, and the pop
\* does not run until a later seal or abandon.
PassOver ==
    /\ count > 0
    /\ LET p == Place
       IN /\ sealed[p.s] = K
          /\ cursor' = p.cur + 1
          /\ UNCHANGED <<sealed, bid, head, count, next, seals, gone, abandoned, dropped, pass, bad>>

\* rocketnet_server_transport_stop_step with no send room and the transport
\* idle (ServerStopSpread AbandonNoRoom): the loop pops every waiting burst
\* from the head, wiping its key (the sealed counts stay, as the code leaves
\* them), so head moves on by count modulo C and count is 0; then the cursor
\* is reset. Each burst not yet complete is dropped; one already complete
\* (abandoned behind the head) leaves as it would have.
DropForRoom ==
    /\ count > 0
    /\ LET live == { bid[At(p)] : p \in 0..(count - 1) }
           short == { bid[At(p)] : p \in { q \in 0..(count - 1) : sealed[At(q)] < K } }
       IN /\ head' = (head + count) % C
          /\ count' = 0
          /\ cursor' = IF DropResetsCursor THEN 0 ELSE cursor
          /\ gone' = gone \cup live
          /\ dropped' = dropped \cup short
          /\ pass' = {}
    /\ UNCHANGED <<sealed, bid, next, seals, abandoned, bad>>

Next == Defer \/ SealStep \/ Abandon \/ PassOver \/ DropForRoom

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

\* a deferred burst never overwrites a waiting one
NoOverwrite == bad # "overwrite"

\* a seal is made only for a waiting burst, never for a slot past the tail
SealOnlyLive == bad # "dead"

\* one packet per burst still waiting per pass: no burst is sealed twice in
\* a pass, and a pass ends only when every burst that is not yet complete
\* was sealed in it. An abandoned burst is complete, so it is not owed a
\* seal and passing over it is not a second seal.
PassFair == bad \notin {"skipped", "twice"}

\* among bursts still waiting, sealed counts never rise from the head to
\* the tail. A live entry already at K was abandoned and sits behind the
\* head; the head itself is never left complete.
SealedFallFromHead ==
    /\ count = 0 \/ sealed[head] < K
    /\ \A p, q \in 0..(count - 1) :
          (p < q /\ sealed[At(p)] < K /\ sealed[At(q)] < K)
              => sealed[At(p)] >= sealed[At(q)]

\* the cursor names a waiting burst or the end of the pass
CursorInRange == cursor <= count

\* a waiting burst that was not abandoned holds its own seals, fewer than K.
\* An abandoned one is marked complete and was sent fewer than K. A burst
\* that left ended exactly one way: sealed whole (exactly K sent), abandoned
\* for its destination, or dropped for room (each fewer than K sent).
ExactSeals ==
    /\ \A p \in 0..(count - 1) : LET b == bid[At(p)] IN
          /\ b \in Ids
          /\ b \notin dropped
          /\ IF b \in abandoned
             THEN sealed[At(p)] = K /\ seals[b] < K
             ELSE sealed[At(p)] = seals[b] /\ seals[b] < K
    /\ \A b \in gone : b \in Ids
          /\ ~(b \in abandoned /\ b \in dropped)
          /\ IF b \in abandoned \/ b \in dropped THEN seals[b] < K ELSE seals[b] = K
    /\ dropped \subseteq gone

=============================================================================
