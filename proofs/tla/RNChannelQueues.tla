------------------------------ MODULE RNChannelQueues ------------------------------
\* Sequence scheduling for the per-channel descriptor FIFOs, the ready-channel
\* list, and the unreliable endpoint FIFO, as sequences. The C++ is
\* sdk/rocketnet_channel.cpp and sdk/rocketnet_endpoint.cpp. The resend
\* FIFO's names are RNResendGenerations.tla's.
\*
\* Each channel's descriptor FIFO is a linked queue: append at the tail
\* (Enqueue, rn_channels_finish with rn_channel_queue_append), CONFIG (id 0)
\* at the head of channel 0 once per connection (Front,
\* rn_ordered_announce_config with rn_channel_queue_push_front), and
\* numbering pops the head (Rotate, rn_ordered_write with
\* rn_channel_queue_pop); nothing leaves from the middle.
\*
\* The ready list holds the channels that take turns. ReadyJoin is the join
\* (rn_ready_link): a channel already on the list stays where it is (its
\* nonzero ready_ticket), any other joins at the back, or at the front for
\* CONFIG. Enqueue joins a channel whose FIFO was empty. Front removes
\* channel 0 under its name (rn_ready_remove_named) and joins it at the front.
\* Rotate is the numbering step of rn_ordered_write: the ready head's FIFO
\* head is numbered, the channel leaves the list under the name it joined
\* with, the head is popped, and the channel rejoins the back if its FIFO has
\* more (remove, pop, rejoin). The ticket counter's wrap (skipping 0), the
\* head generation a member captures and the links themselves are not here:
\* RNChannelQueuesLayout.tla carries them at the code's layout and checks its
\* links walk these sequences.
\*
\* The unreliable FIFO: EnqueueUnreliable is rn_unreliable_push, StaleEpoch is
\* the descriptor taking another ticket or leaving the queue, and Expire drops
\* the entries of a stale ticket with the rest kept in order.
\*
\* Reversed witnesses (Broken), each a line of the code changed:
\*   "ConfigAppended"    CONFIG appended at the tail of channel 0, not pushed
\*                       to its head: OrderPreserved;
\*   "RejoinEmpty"       Rotate rejoins the channel without checking that its
\*                       FIFO has more: ReadyIsWork;
\*   "FrontJoinsTwice"   Front joins channel 0 at the front with neither the
\*                       removal before it nor the join's membership test (with
\*                       either one alone the list keeps one entry):
\*                       ReadyNoDuplicates;
\*   "ExpireSwapRemove"  Expire drops a stale entry by moving the last entry
\*                       into its place: UnreliableOrder.

EXTENDS Naturals, Sequences

CONSTANTS MaxQ, Broken

Chans == {0, 1}

VARIABLES epoch, nextId, queue, ready, unreliable, configDone

vars == <<epoch, nextId, queue, ready, unreliable, configDone>>

