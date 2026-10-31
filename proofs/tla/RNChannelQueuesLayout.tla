------------------------ MODULE RNChannelQueuesLayout ------------------------
(***************************************************************************)
(* The send side's channel FIFOs and ready list as laid out in             *)
(* sdk/rocketnet_channel.cpp, with the code's own links.                   *)
(*                                                                         *)
(* Each channel's reliable FIFO is intrusive: queue_head and queue_tail in *)
(* its rn_channel_send_t, queue_next in each send descriptor               *)
(* (rn_channel_queue_append, rn_channel_queue_push_front,                  *)
(* rn_channel_queue_pop).  The ready list is doubly linked by channel      *)
(* index through ready_prev and ready_next, with ready_head and ready_tail *)
(* in rn_channels_t; a member's name is its ready_ticket (never 0, from    *)
(* ready_ticket_next, which skips 0 on a wrap) and the head generation it  *)
(* captured (rn_ready_link, rn_ready_remove_named).  The callers:          *)
(*   Enqueue    rn_channels_finish: take a descriptor, stamp it, count it, *)
(*              append it, and push the channel at the back if it had      *)
(*              nothing queued;                                            *)
(*   Config     rn_ordered_announce_config: CONFIG at the head of channel  *)
(*              0 and channel 0 at the head of the ready list, once;       *)
(*   WalkEmpty  rn_ordered_write: a ready channel with nothing queued      *)
(*              leaves the turns;                                          *)
(*   Number     rn_ordered_write: the head channel's FIFO head is          *)
(*              numbered: the channel leaves the list under the name it    *)
(*              joined with, the head is popped, and the channel rejoins   *)
(*              the back if it has more.                                   *)
(* Free is the environment: the allocator frees a numbered descriptor when *)
(* its number is acked (rn_region_send_free; RNSendStoreLayout models the  *)
(* free stack), and a later take reuses it, clearing it whole (queue_next  *)
(* reads 0, descriptor 0's index, until the stamp sets it to NONE) and     *)
(* bumping its generation, which skips 0 on a wrap.                        *)
(*                                                                         *)
(* The ghost state is the abstract one of RNChannelQueues.tla: each        *)
(* channel's FIFO and the ready list as sequences.  The invariants check   *)
(* that the links walk exactly those sequences.                            *)
(*                                                                         *)
(* Reversed witnesses, each a line of the code removed:                    *)
(*   PopResetsTail = FALSE      pop of the last entry leaves queue_tail;   *)
(*   PushFrontSetsTail = FALSE  push_front on an empty FIFO leaves the     *)
(*                              tail NONE;                                 *)
(*   RemoveUpdatesEnds = FALSE  removing the ready head or tail leaves     *)
(*                              ready_head or ready_tail naming it.        *)
(***************************************************************************)
EXTENDS Integers, Sequences, FiniteSets

CONSTANTS
    D,                  \* send descriptors
    C,                  \* channels
    TicketMod,          \* ready_ticket_next wraps here (2^32 in the code), skipping 0
    GenMax,             \* a descriptor generation wraps after GenMax to 1 (UINT32_MAX in the code)
    PopResetsTail,      \* BOOLEAN
    PushFrontSetsTail,  \* BOOLEAN
    RemoveUpdatesEnds   \* BOOLEAN

NONE == -1
Descs == 0..D-1
Chans == 0..C-1

VARIABLES
    L,          \* the layout: every field the code reads or writes
    dstate,     \* each descriptor: "free", "queued" or "out" (numbered, awaiting its ack)
    configDone, \* ordered->config_announced
    \* ghost
    q,          \* channel -> the FIFO as a sequence of descriptors
    ready,      \* the ready list as a sequence of channels
    foreign     \* a link was written into a descriptor not queued on that channel

vars == <<L, dstate, configDone, q, ready, foreign>>

LayoutType ==
    [qnext   : [Descs -> Descs \cup {NONE}],
     gen     : [Descs -> 0..GenMax],
     qhead   : [Chans -> Descs \cup {NONE}],
     qtail   : [Chans -> Descs \cup {NONE}],
     queued  : [Chans -> 0..D],
     rhead   : Chans \cup {NONE},
     rtail   : Chans \cup {NONE},
     rprev   : [Chans -> Chans \cup {NONE}],
     rnext   : [Chans -> Chans \cup {NONE}],
     rticket : [Chans -> 0..TicketMod-1],
     rgen    : [Chans -> 0..GenMax],
     tnext   : 1..TicketMod-1]

TypeOK ==
    /\ L \in LayoutType
    /\ dstate \in [Descs -> {"free", "queued", "out"}]
    /\ configDone \in BOOLEAN
    /\ q \in [Chans -> Seq(Descs)]
    /\ ready \in Seq(Chans)
    /\ foreign \in BOOLEAN

Init ==
    /\ L = [qnext   |-> [d \in Descs |-> NONE],
            gen     |-> [d \in Descs |-> 0],
            qhead   |-> [c \in Chans |-> NONE],
            qtail   |-> [c \in Chans |-> NONE],
            queued  |-> [c \in Chans |-> 0],
            rhead   |-> NONE,
            rtail   |-> NONE,
            rprev   |-> [c \in Chans |-> NONE],
            rnext   |-> [c \in Chans |-> NONE],
            rticket |-> [c \in Chans |-> 0],
            rgen    |-> [c \in Chans |-> 0],
            tnext   |-> 1]
    /\ dstate = [d \in Descs |-> "free"]
    /\ configDone = FALSE
    /\ q = [c \in Chans |-> <<>>]
    /\ ready = <<>>
    /\ foreign = FALSE

(***************************************************************************)
(* The code's functions, each a map from layout to layout.                 *)
(***************************************************************************)

\* rn_region_descriptor_take: cleared whole (queue_next 0), the next generation;
\* rn_descriptor_enqueue_stamp: queue_next NONE
Take(s, d) ==
    [s EXCEPT !.gen[d] = IF @ = 0 \/ @ = GenMax THEN 1 ELSE @ + 1,
              !.qnext[d] = NONE]

\* rn_channel_queue_append
QAppend(s, c, d) ==
    LET s1 == [s EXCEPT !.qnext[d] = NONE]
        s2 == IF s1.qtail[c] # NONE
                THEN [s1 EXCEPT !.qnext[s1.qtail[c]] = d]
                ELSE [s1 EXCEPT !.qhead[c] = d]
    IN  [s2 EXCEPT !.qtail[c] = d]

\* rn_channel_queue_push_front
PushFront(s, c, d) ==
    LET s1 == [s EXCEPT !.qnext[d] = s.qhead[c], !.qhead[c] = d]
    IN  IF PushFrontSetsTail /\ s1.qtail[c] = NONE
          THEN [s1 EXCEPT !.qtail[c] = d]
          ELSE s1

\* rn_channel_queue_pop
Pop(s, c) ==
    LET d  == s.qhead[c]
        s1 == [s EXCEPT !.qhead[c] = s.qnext[d]]
        s2 == [s1 EXCEPT !.qnext[d] = NONE]
    IN  IF PopResetsTail /\ s2.qhead[c] = NONE
          THEN [s2 EXCEPT !.qtail[c] = NONE]
          ELSE s2

\* rn_channel_head_generation
HeadGen(s, c) == IF s.qhead[c] = NONE THEN 0 ELSE s.gen[s.qhead[c]]

\* rn_ready_link
Link(s, c, front) ==
    IF s.rticket[c] # 0 THEN s ELSE
    LET t   == s.tnext
        nx  == (t + 1) % TicketMod
        s1  == [s EXCEPT !.tnext = IF nx = 0 THEN 1 ELSE nx,
                         !.rticket[c] = t,
                         !.rgen[c] = HeadGen(s, c)]
    IN  IF front
          THEN LET s2 == [s1 EXCEPT !.rprev[c] = NONE, !.rnext[c] = s1.rhead]
                   s3 == IF s2.rhead # NONE
                           THEN [s2 EXCEPT !.rprev[s2.rhead] = c]
                           ELSE [s2 EXCEPT !.rtail = c]
               IN  [s3 EXCEPT !.rhead = c]
          ELSE LET s2 == [s1 EXCEPT !.rnext[c] = NONE, !.rprev[c] = s1.rtail]
                   s3 == IF s2.rtail # NONE
                           THEN [s2 EXCEPT !.rnext[s2.rtail] = c]
                           ELSE [s2 EXCEPT !.rhead = c]
               IN  [s3 EXCEPT !.rtail = c]

\* rn_ready_remove_named: the test, then the unlink
Named(s, c, ticket, g) == ticket # 0 /\ s.rticket[c] = ticket /\ s.rgen[c] = g

Unlink(s, c) ==
    LET prev == s.rprev[c]
        next == s.rnext[c]
        s1 == IF prev # NONE THEN [s EXCEPT !.rnext[prev] = next]
              ELSE IF RemoveUpdatesEnds THEN [s EXCEPT !.rhead = next] ELSE s
        s2 == IF next # NONE THEN [s1 EXCEPT !.rprev[next] = prev]
              ELSE IF RemoveUpdatesEnds THEN [s1 EXCEPT !.rtail = prev] ELSE s1
    IN  [s2 EXCEPT !.rprev[c] = NONE, !.rnext[c] = NONE, !.rticket[c] = 0, !.rgen[c] = 0]

RemoveNamed(s, c, ticket, g) == IF Named(s, c, ticket, g) THEN Unlink(s, c) ELSE s

\* the ghost ready list
RemoveSeq(sq, c) == SelectSeq(sq, LAMBDA x : x # c)
InSeq(sq, x) == \E i \in 1..Len(sq) : sq[i] = x
GhostLink(sq, c, front) ==
    IF InSeq(sq, c) THEN sq ELSE IF front THEN <<c>> \o sq ELSE Append(sq, c)

\* the append writes the tail's queue_next: it must be this channel's last entry
WritesForeign(s, c) ==
    s.qtail[c] # NONE /\ (q[c] = <<>> \/ s.qtail[c] # q[c][Len(q[c])])

(***************************************************************************)
(* The actions.                                                            *)
(***************************************************************************)

\* rn_channels_finish
Enqueue(c, d) ==
    /\ dstate[d] = "free"
    /\ LET s1 == Take(L, d)
           wasEmpty == s1.queued[c] = 0
           s2 == [s1 EXCEPT !.queued[c] = @ + 1]
           s3 == QAppend(s2, c, d)
       IN  /\ L' = IF wasEmpty THEN Link(s3, c, FALSE) ELSE s3
           /\ foreign' = (foreign \/ WritesForeign(s2, c))
           /\ ready' = IF wasEmpty THEN GhostLink(ready, c, FALSE) ELSE ready
    /\ dstate' = [dstate EXCEPT ![d] = "queued"]
    /\ q' = [q EXCEPT ![c] = Append(@, d)]
    /\ UNCHANGED configDone

\* rn_ordered_announce_config: channel 0
Config(d) ==
    /\ ~configDone
    /\ dstate[d] = "free"
    /\ LET s1 == Take(L, d)
           s2 == [s1 EXCEPT !.queued[0] = @ + 1]
           s3 == PushFront(s2, 0, d)
           s4 == IF s3.rticket[0] # 0 THEN RemoveNamed(s3, 0, s3.rticket[0], s3.rgen[0]) ELSE s3
       IN  L' = Link(s4, 0, TRUE)
    /\ dstate' = [dstate EXCEPT ![d] = "queued"]
    /\ configDone' = TRUE
    /\ q' = [q EXCEPT ![0] = <<d>> \o @]
    /\ ready' = <<0>> \o RemoveSeq(ready, 0)
    /\ UNCHANGED foreign

\* rn_ordered_write: a ready channel with nothing queued leaves
WalkEmpty ==
    /\ L.rhead # NONE
    /\ L.qhead[L.rhead] = NONE
    /\ LET c == L.rhead IN
       /\ L' = RemoveNamed(L, c, L.rticket[c], L.rgen[c])
       /\ ready' = RemoveSeq(ready, c)
    /\ UNCHANGED <<dstate, configDone, q, foreign>>

\* rn_ordered_write: the head channel's FIFO head is numbered
Number ==
    /\ L.rhead # NONE
    /\ L.qhead[L.rhead] # NONE
    /\ LET c  == L.rhead
           d  == L.qhead[c]
           tk == L.rticket[c]
           g  == L.rgen[c]
           s1 == IF Named(L, c, tk, g) THEN Unlink(L, c)
                 ELSE IF L.rticket[c] # 0 THEN RemoveNamed(L, c, L.rticket[c], L.rgen[c]) ELSE L
           s2 == [s1 EXCEPT !.queued[c] = IF @ > 0 THEN @ - 1 ELSE 0]
           s3 == Pop(s2, c)
           more == s3.qhead[c] # NONE
       IN  /\ L' = IF more THEN Link(s3, c, FALSE) ELSE s3
           /\ dstate' = [dstate EXCEPT ![d] = "out"]
           /\ q' = [q EXCEPT ![c] = Tail(@)]
           /\ ready' = IF more THEN Append(RemoveSeq(ready, c), c) ELSE RemoveSeq(ready, c)
           \* the descriptor popped must be the FIFO's first
           /\ foreign' = (foreign \/ q[c] = <<>> \/ q[c][1] # d)
    /\ UNCHANGED configDone

\* the environment: the allocator frees a numbered descriptor on its ack
Free(d) ==
    /\ dstate[d] = "out"
    /\ dstate' = [dstate EXCEPT ![d] = "free"]
    /\ UNCHANGED <<L, configDone, q, ready, foreign>>

Next ==
    \/ \E c \in Chans, d \in Descs : Enqueue(c, d)
    \/ \E d \in Descs : Config(d)
    \/ WalkEmpty
    \/ Number
    \/ \E d \in Descs : Free(d)

Spec == Init /\ [][Next]_vars

(***************************************************************************)
(* Invariants.                                                             *)
(***************************************************************************)

RECURSIVE Walk(_, _, _)
Walk(nx, d, fuel) == IF d = NONE \/ fuel = 0 THEN <<>> ELSE <<d>> \o Walk(nx, nx[d], fuel - 1)

Reverse(sq) == [i \in 1..Len(sq) |-> sq[Len(sq) + 1 - i]]

\* each channel's links walk exactly its FIFO, from head to tail
QueueIsList ==
    \A c \in Chans :
        /\ Walk(L.qnext, L.qhead[c], D + 1) = q[c]
        /\ L.qtail[c] = (IF q[c] = <<>> THEN NONE ELSE q[c][Len(q[c])])
        /\ L.queued[c] = Len(q[c])

\* no link is ever written into a descriptor another FIFO or the allocator owns
NoForeignLink == ~foreign

\* the ready links walk exactly the ready list, forwards and backwards
ReadyIsList ==
    /\ Walk(L.rnext, L.rhead, C + 1) = ready
    /\ Walk(L.rprev, L.rtail, C + 1) = Reverse(ready)

\* membership is the ticket: a member holds a nonzero ticket and its head's
\* generation; a nonmember has no ticket and no links
MembershipByTicket ==
    \A c \in Chans :
        IF InSeq(ready, c)
          THEN L.rticket[c] # 0
          ELSE L.rticket[c] = 0 /\ L.rprev[c] = NONE /\ L.rnext[c] = NONE

\* a channel with work queued takes turns
NonEmptyIsReady == \A c \in Chans : q[c] # <<>> => InSeq(ready, c)

\* a queued descriptor sits in exactly one FIFO
QueuedOwned ==
    \A d \in Descs :
        (dstate[d] = "queued") <=>
            (Cardinality({<<c, i>> \in Chans \X (1..D) : i <= Len(q[c]) /\ q[c][i] = d}) = 1)
=============================================================================
