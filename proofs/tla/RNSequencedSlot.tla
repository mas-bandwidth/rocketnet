--------------------------- MODULE RNSequencedSlot ---------------------------
EXTENDS Integers, FiniteSets

\* One ordered channel, one two-fragment code-4 block O (numbers 1,2),
\* ordered predecessors Y (0) and Z (3), and newer code-4 state N (4).
\* Numbers 5..MaxNum are ordinary reliable records. Numbering follows the
\* channel FIFO; packet delivery and transport ACK are independently reordered.
\* Each number has its own packet here. ACK eligibility starts only after
\* EndUpdate has released every delivery view (P6). The receiver's received
\* set, live descriptors, future/in-turn holds, and ordered epoch are distinct.
\* sdk/rocketnet_channel.cpp:381-403: ordered enqueue advances ordered_write;
\* sdk/rocketnet_channel.cpp:1296-1313 stamps it before same-channel FIFO append.
\* sdk/rocketnet_channel.cpp:1673-1705 numbers that FIFO's head first.
\* Thus N#3 before Z has ordering 1; ordering 2 requires Z#3, then N#4.
CONSTANTS W, MaxNum, CheckAcceptedSpan, ForwardFragmentOrder
ASSUME /\ W \in {4, 8}
       /\ MaxNum \in 6..10
       /\ CheckAcceptedSpan \in BOOLEAN
       /\ ForwardFragmentOrder \in BOOLEAN

Nums == 0..MaxNum
VARIABLES next, acked, received, ackable, R, live, future, inTurn,
          orderedRead, delivered, collision
vars == <<next, acked, received, ackable, R, live, future, inTurn,
          orderedRead, delivered, collision>>

Unacked == (0..next-1) \ acked
U == IF Unacked = {} THEN next ELSE CHOOSE x \in Unacked : \A y \in Unacked : x <= y
UPrime == IF U \in {1,2} THEN 1 ELSE U
RECURSIVE Advance(_, _)
Advance(s, r) == IF r \in s THEN Advance(s, r+1) ELSE r

Init ==
  /\ next = 0 /\ acked = {} /\ received = {} /\ ackable = {}
  /\ R = 0 /\ live = {} /\ future = 0 /\ inTurn = 0
  /\ orderedRead = 0 /\ delivered = {} /\ collision = FALSE

\* The block's two numbers are reserved atomically, even if its fragments
\* are transmitted separately. All other messages take one number.
Number ==
  /\ next <= MaxNum
  /\ LET k == IF next = 1 THEN 2 ELSE 1
     IN /\ next + k <= UPrime + W
        /\ next' = next + k
  /\ UNCHANGED <<acked, received, ackable, R, live, future, inTurn,
                 orderedRead, delivered, collision>>

\* A packet accepted in the prior update can be acknowledged. P2 permits
\* ACK for stale application data; P6 forbids reuse before EndUpdate.
Ack(n) ==
  /\ n \in ackable \ acked
  /\ acked' = acked \cup {n}
  /\ UNCHANGED <<next, received, ackable, R, live, future, inTurn,
                 orderedRead, delivered, collision>>

EndUpdate ==
  /\ ackable' = received
  /\ live' = live \ delivered
  /\ delivered' = {}
  /\ UNCHANGED <<next, acked, received, R, future, inTurn,
                 orderedRead, collision>>

\* Common layer-3 acceptance, followed by the result of layer-4 dispatch.
\* No slot-availability guard appears: collision records an actual check-10
\* refusal. The invariant must derive its absence from the composed rules.
Accept(n, l, f, i, o, d) ==
  /\ n \in 0..next-1
  /\ n \notin received
  /\ n >= R /\ n < R + W
  /\ collision' = (collision \/ (\E x \in live : x # n /\ x % W = n % W))
  /\ received' = received \cup {n}
  /\ R' = Advance(received \cup {n}, R)
  /\ live' = l
  /\ future' = f /\ inTurn' = i /\ orderedRead' = o
  /\ delivered' = d
  /\ UNCHANGED <<next, acked, ackable>>

\* Y opens epoch 1. Drain promotes an incomplete O future hold into the
\* in-turn slot; it does not leave that hold available for N to supersede.
ReceiveY ==
  Accept(0,
    (live \cup {0}) \ (IF 3 \in received /\ future \in {1,2}
                            /\ ~({1,2} \subseteq received)
                         THEN {1,2} ELSE {}),
    IF 3 \in received \/ future \in {1,2} THEN 0 ELSE future,
    IF 3 \in received THEN 0
      ELSE IF future \in {1,2} /\ ~({1,2} \subseteq received)
        THEN future ELSE 0,
    IF 3 \in received THEN 2 ELSE 1,
    delivered \cup {0} \cup (IF 3 \in received THEN {3} ELSE {})
      \cup (IF 3 \in received /\ future = 4 THEN {4} ELSE {})
      \cup (IF future \in {1,2} /\ {1,2} \subseteq received
               THEN {1,2} ELSE {}))

