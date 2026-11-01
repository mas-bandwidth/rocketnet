------------------------ MODULE RNDisconnectBurstRing ------------------------
(***************************************************************************)
(* The server transport's ring of deferred disconnect bursts, as laid out  *)
(* in sdk/rocketnet_server_transport.cpp, with the code's own arithmetic.  *)
(* ServerSession.tla counts the waiting seals per session; this model      *)
(* carries the ring those counts live in.                                  *)
(*                                                                         *)
(* The ring: disconnect_bursts[C] (C = ROCKETNET_MAX_CLIENTS), each with   *)
(* its sealed count, and disconnect_burst_head, disconnect_burst_count     *)
(* and disconnect_burst_cursor, the offset from the head of the burst the  *)
(* current pass seals next.  A burst is K sealed packets                   *)
(* (ROCKETNET_SERVER_NUM_DISCONNECT_PACKETS).                              *)
(*                                                                         *)
(*   Defer     rocketnet_server_transport_disconnect_client_impl with a    *)
(*             burst waiting or an allowance short of K: with room,        *)
(*             rocketnet_server_transport_defer_disconnect_burst writes    *)
(*             slot (head + count) % C and counts it; with the ring full   *)
(*             the burst is dropped and the slot closed locally            *)
(*   SealStep  one iteration of                                            *)
(*             rocketnet_server_transport_drain_disconnect_bursts: the     *)
(*             cursor restarts at 0 once it reaches count, the burst at    *)
(*             (head + cursor) % C seals its next packet, the cursor moves *)
(*             on, and every completed burst at the head leaves (head      *)
(*             moves modulo C, count falls, the cursor drops with it)      *)
(*                                                                         *)
(* A drain ends when its allowance is spent or a send frame is refused;    *)
(* either leaves the ring and the cursor as they are, so the next update's *)
(* drain continues the pass.  Any interleaving of Defer and SealStep is    *)
(* therefore a run of updates.  The ghosts name each burst by the order it *)
(* was deferred and count the seals it received.                           *)
(*                                                                         *)
(* Reversed witnesses (each flag FALSE is a real bug class):               *)
(*   FullTestExact  the room test one late (count <= C)                    *)
(*   CursorWrapAt   the cursor restarts only past count (cursor > count)   *)
(*   CursorDrops    the cursor is not lowered when the head pops           *)
(*                                                                         *)
(* A burst deferred during a pass joins it at the tail: the bursts ahead   *)
(* of it may have a redundant packet sealed before its first.  The pass is *)
(* the unit of fairness (PassFair), not the first packet of every burst.   *)
(***************************************************************************)
EXTENDS Integers, FiniteSets

CONSTANTS
    C,                  \* ROCKETNET_MAX_CLIENTS: the ring's slots
    K,                  \* ROCKETNET_SERVER_NUM_DISCONNECT_PACKETS
    Bursts,             \* how many disconnects the instance makes
    FullTestExact,      \* disconnect_client_impl defers only at count < C
    CursorWrapAt,       \* drain restarts the cursor at cursor >= count
    CursorDrops         \* drain lowers the cursor when the head pops

ASSUME C \in Nat \ {0} /\ K \in Nat \ {0} /\ Bursts \in Nat
ASSUME \A f \in {FullTestExact, CursorWrapAt, CursorDrops} : f \in BOOLEAN

Ring == 0..(C - 1)
NOBURST == 0                    \* ghost id of a slot no live burst holds

VARIABLES
    sealed,         \* sealed[s]: disconnect_bursts[s].sealed
    bid,            \* ghost: bid[s], the burst whose record slot s last received
    head,           \* disconnect_burst_head
    count,          \* disconnect_burst_count
    cursor,         \* disconnect_burst_cursor
    next,           \* ghost: the next burst's id (1-based)
    seals,          \* ghost: seals[b], the packets sealed for burst b
    gone,           \* ghost: the bursts that left the ring
    pass,           \* ghost: the bursts sealed in the current pass
    bad             \* ghost: the first rule a step broke, "" while none

vars == <<sealed, bid, head, count, cursor, next, seals, gone, pass, bad>>

Ids == 1..Bursts
Live == { bid[(head + p) % C] : p \in 0..(count - 1) }
At(p) == (head + p) % C

Init ==
    /\ sealed = [ s \in Ring |-> 0 ]
    /\ bid = [ s \in Ring |-> NOBURST ]
    /\ head = 0
    /\ count = 0
    /\ cursor = 0
    /\ next = 1
    /\ seals = [ b \in Ids |-> 0 ]
    /\ gone = {}
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
    /\ UNCHANGED <<head, cursor, seals, gone, pass>>

\* the pop loop at the end of a seal: each burst at the head with all K
\* packets sealed leaves; the cursor drops with each
RECURSIVE Pop(_, _, _, _, _)
Pop(ns, h, c, cur, out) ==
    IF c > 0 /\ ns[h] = K
    THEN Pop(ns, (h + 1) % C, c - 1, IF CursorDrops /\ cur > 0 THEN cur - 1 ELSE cur, out \cup {bid[h]})
    ELSE [ h |-> h, c |-> c, cur |-> cur, out |-> out ]

\* one iteration of the drain's loop
SealStep ==
    /\ count > 0
    /\ LET wrap == IF CursorWrapAt THEN cursor >= count ELSE cursor > count
           cur == IF wrap THEN 0 ELSE cursor
           s == (head + cur) % C
           b == bid[s]
           live == cur < count
           ns == [ sealed EXCEPT ![s] = @ + 1 ]
       IN /\ sealed' = ns
          /\ seals' = IF b \in Ids THEN [ seals EXCEPT ![b] = @ + 1 ] ELSE seals
          /\ LET r == Pop(ns, head, count, cur + 1, {})
             IN /\ head' = r.h
                /\ count' = r.c
                /\ cursor' = r.cur
                /\ gone' = gone \cup r.out
          /\ pass' = IF wrap THEN {b} ELSE pass \cup {b}
          /\ bad' = IF bad # "" THEN bad
                    ELSE IF ~live THEN "dead"
                    ELSE IF wrap /\ ~(Live \subseteq pass) THEN "skipped"
                    ELSE IF ~wrap /\ b \in pass THEN "twice"
                    ELSE ""
    /\ UNCHANGED <<bid, next>>

Next == Defer \/ SealStep

Spec == Init /\ [][Next]_vars

----------------------------------------------------------------------------

TypeOK ==
    /\ sealed \in [ Ring -> Nat ]
    /\ head \in Ring
    /\ count \in 0..C
    /\ cursor \in Nat
    /\ next \in 1..(Bursts + 1)

\* a deferred burst never overwrites a waiting one
NoOverwrite == bad # "overwrite"

\* a seal is made only for a waiting burst, never for a slot past the tail
SealOnlyLive == bad # "dead"

\* one packet per waiting burst per pass: no burst is sealed twice in a
\* pass, and a pass ends only when every waiting burst was sealed in it
\* (so the bursts waiting when a pass starts all have their first packet
\* sealed before any of them its second)
PassFair == bad \notin {"skipped", "twice"}

\* sealed counts never rise from the head to the tail, so the head's burst
\* completes first and the pop at the head frees every completed burst
SealedFallFromHead ==
    \A p, q \in 0..(count - 1) : p < q => sealed[At(p)] >= sealed[At(q)]

\* the cursor names a waiting burst or the end of the pass
CursorInRange == cursor <= count

\* each waiting burst's record holds its own seals, fewer than K; each burst
\* that left had exactly K
ExactSeals ==
    /\ \A p \in 0..(count - 1) : LET b == bid[At(p)] IN
          b \in Ids /\ sealed[At(p)] = seals[b] /\ seals[b] < K
    /\ \A b \in gone : b \in Ids /\ seals[b] = K

=============================================================================
