------------------------------ MODULE RNChannelQueues ------------------------------
\* Generation-tagged sequence scheduling for the reliable resend FIFO, the
\* per-channel descriptor FIFO, ready-channel membership, and the unreliable
\* endpoint FIFO. The C++ is sdk/rocketnet_window_ack.cpp,
\* sdk/rocketnet_channel.cpp and sdk/rocketnet_endpoint.cpp.
\*
\* A resend entry is removed only when the caller names its generation. The
\* per-channel descriptor FIFO is a linked queue: append at the tail, CONFIG
\* (id 0) at the head once, numbering pops the head, all O(1), and nothing
\* leaves from the middle (rn_channel_queue_append, rn_channel_queue_push_front,
\* rn_channel_queue_pop). Ready membership has no duplicates.

EXTENDS Naturals, Sequences

CONSTANTS MaxQ, BrokenStale

VARIABLES epoch, nextId, queue, ready, unreliable, resend, staleRemoved

vars == <<epoch, nextId, queue, ready, unreliable, resend, staleRemoved>>

Rec(id, gen) == [id |-> id, gen |-> gen]

RECURSIVE DropHoles(_)
DropHoles(seq) ==
    IF seq = <<>> THEN <<>>
    ELSE IF Head(seq).hole THEN DropHoles(Tail(seq))
         ELSE <<Head(seq)>> \o DropHoles(Tail(seq))


Init ==
    /\ epoch = 1
    /\ nextId = 1
    /\ queue = <<>>
    /\ ready = <<>>
    /\ unreliable = <<>>
    /\ resend = <<>>
    /\ staleRemoved = FALSE

Enqueue ==
    /\ Len(queue) < MaxQ
    /\ nextId <= MaxQ + 1
    /\ queue' = Append(queue, Rec(nextId, 1))
    /\ nextId' = nextId + 1
    /\ ready' = IF ready = <<>> THEN <<1>> ELSE ready
    /\ UNCHANGED <<epoch, unreliable, resend, staleRemoved>>

\* CONFIG at the head, once per connection: one link, nothing restamped.
Front ==
    /\ Len(queue) < MaxQ
    /\ \A i \in 1..Len(queue) : queue[i].id # 0
    /\ queue' = <<Rec(0, 1)>> \o queue
    /\ ready' = <<1>>
    /\ UNCHANGED <<epoch, nextId, unreliable, resend, staleRemoved>>

EnqueueUnreliable ==
    /\ Len(unreliable) < MaxQ
    /\ nextId <= MaxQ + 1
    /\ unreliable' = Append(unreliable, [id |-> nextId, gen |-> 1, epoch |-> epoch, hole |-> FALSE])
    /\ nextId' = nextId + 1
    /\ UNCHANGED <<epoch, queue, ready, resend, staleRemoved>>

\* An outside event stamps one live unreliable entry with a previous epoch.
StaleEpoch ==
    /\ \E i \in 1..Len(unreliable) :
        /\ ~unreliable[i].hole
        /\ unreliable' = [unreliable EXCEPT ![i].epoch = 0]
    /\ UNCHANGED <<epoch, nextId, queue, ready, resend, staleRemoved>>

Expire ==
    /\ Len(unreliable) > 0
    /\ unreliable' = DropHoles([i \in 1..Len(unreliable) |->
            IF unreliable[i].epoch # epoch THEN [unreliable[i] EXCEPT !.hole = TRUE]
            ELSE unreliable[i]])
    /\ UNCHANGED <<epoch, nextId, queue, ready, resend, staleRemoved>>

Resend ==
    /\ Len(resend) < MaxQ
    /\ resend' = Append(resend, [num |-> Len(resend) + 1, gen |-> 1])
    /\ UNCHANGED <<epoch, nextId, queue, ready, unreliable, staleRemoved>>

\* Ack removes the head only while it still has generation 1.
Ack ==
    /\ Len(resend) > 0
    /\ resend[1].gen = 1
    /\ resend' = Tail(resend)
    /\ UNCHANGED <<epoch, nextId, queue, ready, unreliable, staleRemoved>>

StaleAck ==
    /\ Len(resend) > 0
    /\ IF BrokenStale
       THEN /\ resend' = Tail(resend)
            /\ staleRemoved' = TRUE
       ELSE /\ UNCHANGED resend
            /\ staleRemoved' = FALSE
    /\ UNCHANGED <<epoch, nextId, queue, ready, unreliable>>

ReadyJoin ==
    /\ Len(ready) < 2
    /\ LET id == Len(ready) + 1
       IN /\ \A i \in 1..Len(ready) : ready[i] # id
          /\ ready' = Append(ready, id)
    /\ UNCHANGED <<epoch, nextId, queue, unreliable, resend, staleRemoved>>

Rotate ==
    /\ Len(ready) = 2
    /\ ready' = <<ready[2], ready[1]>>
    /\ UNCHANGED <<epoch, nextId, queue, unreliable, resend, staleRemoved>>

Reset ==
    /\ epoch < 2
    /\ epoch' = epoch + 1
    /\ queue' = <<>>
    /\ ready' = <<>>
    /\ unreliable' = <<>>
    /\ resend' = <<>>
    /\ nextId' = 1
    /\ UNCHANGED staleRemoved

Next ==
    \/ Enqueue
    \/ Front
    \/ EnqueueUnreliable
    \/ StaleEpoch
    \/ Expire
    \/ Resend
    \/ Ack
    \/ StaleAck
    \/ ReadyJoin
    \/ Rotate
    \/ Reset

Spec == Init /\ [][Next]_vars

TypeOK ==
    /\ epoch \in 1..2
    /\ nextId \in 1..(MaxQ + 2)
    /\ Len(queue) <= MaxQ
    /\ Len(unreliable) <= MaxQ
    /\ Len(resend) <= MaxQ
    /\ Len(ready) <= 2
    /\ staleRemoved \in BOOLEAN
    /\ \A i \in DOMAIN queue :
        /\ queue[i].gen \in 1..2

NamedGenerationOnly == staleRemoved = FALSE

\* The queue holds its entries in the order they were queued, CONFIG first.
OrderPreserved ==
    \A a, b \in 1..Len(queue) :
        a < b => \/ queue[a].id < queue[b].id
                 \/ queue[a].id = 0

ReadyNoDuplicates ==
    \A i, j \in 1..Len(ready) : i # j => ready[i] # ready[j]

UnreliableOrder ==
    LET live == DropHoles(unreliable)
    IN \A a, b \in 1..Len(live) : a < b => live[a].id < live[b].id

================================================================================
