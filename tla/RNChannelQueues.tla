------------------------------ MODULE RNChannelQueues ------------------------------
\* Generation-tagged sequence scheduling for the reliable resend FIFO, the
\* per-channel descriptor FIFO, ready-channel membership, and the unreliable
\* endpoint FIFO. The C++ is sdk/rocketnet_window_ack.cpp,
\* sdk/rocketnet_channel.cpp and sdk/rocketnet_endpoint.cpp.
\*
\* A resend or queue entry is removed only when the caller names its
\* generation. Holes compact in one action, not once per emitted record.
\* Tickets increase along a live queue. CONFIG (id 0) is inserted at the front
\* and that queue's tickets are rebased. Ready membership has no duplicates.

EXTENDS Naturals, Sequences

CONSTANTS MaxQ, HoleBound, BrokenStale

VARIABLES epoch, ticketNext, nextId, queue, ready, unreliable, resend, holes, staleRemoved

vars == <<epoch, ticketNext, nextId, queue, ready, unreliable, resend, holes, staleRemoved>>

Rec(id, gen, ticket) == [id |-> id, gen |-> gen, ticket |-> ticket, hole |-> FALSE]

RECURSIVE DropHoles(_)
DropHoles(seq) ==
    IF seq = <<>> THEN <<>>
    ELSE IF Head(seq).hole THEN DropHoles(Tail(seq))
         ELSE <<Head(seq)>> \o DropHoles(Tail(seq))

Live(seq) == {i \in DOMAIN seq : ~seq[i].hole}

\* Tickets stay strictly increasing inside this instance. The C++ counter is a
\* uint32 that skips 0 and RN_ENQUEUE_HOLE; a compact restamps one channel.
TicketLimit == MaxQ * 4

Take == <<ticketNext, ticketNext + 1>>

Init ==
    /\ epoch = 1
    /\ ticketNext = 1
    /\ nextId = 1
    /\ queue = <<>>
    /\ ready = <<>>
    /\ unreliable = <<>>
    /\ resend = <<>>
    /\ holes = 0
    /\ staleRemoved = FALSE

Enqueue ==
    /\ Len(queue) < MaxQ
    /\ ticketNext < TicketLimit
    /\ nextId <= MaxQ * 3
    /\ LET pair == Take
           rec == Rec(nextId, 1, pair[1])
       IN /\ queue' = Append(queue, rec)
          /\ ticketNext' = pair[2]
          /\ nextId' = nextId + 1
          /\ ready' = IF ready = <<>> THEN <<1>> ELSE ready
          /\ UNCHANGED <<epoch, unreliable, resend, holes, staleRemoved>>

\* CONFIG at the front. This channel's live tickets are rebased in one step
\* so the new head holds the smallest ticket and the previous order remains.
Front ==
    /\ Len(queue) < MaxQ
    /\ LET inserted == <<Rec(0, 1, 1)>> \o queue
           kept == DropHoles(inserted)
           rebuilt == [i \in 1..Len(kept) |-> Rec(kept[i].id, kept[i].gen, i)]
       IN /\ queue' = rebuilt
          /\ ticketNext' = Len(kept) + 1
          /\ holes' = 0
          /\ ready' = <<1>>
          /\ UNCHANGED <<epoch, nextId, unreliable, resend, staleRemoved>>

Cancel ==
    /\ \E i \in 1..Len(queue) :
        /\ ~queue[i].hole
        /\ LET holed == [queue EXCEPT ![i].hole = TRUE, ![i].ticket = 0]
           IN IF holes + 1 > HoleBound
              THEN /\ queue' = DropHoles(holed)
                   /\ holes' = 0
              ELSE /\ queue' = holed
                   /\ holes' = holes + 1
    /\ UNCHANGED <<epoch, ticketNext, nextId, ready, unreliable, resend, staleRemoved>>

\* A cancel that names gen+1. The real scheduler leaves the entry. BrokenStale
\* removes it, which NamedGenerationOnly forbids.
StaleCancel ==
    /\ \E i \in 1..Len(queue) : ~queue[i].hole
    /\ IF BrokenStale
       THEN /\ \E i \in 1..Len(queue) :
                /\ ~queue[i].hole
                /\ queue' = [queue EXCEPT ![i].hole = TRUE, ![i].ticket = 0]
                /\ holes' = holes + 1
           /\ staleRemoved' = TRUE
       ELSE /\ UNCHANGED <<queue, holes>>
            /\ staleRemoved' = FALSE
    /\ UNCHANGED <<epoch, ticketNext, nextId, ready, unreliable, resend>>

EnqueueUnreliable ==
    /\ Len(unreliable) < MaxQ
    /\ unreliable' = Append(unreliable, [id |-> Len(unreliable) + 1, gen |-> 1, epoch |-> epoch, hole |-> FALSE])
    /\ UNCHANGED <<epoch, ticketNext, nextId, queue, ready, resend, holes, staleRemoved>>

