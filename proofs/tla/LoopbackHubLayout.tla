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
(* datagram; Receive takes the slot at head and leaves its bytes there    *)
(* (loopback_queue_pop never clears a slot); when a queue empties both    *)
(* indices restart at zero.  Reset is an endpoint's record handed back    *)
(* and taken again (loopback_destroy, then loopback_create): head, tail   *)
(* and count go to zero whatever is queued, and the queued datagrams are  *)
(* discarded with their bytes left in the cells.                          *)
(*                                                                         *)
(* The abstract contract is one FIFO sequence per endpoint (queue).       *)
(* RingIsQueue states that the cells hold exactly that sequence: every    *)
(* unread datagram is in its own slot, in order, unoverwritten, so a read *)
(* never returns a stale cell.  An abstract FIFO cannot see a slot written *)
(* twice or a stale cell read; this model can.                            *)
(*                                                                         *)
(* The code's capacities are powers of two and it indexes with            *)
(* i & (capacity - 1), which is i mod capacity for them, and 2^32 is a    *)
(* multiple of every power of two, so the slot sequence stays contiguous  *)
(* across the counter wrap.  The control checks capacity 3 with           *)
(* CounterMod 6, a multiple of 3, the same property at the small size.    *)
(*                                                                         *)
(* The block's bytes.  rocketnet_loopback_hub_create walks the queues,    *)
(* each taking loopback_carve(bytes) = the bytes plus the red zone        *)
(* (ROCKETNET_RED_ZONE_BYTES, poisoned in ASan builds) rounded up to the   *)
(* alignment, and rocketnet_loopback_hub_bytes sizes the block with the    *)
(* same carves, the server's plus Clients times a client's.  StaysInRegion *)
(* checks that arithmetic, relative to the first queue: every slot of     *)
(* every index lies in its queue's carve before the red zone, the red     *)
(* zone lies in the carve, the carves are disjoint and the block holds    *)
(* them all.                                                               *)
(*                                                                         *)
(* Reversed witnesses: FullSlack = 1 is the off-by-one at capacity (the   *)
(* full test count > capacity), and CounterMod = 8 at capacity 3 is a     *)
(* wrap that is not a multiple of the capacity, a capacity that is not a  *)
(* power of two under the code's 2^32 wrap; both violate RingIsQueue.     *)
(* ResetRestartsIndices = FALSE zeroes the count but leaves head and tail, *)
(* so the next send writes at tail and the receive reads a stale cell at   *)
(* head (RingIsQueue).  RedZoneInCarve = FALSE carves the bytes without    *)
(* the red zone, which then lies on the next queue (StaysInRegion).        *)
(*                                                                         *)
(* The code cites Send, Receive and Reset where they happen.  Done is the *)
(* terminal stutter once every datagram is sent and every queue is empty. *)
(***************************************************************************)
EXTENDS Naturals, Sequences

CONSTANTS
    Clients,            \* client queues after the server's
    Capacity,           \* slots in a client queue
    ServerCapacity,     \* slots in the server queue
    CounterMod,         \* the unsigned index counters wrap here (2^32 in the code)
    MaxSent,            \* exploration bound: datagrams the harness sends
    FullSlack,          \* 0 is the code's full test; 1 is the off-by-one at capacity
    ResetRestartsIndices, \* TRUE is the code: a reset zeroes head and tail with count
    SlotBytes,          \* bytes of one datagram slot
    RedZoneBytes,       \* ROCKETNET_RED_ZONE_BYTES
    AlignBytes,         \* ROCKETNET_ALLOCATION_ALIGNMENT
    RedZoneInCarve      \* TRUE is the code: a carve holds the bytes and the red zone

VARIABLES
    head, tail, count,  \* each queue's ring indices
    cells,              \* the block's datagram slots: a datagram id, stale after a read; 0 if never written
    queue,              \* each queue's contract: its unread datagrams in order
    nextId              \* the next datagram id

vars == <<head, tail, count, cells, queue, nextId>>

Queues == 0..Clients        \* 0 is the server's queue
Cap(q) == IF q = 0 THEN ServerCapacity ELSE Capacity
Base(q) == IF q = 0 THEN 0 ELSE ServerCapacity + (q - 1) * Capacity
Cells == 0..ServerCapacity + Clients * Capacity - 1

\* the cell of free-running index i in queue q
Addr(q, i) == Base(q) + (i % Cap(q))

\* loopback_align and loopback_carve
Align(b) == ((b + AlignBytes - 1) \div AlignBytes) * AlignBytes
Carve(b) == IF RedZoneInCarve THEN Align(b + RedZoneBytes) ELSE Align(b)

\* create's walk: queue q's first byte, after the carves of the queues before it
RECURSIVE ByteBase(_)
ByteBase(q) == IF q = 0 THEN 0 ELSE ByteBase(q - 1) + Carve(Cap(q - 1) * SlotBytes)

\* rocketnet_loopback_hub_bytes' queue part: the server's carve and Clients client carves
BlockBytes == Carve(ServerCapacity * SlotBytes) + Clients * Carve(Capacity * SlotBytes)

CarveOf(q) == ByteBase(q) .. ByteBase(q) + Carve(Cap(q) * SlotBytes) - 1
RedZone(q) == ByteBase(q) + Cap(q) * SlotBytes .. ByteBase(q) + Cap(q) * SlotBytes + RedZoneBytes - 1
SlotSpan(q, i) == ByteBase(q) + (i % Cap(q)) * SlotBytes .. ByteBase(q) + (i % Cap(q)) * SlotBytes + SlotBytes - 1

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
\* head and leave its bytes; an emptied queue restarts both indices at zero
Receive(q) ==
    /\ count[q] > 0
    /\ queue' = [ queue EXCEPT ![q] = Tail(@) ]
    /\ count' = [ count EXCEPT ![q] = @ - 1 ]
    /\ head' = [ head EXCEPT ![q] = IF count[q] = 1 THEN 0 ELSE (@ + 1) % CounterMod ]
    /\ tail' = [ tail EXCEPT ![q] = IF count[q] = 1 THEN 0 ELSE @ ]
    /\ UNCHANGED <<cells, nextId>>

\* loopback_destroy and loopback_create on queue q's record: count, head
\* and tail to zero whatever is queued; the queued datagrams are discarded
\* and their bytes stay in the cells
Reset(q) ==
    /\ count' = [ count EXCEPT ![q] = 0 ]
    /\ head' = IF ResetRestartsIndices THEN [ head EXCEPT ![q] = 0 ] ELSE head
    /\ tail' = IF ResetRestartsIndices THEN [ tail EXCEPT ![q] = 0 ] ELSE tail
    /\ queue' = [ queue EXCEPT ![q] = <<>> ]
    /\ UNCHANGED <<cells, nextId>>

Done ==
    /\ nextId > MaxSent
    /\ \A q \in Queues : count[q] = 0
    /\ UNCHANGED vars

Next ==
    \/ \E q \in Queues : Send(q)
    \/ \E q \in Queues : Receive(q)
    \/ \E q \in Queues : Reset(q)
    \/ Done

Spec == Init /\ [][Next]_vars

TypeOK ==
    /\ head \in [ Queues -> 0..CounterMod-1 ]
    /\ tail \in [ Queues -> 0..CounterMod-1 ]
    /\ count \in [ Queues -> 0..ServerCapacity + Capacity + 1 ]
    /\ cells \in [ Cells -> 0..MaxSent ]
    /\ nextId \in 1..MaxSent+1

\* every queue's cells hold exactly its unread datagrams, in order: no
\* unread datagram is overwritten, and receive reads the oldest, never a
\* stale cell
RingIsQueue ==
    \A q \in Queues :
        /\ count[q] = Len(queue[q])
        /\ \A k \in 1..Len(queue[q]) :
             cells[Addr(q, (head[q] + k - 1) % CounterMod)] = queue[q][k]

\* a queue never holds more than its capacity
WithinCapacity ==
    \A q \in Queues : count[q] <= Cap(q)

\* the block's carve arithmetic: every slot of every index lies in its own
\* queue's carve before the red zone, the red zone lies in the carve, no
\* two carves overlap, and the block's size holds every carve
StaysInRegion ==
    /\ \A q \in Queues :
         /\ RedZone(q) \subseteq CarveOf(q)
         /\ CarveOf(q) \subseteq 0..BlockBytes-1
         /\ \A i \in 0..CounterMod-1 :
              SlotSpan(q, i) \subseteq CarveOf(q) \ RedZone(q)
    /\ \A q1, q2 \in Queues : q1 # q2 => CarveOf(q1) \cap CarveOf(q2) = {}

\* an empty queue has restarted both indices
EmptyIsRestarted ==
    \A q \in Queues : count[q] = 0 => head[q] = 0 /\ tail[q] = 0

=============================================================================
