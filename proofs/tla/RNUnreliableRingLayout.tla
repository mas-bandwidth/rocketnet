----------------------- MODULE RNUnreliableRingLayout -----------------------
(***************************************************************************)
(* The endpoint's unreliable queue as sdk/rocketnet_endpoint.cpp lays it  *)
(* out, with the enqueue tickets of sdk/rocketnet_allocator.h.            *)
(* RNIndexRingArray carries the same ring (array, head, count, positions  *)
(* modulo N, in-place compaction) with tickets that never wrap; this      *)
(* module adds what the endpoint's ring has beyond it:                    *)
(*                                                                         *)
(*   - tickets from one per-region counter, region->schedule_ticket_next, *)
(*     modulo TicketMod (2^32 in the code) and skipping 0, shared with    *)
(*     the reliable enqueues (DrawOther), with the wrap flag               *)
(*     schedule_ticket_wrapped (rn_region_schedule_ticket);                *)
(*   - the renumber at the first push after a wrap                        *)
(*     (rn_unreliable_renumber): every tombstone and every entry naming   *)
(*     the descriptor being pushed dropped, the survivors closed up from  *)
(*     the head and each given a fresh ticket in queue order, then the    *)
(*     pushed descriptor's ticket drawn last;                              *)
(*   - the code's eager head sync after every push and every unlink       *)
(*     (rn_unreliable_sync);                                               *)
(*   - the walk (rn_unreliable_walk_begin, rn_unreliable_walk_next): the  *)
(*     head and count as they stood at its start, entries read in place,  *)
(*     unlinks (and their syncs) between its steps and no push.           *)
(*                                                                         *)
(* An entry is live when its descriptor is queued under the entry's       *)
(* ticket (rn_unreliable_entry_live); ticket[d] = 0 is a descriptor not   *)
(* queued (its enqueue_ticket after rn_endpoint_unreliable_unlink).  A    *)
(* push and the loops inside it run with nothing interleaved, so Push is  *)
(* one action; its loops read the array as they rewrite it, in place.     *)
(*                                                                         *)
(* Reversed witnesses (each flag FALSE is a bug class):                    *)
(*   Renumber      no renumber after a wrap                                *)
(*   FreshTickets  the renumber drops tombstones but keeps the survivors' *)
(*                 tickets from the last lap                               *)
(*   DropPushed    the renumber keeps entries naming the pushed           *)
(*                 descriptor (a stale one revived by its new stamp)      *)
(*   WalkSnapshot  the walk reads from the current head instead of the    *)
(*                 head captured at its start: after an unlink's sync     *)
(*                 moves the head, the walk re-reads entries past the     *)
(*                 live range and returns one descriptor twice            *)
(***************************************************************************)
EXTENDS Integers, Sequences, FiniteSets

CONSTANTS
    Q,              \* send descriptors that can queue unreliable records
    N,              \* ring entries (2 * Q in the code)
    TicketMod,      \* the counter runs 1..TicketMod-1 (2^32 in the code)
    Renumber,       \* rn_unreliable_push renumbers after a wrap
    FreshTickets,   \* the renumber draws a fresh ticket for each survivor
    DropPushed,     \* the renumber drops every entry naming the pushed descriptor
    WalkSnapshot    \* the walk reads from the head captured at WalkBegin

ASSUME Q \in Nat \ {0} /\ N >= Q + 1 /\ TicketMod >= 3
ASSUME \A f \in {Renumber, FreshTickets, DropPushed, WalkSnapshot} : f \in BOOLEAN

Descs == 1..Q
Pos == 0..(N - 1)
Empty == [ slot |-> 0, t |-> 0 ]

VARIABLES
    ticket,     \* ticket[d]: descriptor d's enqueue_ticket, 0 when not queued
    next,       \* region->schedule_ticket_next
    wrapped,    \* region->schedule_ticket_wrapped
    arr,        \* region->unreliable_ring
    head,       \* endpoint->unreliable_ring_head
    count,      \* endpoint->unreliable_ring_used
    order,      \* ghost: the queued descriptors in push order
    wpc,        \* "idle", or "walk" while a walk is open
    wbase,      \* walk->base
    wspan,      \* walk->span
    wstep,      \* walk->step
    worder,     \* ghost: order when the walk began
    wseen       \* ghost: the descriptors the walk has returned

vars == << ticket, next, wrapped, arr, head, count, order, wpc, wbase, wspan, wstep, worder, wseen >>

Phys(h, k) == (h + k) % N
Current(tk, e) == e.slot # 0 /\ tk[e.slot] # 0 /\ tk[e.slot] = e.t
Remove(q, d) == SelectSeq(q, LAMBDA x : x # d)
InSeq(q, d) == \E k \in 1..Len(q) : q[k] = d
IndexOf(q, d) == CHOOSE k \in 1..Len(q) : q[k] = d

-----------------------------------------------------------------------------
(* The code's loops, on a state record                                    *)
(* st = [ticket, next, wrapped, arr, head, count].                         *)

\* rn_region_schedule_ticket: the ticket, then next + 1, 0 skipped and the wrap noted
DrawT(st) ==
    LET n == st.next + 1 IN
    [ st EXCEPT !.next = IF n = TicketMod THEN 1 ELSE n,
                !.wrapped = st.wrapped \/ n = TicketMod ]

\* rn_unreliable_renumber, the loop: entry i read in place at head + i,
\* survivors written at head + kept
RECURSIVE RenumberLoop(_, _, _, _)
RenumberLoop(st, d, i, kept) ==
    IF i >= st.count THEN [ st EXCEPT !.count = kept ]
    ELSE LET e == st.arr[Phys(st.head, i)]
             drop == (DropPushed /\ e.slot = d) \/ ~Current(st.ticket, e)
         IN IF drop THEN RenumberLoop(st, d, i + 1, kept)
            ELSE LET t == IF FreshTickets THEN st.next ELSE e.t
                     s1 == IF FreshTickets THEN DrawT(st) ELSE st
                     e1 == [ e EXCEPT !.t = t ]
                     s2 == [ s1 EXCEPT !.ticket = [ s1.ticket EXCEPT ![e.slot] = t ],
                                       !.arr = [ s1.arr EXCEPT ![Phys(st.head, kept)] = e1 ] ]
                 IN RenumberLoop(s2, d, i + 1, kept + 1)

\* rn_unreliable_renumber: the flag cleared, the loop, then d's ticket drawn
RenumberL(st, d) ==
    LET s1 == RenumberLoop([ st EXCEPT !.wrapped = FALSE ], d, 0, 0)
    IN [ DrawT(s1) EXCEPT !.ticket = [ s1.ticket EXCEPT ![d] = s1.next ] ]

\* rn_unreliable_push, the full-ring pass: live entries closed up from the head
RECURSIVE CompactLoop(_, _, _)
CompactLoop(st, i, kept) ==
    IF i >= st.count THEN [ st EXCEPT !.count = kept ]
    ELSE LET e == st.arr[Phys(st.head, i)] IN
         IF Current(st.ticket, e)
         THEN CompactLoop([ st EXCEPT !.arr = [ st.arr EXCEPT ![Phys(st.head, kept)] = e ] ], i + 1, kept + 1)
         ELSE CompactLoop(st, i + 1, kept)

\* rn_unreliable_sync: the head skips tombstones
RECURSIVE SyncL(_)
SyncL(st) ==
    IF st.count > 0 /\ ~Current(st.ticket, st.arr[st.head])
    THEN SyncL([ st EXCEPT !.head = (st.head + 1) % N, !.count = st.count - 1 ])
    ELSE st

\* rn_endpoint_finish's rn_descriptor_enqueue_stamp, then rn_unreliable_push
PushL(st, d) ==
    LET s0 == [ DrawT(st) EXCEPT !.ticket = [ st.ticket EXCEPT ![d] = st.next ] ]
        s1 == IF Renumber /\ s0.wrapped THEN RenumberL(s0, d) ELSE s0
        s2 == IF s1.count = N THEN CompactLoop(s1, 0, 0) ELSE s1
        s3 == [ s2 EXCEPT !.arr = [ s2.arr EXCEPT ![Phys(s2.head, s2.count)] = [ slot |-> d, t |-> s2.ticket[d] ] ],
                          !.count = s2.count + 1 ]
    IN SyncL(s3)

St == [ ticket |-> ticket, next |-> next, wrapped |-> wrapped, arr |-> arr, head |-> head, count |-> count ]

SetSt(st) ==
    /\ ticket' = st.ticket /\ next' = st.next /\ wrapped' = st.wrapped
    /\ arr' = st.arr /\ head' = st.head /\ count' = st.count

-----------------------------------------------------------------------------

Init ==
    /\ ticket = [ d \in Descs |-> 0 ]
    /\ next = 1
    /\ wrapped = FALSE
    /\ arr = [ k \in Pos |-> Empty ]
    /\ head = 0
    /\ count = 0
    /\ order = << >>
    /\ wpc = "idle"
    /\ wbase = 0 /\ wspan = 0 /\ wstep = 0
    /\ worder = << >>
    /\ wseen = << >>

\* an unreliable send of a free descriptor: rn_endpoint_finish stamps it and
\* pushes it
Push(d) ==
    /\ wpc = "idle"
    /\ ticket[d] = 0
    /\ SetSt(PushL(St, d))
    /\ order' = Append(order, d)
    /\ UNCHANGED << wpc, wbase, wspan, wstep, worder, wseen >>

\* rn_endpoint_unreliable_unlink: the descriptor leaves (enqueue_ticket 0,
\* freed), then the sync; also between a walk's steps (the taken records of
\* rn_endpoint_write_packet)
Unlink(d) ==
    /\ ticket[d] # 0
    /\ SetSt(SyncL([ St EXCEPT !.ticket = [ ticket EXCEPT ![d] = 0 ] ]))
    /\ order' = Remove(order, d)
    /\ UNCHANGED << wpc, wbase, wspan, wstep, worder, wseen >>

\* the environment: a reliable enqueue draws from the shared counter
DrawOther ==
    /\ wpc = "idle"
    /\ LET st == DrawT(St) IN /\ next' = st.next /\ wrapped' = st.wrapped
    /\ UNCHANGED << ticket, arr, head, count, order, wpc, wbase, wspan, wstep, worder, wseen >>

\* rn_unreliable_walk_begin
WalkBegin ==
    /\ wpc = "idle"
    /\ wpc' = "walk"
    /\ wbase' = head /\ wspan' = count /\ wstep' = 0
    /\ worder' = order
    /\ wseen' = << >>
    /\ UNCHANGED << ticket, next, wrapped, arr, head, count, order >>

\* rn_unreliable_walk_next: the next live entry from the step, or the end.
\* The code reads at walk->base, the head captured by WalkBegin; the
\* WalkSnapshot = FALSE witness reads at the current head instead, which
\* an unlink's sync may have moved since the walk began.
WalkBase == IF WalkSnapshot THEN wbase ELSE head

RECURSIVE NextLive(_)
NextLive(k) ==
    IF k >= wspan THEN wspan
    ELSE IF Current(ticket, arr[Phys(WalkBase, k)]) THEN k ELSE NextLive(k + 1)

WalkNext ==
    /\ wpc = "walk"
    /\ LET k == NextLive(wstep) IN
       IF k = wspan
       THEN /\ wpc' = "idle" /\ wstep' = wspan /\ UNCHANGED wseen
       ELSE /\ wseen' = Append(wseen, arr[Phys(WalkBase, k)].slot)
            /\ wstep' = k + 1
            /\ UNCHANGED wpc
    /\ UNCHANGED << ticket, next, wrapped, arr, head, count, order, wbase, wspan, worder >>

\* the caller stops the walk early (a full packet, a large record)
WalkStop ==
    /\ wpc = "walk"
    /\ wpc' = "idle"
    /\ UNCHANGED << ticket, next, wrapped, arr, head, count, order, wbase, wspan, wstep, worder, wseen >>

Next ==
    \/ \E d \in Descs : Push(d) \/ Unlink(d)
    \/ DrawOther
    \/ WalkBegin
    \/ WalkNext
    \/ WalkStop

Spec == Init /\ [][Next]_vars

-----------------------------------------------------------------------------

Logical == [ k \in 1..count |-> arr[Phys(head, k - 1)] ]
SlotsOf(q) == [ k \in 1..Len(q) |-> q[k].slot ]

TypeOK ==
    /\ ticket \in [ Descs -> 0..(TicketMod - 1) ]
    /\ next \in 1..(TicketMod - 1)
    /\ wrapped \in BOOLEAN
    /\ head \in Pos
    /\ count \in 0..N
    /\ wpc \in {"idle", "walk"}

\* the live entries from the head, in order, are exactly the queue
LiveIsOrder == SlotsOf(SelectSeq(Logical, LAMBDA e : Current(ticket, e))) = order

\* the head entry is live whenever the ring is not empty (the eager sync)
HeadSynced == count > 0 => Current(ticket, arr[head])

\* a walk returns each queued descriptor at most once, in queue order, and
\* only descriptors queued when it began
WalkExact ==
    /\ \A i \in 1..Len(wseen) : InSeq(worder, wseen[i])
    /\ \A i, j \in 1..Len(wseen) : i < j => IndexOf(worder, wseen[i]) < IndexOf(worder, wseen[j])

-----------------------------------------------------------------------------

(* The view for the control.  A ring cell outside [head, head + count) is  *)
(* dead unless an open walk's span covers it: every push and every pass    *)
(* writes such a cell before it reads it, and only a walk reads past the   *)
(* head, over cells no push can reach while it is open.  A closed walk's   *)
(* fields are dead too: WalkBegin writes them all.  The view leaves dead   *)
(* values out, so states that differ only in them are one state.           *)
InLive(k) == \E i \in 0..(count - 1) : Phys(head, i) = k
InWalk(k) == wpc = "walk" /\ \E i \in 0..(wspan - 1) : Phys(WalkBase, i) = k
View ==
    << ticket, next, wrapped,
       [ k \in Pos |-> IF InLive(k) \/ InWalk(k) THEN arr[k] ELSE Empty ],
       head, count, order, wpc,
       IF wpc = "walk" THEN << wbase, wspan, wstep, worder, wseen >> ELSE << >> >>
=============================================================================