\* An outside event stamps one live unreliable entry with a previous epoch.
StaleEpoch ==
    /\ \E i \in 1..Len(unreliable) :
        /\ ~unreliable[i].hole
        /\ unreliable' = [unreliable EXCEPT ![i].epoch = 0]
    /\ UNCHANGED <<epoch, ticketNext, nextId, queue, ready, resend, holes, staleRemoved>>

Expire ==
    /\ Len(unreliable) > 0
    /\ unreliable' = DropHoles([i \in 1..Len(unreliable) |->
            IF unreliable[i].epoch # epoch THEN [unreliable[i] EXCEPT !.hole = TRUE]
            ELSE unreliable[i]])
    /\ UNCHANGED <<epoch, ticketNext, nextId, queue, ready, resend, holes, staleRemoved>>

Resend ==
    /\ Len(resend) < MaxQ
    /\ resend' = Append(resend, [num |-> Len(resend) + 1, gen |-> 1])
    /\ UNCHANGED <<epoch, ticketNext, nextId, queue, ready, unreliable, holes, staleRemoved>>

\* Ack removes the head only while it still has generation 1.
Ack ==
    /\ Len(resend) > 0
    /\ resend[1].gen = 1
    /\ resend' = Tail(resend)
    /\ UNCHANGED <<epoch, ticketNext, nextId, queue, ready, unreliable, holes, staleRemoved>>

StaleAck ==
    /\ Len(resend) > 0
    /\ IF BrokenStale
       THEN /\ resend' = Tail(resend)
            /\ staleRemoved' = TRUE
       ELSE /\ UNCHANGED resend
            /\ staleRemoved' = FALSE
    /\ UNCHANGED <<epoch, ticketNext, nextId, queue, ready, unreliable, holes>>

ReadyJoin ==
    /\ Len(ready) < 2
    /\ LET id == Len(ready) + 1
       IN /\ \A i \in 1..Len(ready) : ready[i] # id
          /\ ready' = Append(ready, id)
    /\ UNCHANGED <<epoch, ticketNext, nextId, queue, unreliable, resend, holes, staleRemoved>>

Rotate ==
    /\ Len(ready) = 2
    /\ ready' = <<ready[2], ready[1]>>
    /\ UNCHANGED <<epoch, ticketNext, nextId, queue, unreliable, resend, holes, staleRemoved>>

Compact ==
    /\ holes > 0
    /\ queue' = DropHoles(queue)
    /\ holes' = 0
    /\ UNCHANGED <<epoch, ticketNext, nextId, ready, unreliable, resend, staleRemoved>>

Reset ==
    /\ epoch < 2
    /\ epoch' = epoch + 1
    /\ ticketNext' = 1
    /\ queue' = <<>>
    /\ ready' = <<>>
    /\ unreliable' = <<>>
    /\ resend' = <<>>
    /\ holes' = 0
    /\ nextId' = 1
    /\ UNCHANGED staleRemoved

Next ==
    \/ Enqueue
    \/ Front
    \/ Cancel
    \/ StaleCancel
    \/ EnqueueUnreliable
    \/ StaleEpoch
    \/ Expire
    \/ Resend
    \/ Ack
    \/ StaleAck
    \/ ReadyJoin
    \/ Rotate
    \/ Compact
    \/ Reset

Spec == Init /\ [][Next]_vars /\ WF_vars(Compact)

\* The reversed witness has no fairness. StaleCancel and StaleAck remove a
\* generation the caller did not name.
StaleSpec == Init /\ [][Next]_vars

TypeOK ==
    /\ epoch \in 1..2
    /\ ticketNext \in 1..TicketLimit
    /\ nextId \in 1..(MaxQ * 3 + 1)
    /\ Len(queue) <= MaxQ
    /\ Len(unreliable) <= MaxQ
    /\ Len(resend) <= MaxQ
    /\ Len(ready) <= 2
    /\ holes <= MaxQ
    /\ staleRemoved \in BOOLEAN
    /\ \A i \in DOMAIN queue :
        /\ queue[i].gen \in 1..2
        /\ queue[i].ticket \in 0..(MaxQ * 4)
        /\ queue[i].hole \in BOOLEAN

NamedGenerationOnly == staleRemoved = FALSE

TicketsDistinct ==
    \A i, j \in Live(queue) : i < j => queue[i].ticket < queue[j].ticket

HoleBoundInv == holes <= HoleBound

OrderPreserved ==
    LET live == DropHoles(queue)
    IN \A a, b \in 1..Len(live) :
        a < b => \/ live[a].id < live[b].id
                 \/ live[a].id = 0

ReadyNoDuplicates ==
    \A i, j \in 1..Len(ready) : i # j => ready[i] # ready[j]

UnreliableOrder ==
    LET live == DropHoles(unreliable)
    IN \A a, b \in 1..Len(live) : a < b => live[a].id < live[b].id

HolesEventuallyCompact == holes > 0 ~> holes = 0

================================================================================