\* Z opens epoch 2. Drain releases an incomplete in-turn O and drains N.
ReceiveZ ==
  /\ Accept(3,
       (live \cup {3}) \ (IF orderedRead = 1 /\ inTurn \in {1,2}
                               THEN {1,2} ELSE {}),
       IF orderedRead = 1 THEN 0 ELSE future,
       IF orderedRead = 1 THEN 0 ELSE inTurn,
       IF orderedRead = 1 THEN 2 ELSE 0,
       delivered \cup (IF orderedRead = 1 THEN {3} ELSE {})
         \cup (IF orderedRead = 1 /\ future = 4 THEN {4} ELSE {}))

\* O's first fragment can arrive ahead, in turn, or stale. A newer future N
\* keeps its own hold and makes an older O fragment stale.
ReceiveOFirst ==
  Accept(1,
    IF orderedRead > 1 \/ (future = 4 /\ orderedRead = 0)
       \/ (orderedRead = 1 /\ 2 \in received /\ 2 \notin live
             /\ CheckAcceptedSpan)
      THEN live ELSE live \cup {1},
    IF orderedRead = 0 /\ future = 0 THEN 1 ELSE future,
    IF orderedRead = 1 /\ inTurn = 2 THEN 0
      ELSE IF orderedRead = 1 /\ inTurn = 0
           /\ ~(2 \in received /\ 2 \notin live /\ CheckAcceptedSpan)
        THEN 1 ELSE inTurn,
    orderedRead,
    IF orderedRead = 1 /\ inTurn = 2 THEN delivered \cup {1,2}
      ELSE delivered)

\* A sibling accepted and released by supersede makes the late fragment
\* stale. The reversed setting anchors an orphan in-turn block instead.
Orphan == 1 \in received /\ 1 \notin live /\ inTurn = 0
ReceiveOLast ==
  /\ (~ForwardFragmentOrder \/ 1 \in received)
  /\ Accept(2,
    IF orderedRead > 1 \/ (orderedRead = 0 /\ future = 4)
       \/ (orderedRead = 1 /\ Orphan /\ CheckAcceptedSpan)
      THEN live ELSE live \cup {2},
    IF orderedRead = 0 /\ future = 0 THEN 2 ELSE future,
    IF orderedRead = 1 /\ inTurn \in {1,2} THEN 0
      ELSE IF orderedRead = 1 /\ inTurn = 0 /\ ~(Orphan /\ CheckAcceptedSpan)
        THEN 2 ELSE inTurn,
    orderedRead,
    IF orderedRead = 1 /\ inTurn = 1 THEN delivered \cup {1,2}
      ELSE delivered)

\* N supersedes the future O span, releasing its accepted descriptor. It
\* does not touch an O block already promoted to the in-turn slot.
ReceiveN ==
  Accept(4,
    (live \ (IF future \in {1,2} THEN {1,2} ELSE {})) \cup {4},
    IF orderedRead < 2 THEN 4 ELSE 0,
    inTurn, orderedRead,
    IF orderedRead = 2 THEN delivered \cup {4} ELSE delivered)

ReceivePlain(n) ==
  /\ n \in 5..MaxNum
  /\ Accept(n, live \cup {n}, future, inTurn, orderedRead,
             delivered \cup {n})

Next == Number \/ EndUpdate \/ ReceiveY \/ ReceiveZ \/ ReceiveOFirst
        \/ ReceiveOLast \/ ReceiveN
        \/ (\E n \in 5..MaxNum : ReceivePlain(n))
        \/ (\E n \in Nums : Ack(n))
Spec == Init /\ [][Next]_vars

NoSlotCollision ==
  ~collision /\ \A x,y \in live : x # y => x % W # y % W
TypeOK ==
  /\ next \in 0..MaxNum+1 /\ acked \subseteq 0..next-1
  /\ received \subseteq 0..next-1 /\ ackable \subseteq received
  /\ R \in 0..MaxNum+1 /\ live \subseteq received
  /\ future \in {0,1,2,4} /\ inTurn \in {0,1,2}
  /\ orderedRead \in 0..2 /\ delivered \subseteq live
  /\ collision \in BOOLEAN
=============================================================================
