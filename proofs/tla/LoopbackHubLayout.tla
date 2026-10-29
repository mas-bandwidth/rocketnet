------------------------- MODULE LoopbackHubLayout -------------------------
(***************************************************************************)
(* The memory layout of the loopback hub's queues in                      *)
(* sdk/rocketnet_driver_loopback.cpp, with the code's arithmetic.  The    *)
(* hub is one block: the server's queue of ServerCapacity datagram slots, *)
(* then each client's queue of Capacity slots (rocketnet_loopback_hub_    *)
(* create lays them out in that order).  Every queue is a ring with head, *)
(* tail and count; the slot of free-running index i is i mod its          *)
(* capacity, at its queue's base in the block.  The indices are unsigned  *)
(* counters that wrap at CounterMod (2^32 in the code).  Send writes the  *)
(* slot at tail when count is below capacity, and a full queue drops the  *)
(* datagram; Receive takes the slot at head; when a queue empties both    *)
(* indices restart at zero.                                                *)
(*                                                                         *)
(* The abstract contract is one FIFO sequence per endpoint (queue).       *)
(* RingIsQueue states that the cells hold exactly that sequence: every    *)
(* unread datagram is in its own slot, in order, unoverwritten.  An       *)
(* abstract FIFO cannot see a slot written twice; this model can.         *)
(*                                                                         *)
(* The code's capacities are powers of two and it indexes with            *)
(* i & (capacity - 1), which is i mod capacity for them, and 2^32 is a    *)
(* multiple of every power of two, so the slot sequence stays contiguous  *)
(* across the counter wrap.  The control checks capacity 3 with           *)
(* CounterMod 6, a multiple of 3, the same property at the small size.    *)
(* Reversed witnesses: FullSlack = 1 is the off-by-one at capacity (the   *)
(* full test count > capacity), and CounterMod = 8 at capacity 3 is a     *)
(* wrap that is not a multiple of the capacity, a capacity that is not a  *)
(* power of two under the code's 2^32 wrap.  Both violate RingIsQueue.    *)
(*                                                                         *)
(* The code cites Send and Receive where they happen.  Done is the        *)
(* terminal stutter once every datagram is sent and every queue is empty. *)
(***************************************************************************)
EXTENDS Naturals, Sequences

CONSTANTS
    Clients,            \* client queues after the server's
    Capacity,           \* slots in a client queue
    ServerCapacity,     \* slots in the server queue
    CounterMod,         \* the unsigned index counters wrap here (2^32 in the code)
    MaxSent,            \* exploration bound: datagrams the harness sends
    FullSlack           \* 0 is the code's full test; 1 is the off-by-one at capacity

VARIABLES
    head, tail, count,  \* each queue's ring indices
    cells,              \* the block's datagram slots: a datagram id, or 0 when free
    queue,              \* each queue's contract: its unread datagrams in order
    nextId              \* the next datagram id

vars == <<head, tail, count, cells, queue, nextId>>

Queues == 0..Clients        \* 0 is the server's queue
Cap(q) == IF q = 0 THEN ServerCapacity ELSE Capacity
Base(q) == IF q = 0 THEN 0 ELSE ServerCapacity + (q - 1) * Capacity
Cells == 0..ServerCapacity + Clients * Capacity - 1

\* the cell of free-running index i in queue q
Addr(q, i) == Base(q) + (i % Cap(q))

Init ==
    /\ head = [ q \in Queues |-> 0 ]
    /\ tail = [ q \in Queues |-> 0 ]
    /\ count = [ q \in Queues |-> 0 ]
    /\ cells = [ c \in Cells |-> 0 ]
    /\ queue = [ q \in Queues |-> <<>> ]
    /\ nextId = 1

\* loopback_queue_tail and the send that fills it: write at tail when the
\* queue has room, else drop and count
Send(q) ==
    /\ nextId <= MaxSent
    /\ nextId' = nextId + 1
    /\ IF count[q] < Cap(q) + FullSlack
         THEN /\ cells' = [ cells EXCEPT ![Addr(q, tail[q])] = nextId ]
              /\ tail' = [ tail EXCEPT ![q] = (@ + 1) % CounterMod ]
              /\ count' = [ count EXCEPT ![q] = @ + 1 ]
              /\ queue' = [ queue EXCEPT ![q] = Append(@, nextId) ]
              /\ UNCHANGED head
         ELSE UNCHANGED <<head, tail, count, cells, queue>>

\* loopback_queue_pop after a receive or an oversize drop: take the slot at
\* head; an emptied queue restarts both indices at zero
Receive(q) ==
    /\ count[q] > 0
    /\ cells' = [ cells EXCEPT ![Addr(q, head[q])] = 0 ]
    /\ queue' = [ queue EXCEPT ![q] = Tail(@) ]
    /\ count' = [ count EXCEPT ![q] = @ - 1 ]
    /\ head' = [ head EXCEPT ![q] = IF count[q] = 1 THEN 0 ELSE (@ + 1) % CounterMod ]
    /\ tail' = [ tail EXCEPT ![q] = IF count[q] = 1 THEN 0 ELSE @ ]
    /\ UNCHANGED nextId

Done ==
    /\ nextId > MaxSent
    /\ \A q \in Queues : count[q] = 0
    /\ UNCHANGED vars

Next ==
    \/ \E q \in Queues : Send(q)
    \/ \E q \in Queues : Receive(q)
    \/ Done

Spec == Init /\ [][Next]_vars

TypeOK ==
    /\ head \in [ Queues -> 0..CounterMod-1 ]
    /\ tail \in [ Queues -> 0..CounterMod-1 ]
    /\ count \in [ Queues -> 0..ServerCapacity + Capacity + 1 ]
    /\ cells \in [ Cells -> 0..MaxSent ]
    /\ nextId \in 1..MaxSent+1

\* every queue's cells hold exactly its unread datagrams, in order: no
\* unread datagram is overwritten, and receive reads the oldest
RingIsQueue ==
    \A q \in Queues :
        /\ count[q] = Len(queue[q])
        /\ \A k \in 1..Len(queue[q]) :
             cells[Addr(q, (head[q] + k - 1) % CounterMod)] = queue[q][k]

\* a queue never holds more than its capacity
WithinCapacity ==
    \A q \in Queues : count[q] <= Cap(q)

\* every index of a queue maps inside its own region of the block
StaysInRegion ==
    \A q \in Queues : \A i \in 0..CounterMod-1 :
        Addr(q, i) \in Base(q)..Base(q) + Cap(q) - 1

\* a cell that holds a datagram holds one of its queue's unread datagrams
NoStrayCell ==
    \A c \in Cells : cells[c] # 0 =>
        \E q \in Queues : /\ c \in Base(q)..Base(q) + Cap(q) - 1
                          /\ \E k \in 1..Len(queue[q]) : queue[q][k] = cells[c]

\* an empty queue has restarted both indices
EmptyIsRestarted ==
    \A q \in Queues : count[q] = 0 => head[q] = 0 /\ tail[q] = 0

=============================================================================
