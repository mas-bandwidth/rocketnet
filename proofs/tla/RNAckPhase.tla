----------------------------- MODULE RNAckPhase -----------------------------
(***************************************************************************)
(* The server's ack phase (SERVER-UPDATE-ACK.md P3; sdk/rocketnet_ack.cpp   *)
(* rocketnet_server_acks_defer, _apply_client, _reset_slot, _gather;       *)
(* sdk/rocketnet_server_transport.cpp rocketnet_server_transport_update_  *)
(* begin, _acks, _end; sdk/rocketnet_message_server.cpp                    *)
(* rn_message_object_update_acks and the back's drain; README-RNAckPhase.md)*)
(*                                                                         *)
(* One update: the receive loop on main defers each session packet's ack   *)
(* block, with its client and connection tag, in arrival order (Defer).    *)
(* When the entries run out the update spills: everything deferred so far  *)
(* is applied and gathered, and the rest of the update applies its blocks  *)
(* as they come (Spill, Inline).  A reset of a client in the loop applies  *)
(* that client's deferred blocks first (ResetFlush).  Then the ack phase:  *)
(* any thread applies a client's blocks, one thread per client (AckStart,  *)
(* AckStep, AckEnd), and the message layer takes a settled client's ids    *)
(* at once (AckEnd).    After the join, main applies every client nobody  *)
(* called and copies the ids into the public batch in arrival order, the   *)
(* counts summed (Gather); then the maintenance sweep sends (Sweep); then  *)
(* the message layer's drain takes the batch's ids of every client not    *)
(* taken (Drain).                                                          *)
(*                                                                         *)
(* Beside it runs the serial reference, the update before the ack phase:   *)
(* each block applied in the receive loop as it arrives.  The model's     *)
(* rules are that the two leave the same endpoints, the same batch in the  *)
(* same order, the same counts and the same ids handed to the message      *)
(* layer, and that no send lands on an endpoint between a block's arrival  *)
(* and its apply.                                                          *)
(*                                                                         *)
(* An endpoint is its history since its last reset: the arrivals applied  *)
(* to it and the sends recorded on it ("S"), in the order they landed, and *)
(* its epoch (resets).  process_block is a function of the endpoint and    *)
(* the block, so the ids a block surfaces are named by the client, the     *)
(* epoch and the history it was applied on: two worlds that apply the same *)
(* block on the same history surface the same ids.  A block's counts      *)
(* (unknown sequences, a full batch) are k, 0 or 1.                        *)
(*                                                                         *)
(* The code's words:                                                       *)
(*   entries[i]   acks->entries[i] and its block in acks->store; res is    *)
(*                the ids it surfaced (acked_first, acked_count into the   *)
(*                client's segment), <<>> before it is applied            *)
(*   pending[c]   the client's list, head to tail (clients[c].head, next) *)
(*   ccount[c]    clients[c].unknown_sequence and batch_full               *)
(*   mode         acks->mode: "defer" (DEFERRING), "inline" (DIRECT)       *)
(*   owner[c]     the fill-phase claim of the client in this ack phase     *)
(*   tc, tlist, tpos  a thread's call in progress: its client, the list it *)
(*                read from the head, its place in it                      *)
(*   batch, count acked_packet_ids (with acked_client_index and the tags)  *)
(*                and the two counters                                     *)
(*   taken[c]     hot_connections[c].acks_taken                            *)
(*   delivered[c] the ids rn_endpoint_on_acked_ids was given for c         *)
(***************************************************************************)
EXTENDS Naturals, Sequences, FiniteSets

CONSTANTS
    Clients,            \* the client slots
    Threads,            \* the game's job threads that make ack calls
    NoOwner,            \* no thread claimed the client (0 in the code)
    NoClient,           \* a thread with no call in progress
    ClientOrder,        \* the clients as a sequence: the order a by-client gather walks
    MaxArrivals,        \* session packets in a behaviour
    MaxUpdates,         \* updates in a behaviour
    MaxResets,          \* resets in a behaviour
    Capacity,           \* the entries an update defers before it spills
    NoOwnerCheck,       \* TRUE: two threads may apply one client in one phase (reversed witness)
    GatherByClient,     \* TRUE: the gather copies client by client, not in arrival order (reversed witness)
    SweepBeforeGather,  \* TRUE: update_end sweeps (sends) before it gathers (reversed witness)
    ResetNoFlush,       \* TRUE: a reset leaves the client's deferred blocks for later (reversed witness)
    SpillNoFlush,       \* TRUE: a spill goes inline without gathering what was deferred (reversed witness)
    CountsBeforeApply,  \* TRUE: the gather sums the counts before it applies the clients nobody called (reversed witness)
    DrainTaken,         \* TRUE: the drain takes a taken client's ids again (reversed witness)
    TakeTwice           \* TRUE: a second call for a taken client hands its ids on again (reversed witness)

ASSUME NoOwner \notin Threads
ASSUME NoClient \notin Clients
ASSUME Capacity \in Nat \ {0}
ASSUME ClientOrder \in Seq(Clients) /\ Len(ClientOrder) = Cardinality(Clients) /\
       \A c \in Clients : \E i \in 1..Len(ClientOrder) : ClientOrder[i] = c

VARIABLES
    phase,          \* "idle", "receive", "acks", "swept" (SweepBeforeGather), "gathered", "done"
    updates,        \* updates begun
    narr,           \* arrivals so far; an arrival is named by its number
    nresets,        \* resets so far
    touched,        \* clients that had a packet or a first keep-alive in this update
    resetNow,       \* clients reset in this update's receive loop
    mode,
    entries,
    pending,
    owner,
    tc, tlist, tpos,
    hist,           \* [Clients -> Seq]: the endpoint's history since its reset
    epoch,          \* [Clients -> Nat]: its resets
    alog,           \* [Clients -> Seq]: every arrival applied to the client, in apply order
    ccount,
    batch,          \* this update's public batch: <<client, ids>> records
    count,          \* the counters, over every update
    taken,
    delivered,      \* [Clients -> Seq]: ids handed to the message layer, over every update
    sentPending,    \* a send landed on an endpoint while it had a block deferred
    shist, sepoch,  \* the serial reference: its endpoints
    sbatch, scount, \* its batch (this update) and counters
    sdelivered      \* its message layer

vars == << phase, updates, narr, nresets, touched, resetNow, mode, entries, pending, owner, tc, tlist, tpos,
           hist, epoch, alog, ccount, batch, count, taken, delivered, sentPending,
           shist, sepoch, sbatch, scount, sdelivered >>

serial == << shist, sepoch, sbatch, scount, sdelivered >>

-----------------------------------------------------------------------------
(* The world the folds below rewrite: the endpoints, the entries and lists  *)

World == [ hist |-> hist, alog |-> alog, ccount |-> ccount, entries |-> entries, pending |-> pending ]

\* apply the entries at the indexes idxs, in that order, to client c
\* (rocketnet_server_acks_apply_client's loop, process_block per block)
RECURSIVE ApplyIdx(_, _, _)
ApplyIdx(w, c, idxs) ==
    IF idxs = << >> THEN w
    ELSE LET i == Head(idxs)
             e == w.entries[i]
         IN ApplyIdx([ w EXCEPT !.entries[i].res = << c, epoch[c], w.hist[c] >>,
                                !.hist[c] = Append(w.hist[c], e.n),
                                !.alog[c] = Append(w.alog[c], e.n),
                                !.ccount[c] = w.ccount[c] + e.k ],
                     c, Tail(idxs))

\* a client's whole list applied and emptied (head = -1)
Flush(w, c) == [ ApplyIdx(w, c, w.pending[c]) EXCEPT !.pending[c] = << >> ]

RECURSIVE FlushSet(_, _)
FlushSet(w, S) == IF S = {} THEN w ELSE LET c == CHOOSE x \in S : TRUE IN FlushSet(Flush(w, c), S \ {c})

RECURSIVE SumSet(_, _)
SumSet(f, S) == IF S = {} THEN 0 ELSE LET c == CHOOSE x \in S : TRUE IN f[c] + SumSet(f, S \ {c})

Records(es) == [ i \in 1..Len(es) |-> << es[i].c, es[i].res >> ]

RECURSIVE ByClientFrom(_, _)
ByClientFrom(es, k) ==
    IF k > Len(ClientOrder) THEN << >>
    ELSE LET c == ClientOrder[k] IN SelectSeq(es, LAMBDA e : e.c = c) \o ByClientFrom(es, k + 1)

\* the gather (rocketnet_server_acks_gather): every client nobody called
\* applied on main, the entries' ids in arrival order, the counts summed
GatherW(w) ==
    LET f == FlushSet(w, Clients)
        order == IF GatherByClient THEN ByClientFrom(f.entries, 1) ELSE f.entries
        sum == IF CountsBeforeApply THEN SumSet(w.ccount, Clients) ELSE SumSet(f.ccount, Clients)
    IN [ w |-> [ f EXCEPT !.entries = << >>, !.ccount = [ c \in Clients |-> 0 ] ],
         add |-> Records(order),
         sum |-> sum ]

\* a send recorded on every endpoint (the sweep), or on one
SendAll(h) == [ c \in Clients |-> Append(h[c], "S") ]

AnyPending(p, S) == \E c \in S : p[c] # << >>

\* the serial reference applies a block as it arrives
SerialArrive(c, k, n) ==
    /\ sbatch' = Append(sbatch, << c, << c, sepoch[c], shist[c] >> >>)
    /\ shist' = [ shist EXCEPT ![c] = Append(@, n) ]
    /\ scount' = scount + k
    /\ UNCHANGED << sepoch, sdelivered >>

\* a block applied as it comes, on world w (the DIRECT path in the loop)
InlineOn(w, c, k, n) ==
    [ w |-> [ w EXCEPT !.hist[c] = Append(w.hist[c], n), !.alog[c] = Append(w.alog[c], n) ],
      rec |-> << c, << c, epoch[c], w.hist[c] >> >> ]

SetWorld(w) ==
    /\ hist' = w.hist
    /\ alog' = w.alog
    /\ ccount' = w.ccount
    /\ entries' = w.entries
    /\ pending' = w.pending

-----------------------------------------------------------------------------

Init ==
    /\ phase = "idle"
    /\ updates = 0
    /\ narr = 0
    /\ nresets = 0
    /\ touched = {}
    /\ resetNow = {}
    /\ mode = "inline"
    /\ entries = << >>
    /\ pending = [ c \in Clients |-> << >> ]
    /\ owner = [ c \in Clients |-> NoOwner ]
    /\ tc = [ t \in Threads |-> NoClient ]
    /\ tlist = [ t \in Threads |-> << >> ]
    /\ tpos = [ t \in Threads |-> 1 ]
    /\ hist = [ c \in Clients |-> << >> ]
    /\ epoch = [ c \in Clients |-> 0 ]
    /\ alog = [ c \in Clients |-> << >> ]
    /\ ccount = [ c \in Clients |-> 0 ]
    /\ batch = << >>
    /\ count = 0
    /\ taken = [ c \in Clients |-> FALSE ]
    /\ delivered = [ c \in Clients |-> << >> ]
    /\ sentPending = FALSE
    /\ shist = [ c \in Clients |-> << >> ]
    /\ sepoch = [ c \in Clients |-> 0 ]
    /\ sbatch = << >>
    /\ scount = 0
    /\ sdelivered = [ c \in Clients |-> << >> ]

\* update_begin: begin_update (rocketnet_server_acks_begin_update), then the receive loop
Begin ==
    /\ phase = "idle"
    /\ updates < MaxUpdates
    /\ phase' = "receive"
    /\ updates' = updates + 1
    /\ mode' = "defer"
    /\ touched' = {}
    /\ resetNow' = {}
    /\ batch' = << >>
    /\ sbatch' = << >>
    /\ UNCHANGED << narr, nresets, entries, pending, owner, tc, tlist, tpos, hist, epoch, alog, ccount, count,
                    taken, delivered, sentPending, shist, sepoch, scount, sdelivered >>

\* a session packet's block, deferred with room (rocketnet_server_acks_defer)
Defer(c, k) ==
    /\ phase = "receive"
    /\ narr < MaxArrivals
    /\ mode = "defer"
    /\ Len(entries) < Capacity
    /\ narr' = narr + 1
    /\ entries' = Append(entries, [ c |-> c, k |-> k, n |-> narr + 1, res |-> << >> ])
    /\ pending' = [ pending EXCEPT ![c] = Append(@, Len(entries) + 1) ]
    /\ touched' = touched \cup {c}
    /\ SerialArrive(c, k, narr + 1)
    /\ UNCHANGED << phase, updates, nresets, resetNow, mode, owner, tc, tlist, tpos, hist, epoch, alog, ccount,
                    batch, count, taken, delivered, sentPending >>

\* out of room: rocketnet_server_protocol_ack_gather on main, then this
\* block and the rest of the update inline
Spill(c, k) ==
    /\ phase = "receive"
    /\ narr < MaxArrivals
    /\ mode = "defer"
    /\ Len(entries) >= Capacity
    /\ narr' = narr + 1
    /\ mode' = "inline"
    /\ IF SpillNoFlush
       THEN LET i == InlineOn(World, c, k, narr + 1)
            IN /\ SetWorld(i.w)
               /\ batch' = Append(batch, i.rec)
               /\ count' = count + k
       ELSE LET g == GatherW(World)
                i == InlineOn(g.w, c, k, narr + 1)
            IN /\ SetWorld(i.w)
               /\ batch' = Append(batch \o g.add, i.rec)
               /\ count' = count + g.sum + k
    /\ touched' = touched \cup {c}
    /\ SerialArrive(c, k, narr + 1)
    /\ UNCHANGED << phase, updates, nresets, resetNow, owner, tc, tlist, tpos, epoch, taken, delivered, sentPending >>

\* after a spill, a block applied in the loop as before the ack phase
Inline(c, k) ==
    /\ phase = "receive"
    /\ narr < MaxArrivals
    /\ mode = "inline"
    /\ narr' = narr + 1
    /\ LET i == InlineOn(World, c, k, narr + 1)
       IN /\ SetWorld(i.w)
          /\ batch' = Append(batch, i.rec)
    /\ count' = count + k
    /\ touched' = touched \cup {c}
    /\ SerialArrive(c, k, narr + 1)
    /\ UNCHANGED << phase, updates, nresets, resetNow, mode, owner, tc, tlist, tpos, epoch, taken, delivered, sentPending >>

\* the receive loop's one session send: the first keep-alive of a slot
\* assigned in this update, which had no packet in it before
FirstKeepAlive(c) ==
    /\ phase = "receive"
    /\ c \notin touched
    /\ touched' = touched \cup {c}
    /\ hist' = [ hist EXCEPT ![c] = Append(@, "S") ]
    /\ shist' = [ shist EXCEPT ![c] = Append(@, "S") ]
    /\ sentPending' = ( sentPending \/ pending[c] # << >> )
    /\ UNCHANGED << phase, updates, narr, nresets, resetNow, mode, entries, pending, owner, tc, tlist, tpos, epoch,
                    alog, ccount, batch, count, taken, delivered, sepoch, sbatch, scount, sdelivered >>

\* a reset in the loop (its disconnect packet, the integrity limit):
\* rocketnet_server_acks_reset_slot applies the client's deferred blocks, then resets
ResetFlush(c) ==
    /\ phase = "receive"
    /\ nresets < MaxResets
    /\ nresets' = nresets + 1
    /\ resetNow' = resetNow \cup {c}
    /\ LET w == IF ResetNoFlush THEN World ELSE Flush(World, c)
       IN SetWorld([ w EXCEPT !.hist[c] = << >> ])
    /\ epoch' = [ epoch EXCEPT ![c] = @ + 1 ]
    /\ shist' = [ shist EXCEPT ![c] = << >> ]
    /\ sepoch' = [ sepoch EXCEPT ![c] = @ + 1 ]
    /\ UNCHANGED << phase, updates, narr, touched, mode, owner, tc, tlist, tpos, batch, count, taken, delivered,
                    sentPending, sbatch, scount, sdelivered >>

\* the receive loop ends; update_begin opens the ack phase
OpenAcks ==
    /\ phase = "receive"
    /\ phase' = "acks"
    /\ UNCHANGED << updates, narr, nresets, touched, resetNow, mode, entries, pending, owner, tc, tlist, tpos,
                    hist, epoch, alog, ccount, batch, count, taken, delivered, sentPending, serial >>

\* an ack call (rocketnet_server_update_acks) begins: the fill-phase claim,
\* one thread per client in the phase; a second thread is refused and does nothing
AckStart(t, c) ==
    /\ phase = "acks"
    /\ tc[t] = NoClient
    /\ owner[c] \in { NoOwner, t } \/ NoOwnerCheck
    /\ owner' = [ owner EXCEPT ![c] = t ]
    /\ tc' = [ tc EXCEPT ![t] = c ]
    /\ tlist' = [ tlist EXCEPT ![t] = pending[c] ]
    /\ tpos' = [ tpos EXCEPT ![t] = 1 ]
    /\ UNCHANGED << phase, updates, narr, nresets, touched, resetNow, mode, entries, pending, hist, epoch, alog,
                    ccount, batch, count, taken, delivered, sentPending, serial >>

\* Apply: one block of the list the call read from the head, in list order
AckStep(t) ==
    /\ phase = "acks"
    /\ tc[t] # NoClient
    /\ tpos[t] <= Len(tlist[t])
    /\ SetWorld(ApplyIdx(World, tc[t], << tlist[t][tpos[t]] >>))
    /\ tpos' = [ tpos EXCEPT ![t] = @ + 1 ]
    /\ UNCHANGED << phase, updates, narr, nresets, touched, resetNow, mode, owner, tc, tlist, epoch, batch, count,
                    taken, delivered, sentPending, serial >>

\* the call ends: the list emptied (head = -1); then the message
\* layer takes the client's ids now when the slot is settled (settled: no
\* reset in the loop, one connection tag, no internal error, the slot bound
\* to it; the model leaves the rest to the environment) and none were taken
AckEnd(t) ==
    /\ phase = "acks"
    /\ tc[t] # NoClient
    /\ tpos[t] > Len(tlist[t])
    /\ LET c == tc[t]
           mine == SelectSeq(entries, LAMBDA e : e.c = c)
       IN /\ pending' = [ pending EXCEPT ![c] = << >> ]
          /\ \E settled \in BOOLEAN :
                IF settled /\ c \notin resetNow /\ mine # << >> /\ ( ~taken[c] \/ TakeTwice )
                THEN /\ taken' = [ taken EXCEPT ![c] = TRUE ]
                     /\ delivered' = [ delivered EXCEPT ![c] = @ \o [ i \in 1..Len(mine) |-> mine[i].res ] ]
                ELSE UNCHANGED << taken, delivered >>
    /\ tc' = [ tc EXCEPT ![t] = NoClient ]
    /\ UNCHANGED << phase, updates, narr, nresets, touched, resetNow, mode, entries, owner, tlist, tpos, hist, epoch,
                    alog, ccount, batch, count, sentPending, serial >>

Joined == \A t \in Threads : tc[t] = NoClient

\* update_end, after the join: the gather (rocketnet_server_protocol_ack_gather),
\* first thing in rocketnet_server_transport_update_finish
Gather ==
    /\ \/ phase = "acks" /\ ~SweepBeforeGather /\ Joined
       \/ phase = "swept"
    /\ phase' = IF phase = "swept" THEN "done" ELSE "gathered"
    /\ mode' = "inline"
    /\ LET g == GatherW(World)
       IN /\ SetWorld(g.w)
          /\ batch' = batch \o g.add
          /\ count' = count + g.sum
    /\ owner' = [ c \in Clients |-> NoOwner ]
    /\ UNCHANGED << updates, narr, nresets, touched, resetNow, tc, tlist, tpos, epoch, taken, delivered,
                    sentPending, serial >>

\* the maintenance sweep and protocol_update: keep-alives and standalone acks,
\* on every endpoint; the serial update makes the same sends after its loop
Sweep ==
    /\ \/ phase = "gathered"
       \/ phase = "acks" /\ SweepBeforeGather /\ Joined
    /\ phase' = IF phase = "gathered" THEN "done" ELSE "swept"
    /\ sentPending' = ( sentPending \/ AnyPending(pending, Clients) )
    /\ hist' = SendAll(hist)
    /\ shist' = SendAll(shist)
    /\ UNCHANGED << updates, narr, nresets, touched, resetNow, mode, entries, pending, owner, tc, tlist, tpos,
                    epoch, alog, ccount, batch, count, taken, delivered, sepoch, sbatch, scount, sdelivered >>

\* the message layer's drain (the back): the batch's ids of every client the
\* ack calls did not take; the marks cleared for the next update
RECURSIVE DrainFrom(_, _)
DrainFrom(d, b) ==
    IF b = << >> THEN d
    ELSE LET r == Head(b)
         IN DrainFrom(IF ~taken[r[1]] \/ DrainTaken THEN [ d EXCEPT ![r[1]] = Append(@, r[2]) ] ELSE d, Tail(b))

RECURSIVE SerialDrainFrom(_, _)
SerialDrainFrom(d, b) ==
    IF b = << >> THEN d ELSE SerialDrainFrom([ d EXCEPT ![Head(b)[1]] = Append(@, Head(b)[2]) ], Tail(b))

Drain ==
    /\ phase = "done"
    /\ phase' = "idle"
    /\ delivered' = DrainFrom(delivered, batch)
    /\ sdelivered' = SerialDrainFrom(sdelivered, sbatch)
    /\ taken' = [ c \in Clients |-> FALSE ]
    /\ UNCHANGED << updates, narr, nresets, touched, resetNow, mode, entries, pending, owner, tc, tlist, tpos,
                    hist, epoch, alog, ccount, batch, count, sentPending, shist, sepoch, sbatch, scount >>

Done ==
    /\ phase = "idle"
    /\ updates = MaxUpdates
    /\ UNCHANGED vars

Next ==
    \/ Begin
    \/ \E c \in Clients, k \in 0..1 : Defer(c, k) \/ Spill(c, k) \/ Inline(c, k)
    \/ \E c \in Clients : FirstKeepAlive(c) \/ ResetFlush(c)
    \/ OpenAcks
    \/ \E t \in Threads, c \in Clients : AckStart(t, c)
    \/ \E t \in Threads : AckStep(t) \/ AckEnd(t)
    \/ Gather
    \/ Sweep
    \/ Drain
    \/ Done

Spec == Init /\ [][Next]_vars

-----------------------------------------------------------------------------
(* The rules                                                               *)

TypeOK ==
    /\ phase \in { "idle", "receive", "acks", "swept", "gathered", "done" }
    /\ mode \in { "defer", "inline" }
    /\ owner \in [ Clients -> Threads \cup { NoOwner } ]
    /\ tc \in [ Threads -> Clients \cup { NoClient } ]
    /\ taken \in [ Clients -> BOOLEAN ]
    /\ count \in Nat /\ scount \in Nat
    /\ sentPending \in BOOLEAN

\* acks applied per client in arrival order: each client's blocks applied
\* once each, in the order they arrived
ArrivalOrder ==
    \A c \in Clients : \A i \in 1..Len(alog[c]) : \A j \in 1..Len(alog[c]) : i < j => alog[c][i] < alog[c][j]

\* the public acked-packets batch is the serial one: the same ids, clients and order
PublicOrder == phase = "idle" => batch = sbatch

\* the counters summed exactly
CountsExact == phase = "idle" => count = scount

\* every endpoint is the serial one
EndpointsMatch == phase = "idle" => hist = shist /\ epoch = sepoch

\* every id reaches the message layer once, in the client's batch order
MessageOnce == phase = "idle" => delivered = sdelivered

\* no send between a block's arrival and its apply: the only sends in the
\* update land on endpoints with nothing deferred
NoSendBeforeApply == ~sentPending

=============================================================================
