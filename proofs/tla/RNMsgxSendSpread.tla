--------------------------- MODULE RNMsgxSendSpread ---------------------------
(***************************************************************************)
(* The macOS batched server transport's choice of transmit worker          *)
(* (rocketnet_msgx_socket_table_queue_of, sdk/rocketnet_msgx_socket_table.h; *)
(* rocketnet_server_msgx_send_queue_of and _start_packet,                  *)
(* sdk/rocketnet_server_msgx_apple.cpp) as it feeds the per-queue send     *)
(* reserve (rocketnet_server_transport_send_reserves and                   *)
(* _open_fill_budget, sdk/rocketnet_server_transport.cpp).                 *)
(*                                                                         *)
(* A client with a socket leaves through its slot modulo the Q workers; a  *)
(* client with none (past the open budget, a full table) through its        *)
(* address hash.  Slots come off the table's free stack, slot 0 first, and *)
(* a slot freed by the sweep goes back on top (RNMsgxSocketTable Mark and  *)
(* the sweep's close loop).  Main changes the table only in the update's   *)
(* walk (Mark, Forget, Sweep); then it opens the fill budget, reserving     *)
(* each connected client's frame in the queue QueueOf names then; then the  *)
(* starts (main's own and the fill threads') take QueueOf as it reads at    *)
(* the start.                                                              *)
(*                                                                         *)
(* EvenWhenFull: with every slot held, the clients are spread across the   *)
(* workers within one of each other.  The busiest worker sets the rate     *)
(* (docs/MACOS.md), so an uneven split is a cost.                          *)
(* StartsInReservedQueue: every start of a fill phase lands in the queue   *)
(* its client's frame was reserved in at the open.                         *)
(*                                                                         *)
(* The code has BySlot TRUE and WalkInFill FALSE.  Broken configs:         *)
(* BySlot FALSE (the address hash for every client, the code before) must  *)
(* fail EvenWhenFull; WalkInFill TRUE (the table changed after the budget  *)
(* opened) must fail StartsInReservedQueue.                                *)
(***************************************************************************)
EXTENDS Integers, Sequences, FiniteSets

CONSTANTS
    Cap,            \* slots: the table's capacity (max_clients)
    Clients,        \* client ids
    Q,              \* transmit workers
    HashAt,         \* the address hash of each client: c * Q + q for client c hashed to worker q
    BySlot,         \* TRUE: a client with a socket goes by its slot
    WalkInFill,     \* FALSE: the table changes only in the walk, before the open
    MaxUpdates      \* bound

ASSUME \A c \in Clients : Cardinality({x \in HashAt : x \div Q = c}) = 1

Slots == 0..(Cap - 1)
Queues == 0..(Q - 1)
None == -1

AddressQueue(c) == (CHOOSE x \in HashAt : x \div Q = c) % Q

VARIABLES
    slotOf,         \* slotOf[c]: the open slot, or None
    free,           \* the free stack, its top last
    quarantine,     \* retired slots not yet freed
    phase,          \* "walk" or "fill"
    reservedQ,      \* reservedQ[c]: the queue the open reserved c's frame in, or None
    startedQ,       \* startedQ[c]: the queue c's start of this fill phase took, or None
    update

vars == <<slotOf, free, quarantine, phase, reservedQ, startedQ, update>>

QueueOf(c) == IF BySlot /\ slotOf[c] # None THEN slotOf[c] % Q ELSE AddressQueue(c)

Init ==
    /\ slotOf = [c \in Clients |-> None]
    /\ free = [i \in 1..Cap |-> Cap - i]
    /\ quarantine = {}
    /\ phase = "walk"
    /\ reservedQ = [c \in Clients |-> None]
    /\ startedQ = [c \in Clients |-> None]
    /\ update = 0

TableMayChange == phase = "walk" \/ WalkInFill

\* rocketnet_msgx_socket_table_mark: the top of the free stack
Mark(c) ==
    /\ TableMayChange
    /\ slotOf[c] = None
    /\ Len(free) > 0
    /\ slotOf' = [slotOf EXCEPT ![c] = free[Len(free)]]
    /\ free' = SubSeq(free, 1, Len(free) - 1)
    /\ UNCHANGED <<quarantine, phase, reservedQ, startedQ, update>>

\* rocketnet_msgx_socket_table_forget: the slot into quarantine
Forget(c) ==
    /\ TableMayChange
    /\ slotOf[c] # None
    /\ quarantine' = quarantine \cup {slotOf[c]}
    /\ slotOf' = [slotOf EXCEPT ![c] = None]
    /\ UNCHANGED <<free, phase, reservedQ, startedQ, update>>

\* the sweep's close loop: a quarantined slot back on top of the free stack
Sweep ==
    /\ TableMayChange
    /\ \E s \in quarantine :
        /\ quarantine' = quarantine \ {s}
        /\ free' = Append(free, s)
    /\ UNCHANGED <<slotOf, phase, reservedQ, startedQ, update>>

\* open_fill_budget: every connected client's frame reserved in its queue
Open ==
    /\ phase = "walk"
    /\ update < MaxUpdates
    /\ phase' = "fill"
    /\ reservedQ' = [c \in Clients |-> QueueOf(c)]
    /\ startedQ' = [c \in Clients |-> None]
    /\ UNCHANGED <<slotOf, free, quarantine, update>>

\* a start: the worker QueueOf names as it reads now
Start(c) ==
    /\ phase = "fill"
    /\ startedQ[c] = None
    /\ startedQ' = [startedQ EXCEPT ![c] = QueueOf(c)]
    /\ UNCHANGED <<slotOf, free, quarantine, phase, reservedQ, update>>

\* send_packets: the fill phase closes, the next update's walk begins
Close ==
    /\ phase = "fill"
    /\ phase' = "walk"
    /\ update' = update + 1
    /\ UNCHANGED <<slotOf, free, quarantine, reservedQ, startedQ>>

Next ==
    \/ \E c \in Clients : Mark(c) \/ Forget(c) \/ Start(c)
    \/ Sweep \/ Open \/ Close

Spec == Init /\ [][Next]_vars

TypeOK ==
    /\ slotOf \in [Clients -> Slots \cup {None}]
    /\ quarantine \subseteq Slots
    /\ phase \in {"walk", "fill"}
    /\ reservedQ \in [Clients -> Queues \cup {None}]
    /\ startedQ \in [Clients -> Queues \cup {None}]

\* every slot is held by one client, free, or in quarantine, exactly once
SlotsAccounted ==
    /\ \A c1, c2 \in Clients : c1 # c2 /\ slotOf[c1] # None => slotOf[c1] # slotOf[c2]
    /\ Len(free) + Cardinality(quarantine) + Cardinality({c \in Clients : slotOf[c] # None}) = Cap

Held(q) == Cardinality({c \in Clients : slotOf[c] # None /\ QueueOf(c) = q})

EvenWhenFull ==
    Cardinality({c \in Clients : slotOf[c] # None}) = Cap =>
        \A q1, q2 \in Queues : Held(q1) - Held(q2) <= 1

StartsInReservedQueue ==
    \A c \in Clients : startedQ[c] # None => startedQ[c] = reservedQ[c]

=============================================================================
