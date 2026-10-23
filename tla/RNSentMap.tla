------------------------------ MODULE RNSentMap ------------------------------
(***************************************************************************)
(* The sender's sent-packet map against the reliable window, as            *)
(* sdk/rocketnet_window_ack.cpp keeps it (rn_window_ack_commit,            *)
(* rn_window_ack_on_ack, rn_window_ack_resend) and as                      *)
(* rocketnet_message_validate_config_ex (sdk/rocketnet_config.cpp) sizes   *)
(* it: P entries (packets_in_flight) against W numbers in flight           *)
(* (reliable_messages_in_flight).                                          *)
(*                                                                         *)
(* The question this module answers: what must P be, relative to W, for   *)
(* the map never to evict an entry whose ack is still on its way?  Such   *)
(* an eviction makes the sender ignore that ack (standard/MESSAGES.md      *)
(* m9.1), so the numbers the packet carried stay unacked and are resent,   *)
(* and a sender that keeps doing it can stall into the reliable timeout.   *)
(*                                                                         *)
(* The answer is P >= W, per packet against per number.  Every packet     *)
(* carries at least one reliable number.  With a fixed round trip RTT and  *)
(* a resend delay RTO >= RTT, a number is written at most once in any      *)
(* round trip, and every number written in the last round trip is still   *)
(* unacked (its ack has not had time to land) and inside the window, so   *)
(* at most W packets were written in the last round trip.  The map is a    *)
(* ring in send order that evicts its oldest entry when the P-th is        *)
(* written past it; if that oldest entry's ack were still on its way, all *)
(* P entries and the new packet would have been written inside one round  *)
(* trip: P + 1 <= W.  P >= W rules it out.  W packets of one number each,  *)
(* written in one update, show P >= W is also needed.                      *)
(*                                                                         *)
(* Time is ticks.  The transport is abstract: a written packet is lost or  *)
(* is opened, and an opened packet's ack lands exactly RTT ticks after it  *)
(* was written (the fixed round trip the bound assumes).  Each update      *)
(* takes the acks landing at its time before it writes (Write is enabled  *)
(* only when no ack is due), as the code's update runs before its send.   *)
(* A packet carries 1..MaxPerPacket numbers: first sends of next, next+1,  *)
(* ... while inside the window (n < U + W), and resends of numbers whose  *)
(* last send is at least RTO ago.  Blocks (U') and unreliable receipt      *)
(* records are not modelled: a block only narrows the window, and a        *)
(* receipt-only packet carries no reliable number (README-RNSentMap.md).  *)
(*                                                                         *)
(* The map is the code's ring: entries in send order, an acked entry       *)
(* left as a hole (live = FALSE) until it reaches the head, the head       *)
(* reclaimed past holes (rn_window_ack_reclaim_head), and a write into a   *)
(* ring spanning P entries evicting the head first.  liveEvicted is a      *)
(* ghost: set when an evicted entry's ack is still on its way.             *)
(*                                                                         *)
(* The reversed witnesses are the constants themselves:                    *)
(*   P = W - 1  MCRNSentMapBrokenSmallMap.cfg must fail NoLiveEviction;    *)
(*   RTO < RTT  MCRNSentMapBrokenEarlyResend.cfg must fail NoLiveEviction  *)
(*              with P = W: the bound needs the resend delay above the    *)
(*              round trip, which the code's 2 x srtt + 4 x deviation +    *)
(*              30 ms keeps (standard/MESSAGES.md m9.2).                   *)
(***************************************************************************)
EXTENDS Naturals, Sequences, FiniteSets

CONSTANTS
    W,              \* the window: reliable numbers in flight (reliable_messages_in_flight)
    P,              \* sent-packet map entries (packets_in_flight)
    RTT,            \* ticks from a write to its ack landing
    RTO,            \* ticks from a number's last send to its resend
    MaxNum,         \* numbers are 0..MaxNum
    MaxTime,        \* the clock runs 0..MaxTime
    MaxPerPacket    \* reliable numbers a packet may carry, >= 1

ASSUME /\ W \in Nat \ {0}
       /\ P \in Nat \ {0}
       /\ RTT \in Nat \ {0}
       /\ RTO \in Nat \ {0}
       /\ MaxNum \in Nat
       /\ MaxTime \in Nat
       /\ MaxPerPacket \in Nat \ {0}

Nums == 0..MaxNum
Never == -1

VARIABLES
    now,            \* the update's time
    next,           \* the next number to give out
    acked,          \* the numbers marked acked
    lastSend,       \* each number's last send, Never before its first
    smap,           \* the ring: Seq of [id, nums, live], oldest first
    wire,           \* opened packets whose acks are on their way: [id, land]
    pid,            \* the next packet id; every write takes a fresh one
    liveEvicted     \* ghost: an entry was evicted while its ack was on its way

vars == <<now, next, acked, lastSend, smap, wire, pid, liveEvicted>>

Min(S) == CHOOSE m \in S : \A x \in S : m <= x

\* U: the oldest unacked number, or next when every number given out is acked
Unacked == {n \in 0..(next - 1) : n \notin acked}
U == IF Unacked = {} THEN next ELSE Min(Unacked)

\* a number given out, unacked, and last sent at least RTO ago
Due == {n \in Unacked : now >= lastSend[n] + RTO}

AcksDue == {q \in wire : q.land = now}

OnWire(id) == \E q \in wire : q.id = id

\* the ring with the holes at its head dropped (rn_window_ack_reclaim_head)
Reclaim(s) ==
    LET live == {i \in 1..Len(s) : s[i].live}
    IN IF live = {} THEN <<>> ELSE SubSeq(s, Min(live), Len(s))

Init ==
    /\ now = 0
    /\ next = 0
    /\ acked = {}
    /\ lastSend = [n \in Nums |-> Never]
    /\ smap = <<>>
    /\ wire = {}
    /\ pid = 0
    /\ liveEvicted = FALSE

\* A packet is written: k first sends from next, inside the window, and any
\* due resends R, 1..MaxPerPacket numbers in all.  The ring reclaims its
\* head, evicts the oldest entry if it spans P, and records the packet
\* (rn_window_ack_commit).  The packet is lost, or opened and its ack lands
\* RTT ticks later.
Write(k, R, lost) ==
    LET Fresh == next..(next + k - 1)
        S == Fresh \cup R
        r == Reclaim(smap)
        full == Len(r) >= P
        r2 == IF full THEN Reclaim(Tail(r)) ELSE r
    IN /\ AcksDue = {}
       /\ next + k - 1 < U + W
       /\ next + k - 1 <= MaxNum
       /\ R \subseteq Due
       /\ Cardinality(S) \in 1..MaxPerPacket
       /\ next' = next + k
       /\ lastSend' = [n \in Nums |-> IF n \in S THEN now ELSE lastSend[n]]
       /\ smap' = Append(r2, [id |-> pid, nums |-> S, live |-> TRUE])
       /\ liveEvicted' = (liveEvicted \/ (full /\ OnWire(r[1].id)))
       /\ wire' = IF lost THEN wire ELSE wire \cup {[id |-> pid, land |-> now + RTT]}
       /\ pid' = pid + 1
       /\ UNCHANGED <<now, acked>>

WriteAny ==
    \E k \in 0..MaxPerPacket, R \in SUBSET Due, lost \in BOOLEAN : Write(k, R, lost)

\* An ack lands (rn_window_ack_on_ack): an id the ring still holds marks its
\* numbers acked and leaves a hole; an id the ring no longer holds is ignored.
Ack(q) ==
    /\ q \in AcksDue
    /\ wire' = wire \ {q}
    /\ LET hit == {i \in 1..Len(smap) : smap[i].id = q.id /\ smap[i].live}
       IN IF hit = {}
            THEN UNCHANGED <<acked, smap>>
            ELSE LET i == CHOOSE i \in hit : TRUE
                 IN /\ acked' = acked \cup smap[i].nums
                    /\ smap' = Reclaim([smap EXCEPT ![i].live = FALSE])
    /\ UNCHANGED <<now, next, lastSend, pid, liveEvicted>>

AckAny == \E q \in wire : Ack(q)

\* the next update, once this one has taken its acks
Tick ==
    /\ AcksDue = {}
    /\ now < MaxTime
    /\ now' = now + 1
    /\ UNCHANGED <<next, acked, lastSend, smap, wire, pid, liveEvicted>>

\* the bounded run's end: the clock stops and nothing is due
Done ==
    /\ AcksDue = {}
    /\ now = MaxTime
    /\ UNCHANGED vars

Next == WriteAny \/ AckAny \/ Tick \/ Done

Spec == Init /\ [][Next]_vars

(***************************************************************************)
(* Safety.                                                                 *)
(***************************************************************************)
TypeOK ==
    /\ now \in 0..MaxTime
    /\ next \in 0..MaxNum + 1
    /\ acked \subseteq Nums
    /\ lastSend \in [Nums -> {Never} \cup 0..MaxTime]
    /\ smap \in Seq([id : Nat, nums : SUBSET Nums, live : BOOLEAN])
    /\ \A q \in wire : q.land \in now..(now + RTT)
    /\ pid \in Nat
    /\ liveEvicted \in BOOLEAN

\* the ring never spans more than P entries
MapBounded == Len(smap) <= P

\* the window: never more than W numbers between U and next
WindowBounded == next - U <= W

\* no entry is evicted while its ack is on its way
NoLiveEviction == ~liveEvicted
=============================================================================