InSeq(s, x) == \E i \in 1..Len(s) : s[i] = x
Remove(s, x) == SelectSeq(s, LAMBDA y : y # x)

\* rn_ready_link: a member keeps its place (its ready_ticket is nonzero)
ReadyJoin(s, c, front) ==
    IF InSeq(s, c) THEN s ELSE IF front THEN <<c>> \o s ELSE Append(s, c)

RECURSIVE DropHoles(_)
DropHoles(seq) ==
    IF seq = <<>> THEN <<>>
    ELSE IF Head(seq).hole THEN DropHoles(Tail(seq))
         ELSE <<Head(seq)>> \o DropHoles(Tail(seq))

Init ==
    /\ epoch = 1
    /\ nextId = 1
    /\ queue = [c \in Chans |-> <<>>]
    /\ ready = <<>>
    /\ unreliable = <<>>
    /\ configDone = FALSE

\* rn_channels_finish: append, and join the back if the FIFO was empty
Enqueue(c) ==
    /\ Len(queue[c]) < MaxQ
    /\ nextId <= MaxQ + 1
    /\ queue' = [queue EXCEPT ![c] = Append(@, nextId)]
    /\ nextId' = nextId + 1
    /\ ready' = IF queue[c] = <<>> THEN ReadyJoin(ready, c, FALSE) ELSE ready
    /\ UNCHANGED <<epoch, unreliable, configDone>>

\* rn_ordered_announce_config: CONFIG at the head of channel 0, once per
\* connection; channel 0 leaves the list under its name and joins the front.
\* The other ready channels keep their places behind it.
Front ==
    /\ ~configDone
    /\ Len(queue[0]) < MaxQ
    /\ queue' = [queue EXCEPT ![0] = IF Broken = "ConfigAppended" THEN Append(@, 0) ELSE <<0>> \o @]
    /\ ready' = IF Broken = "FrontJoinsTwice" THEN <<0>> \o ready
                ELSE ReadyJoin(Remove(ready, 0), 0, TRUE)
    /\ configDone' = TRUE
    /\ UNCHANGED <<epoch, nextId, unreliable>>

\* rn_ordered_write, a message numbered: the ready head's FIFO head is
\* numbered; the channel leaves the list, the head is popped, and the channel
\* rejoins the back if its FIFO has more
Rotate ==
    /\ ready # <<>>
    /\ queue[Head(ready)] # <<>>
    /\ LET c == Head(ready)
           rest == Tail(queue[c])
           left == Remove(ready, c)
       IN /\ queue' = [queue EXCEPT ![c] = rest]
          /\ ready' = IF rest # <<>> \/ Broken = "RejoinEmpty" THEN ReadyJoin(left, c, FALSE) ELSE left
    /\ UNCHANGED <<epoch, nextId, unreliable, configDone>>

EnqueueUnreliable ==
    /\ Len(unreliable) < MaxQ
    /\ nextId <= MaxQ + 1
    /\ unreliable' = Append(unreliable, [id |-> nextId, epoch |-> epoch, hole |-> FALSE])
    /\ nextId' = nextId + 1
    /\ UNCHANGED <<epoch, queue, ready, configDone>>

\* An outside event stamps one live unreliable entry with a previous epoch.
StaleEpoch ==
    /\ \E i \in 1..Len(unreliable) :
        /\ ~unreliable[i].hole
        /\ unreliable' = [unreliable EXCEPT ![i].epoch = 0]
    /\ UNCHANGED <<epoch, nextId, queue, ready, configDone>>

\* the swap removal of the first stale entry (Broken = "ExpireSwapRemove")
SwapRemove(s) ==
    LET stale == {i \in 1..Len(s) : s[i].epoch # epoch}
        i == CHOOSE k \in stale : \A j \in stale : k <= j
    IN IF stale = {} THEN s
       ELSE [k \in 1..(Len(s) - 1) |-> IF k = i THEN s[Len(s)] ELSE s[k]]

Expire ==
    /\ Len(unreliable) > 0
    /\ unreliable' =
         IF Broken = "ExpireSwapRemove" THEN SwapRemove(unreliable)
         ELSE DropHoles([i \in 1..Len(unreliable) |->
                IF unreliable[i].epoch # epoch THEN [unreliable[i] EXCEPT !.hole = TRUE]
                ELSE unreliable[i]])
    /\ UNCHANGED <<epoch, nextId, queue, ready, configDone>>

\* a connection reset clears every queue; the next connection announces again
Reset ==
    /\ epoch < 2
    /\ epoch' = epoch + 1
    /\ queue' = [c \in Chans |-> <<>>]
    /\ ready' = <<>>
    /\ unreliable' = <<>>
    /\ nextId' = 1
    /\ configDone' = FALSE

Next ==
    \/ \E c \in Chans : Enqueue(c)
    \/ Front
    \/ Rotate
    \/ EnqueueUnreliable
    \/ StaleEpoch
    \/ Expire
    \/ Reset

Spec == Init /\ [][Next]_vars

TypeOK ==
    /\ epoch \in 1..2
    /\ nextId \in 1..(MaxQ + 2)
    /\ queue \in [Chans -> Seq(0..(MaxQ + 1))]
    /\ \A c \in Chans : Len(queue[c]) <= MaxQ
    /\ Len(unreliable) <= MaxQ
    /\ ready \in Seq(Chans) /\ Len(ready) <= 2
    /\ configDone \in BOOLEAN

\* Each FIFO holds its entries in the order they were queued, CONFIG (id 0)
\* first: numbering pops the head, so first writes are in that order.
OrderPreserved ==
    \A c \in Chans : \A a, b \in 1..Len(queue[c]) : a < b => queue[c][a] < queue[c][b]

ReadyNoDuplicates ==
    \A i, j \in 1..Len(ready) : i # j => ready[i] # ready[j]

\* the channels that take turns are exactly those with work queued
ReadyIsWork ==
    \A c \in Chans : InSeq(ready, c) <=> queue[c] # <<>>

UnreliableOrder ==
    LET live == DropHoles(unreliable)
    IN \A a, b \in 1..Len(live) : a < b => live[a].id < live[b].id

================================================================================
