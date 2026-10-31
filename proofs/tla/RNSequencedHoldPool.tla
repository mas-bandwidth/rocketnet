------------------------ MODULE RNSequencedHoldPool ------------------------
(***************************************************************************)
(* The code-1 (unreliable sequenced) hold pool of one connection region as *)
(* laid out in sdk/rocketnet_allocator.cpp and used by                     *)
(* sdk/rocketnet_channel_sequenced.cpp, with the code's own arithmetic.    *)
(*                                                                         *)
(* Holds slots, each with a used state (0 free, 1 held, 2 delivered) and a *)
(* payload.  One index array of 2 * Holds entries: the free stack is its   *)
(* first Holds entries (sequenced_hold_free_stack, the top at              *)
(* sequenced_hold_free_count - 1) and the delivered list the next Holds    *)
(* (sequenced_holds_delivered = sequenced_hold_free_stack + holds,         *)
(* sequenced_holds_delivered_count): rn_region_layout gives them           *)
(* 2 * sizeof(uint32_t) per hold, back to back, so a push past Holds       *)
(* writes the delivered list's first entries.  Each channel's              *)
(* held_sequenced names its one future-state hold, or NONE.                *)
(*                                                                         *)
(*   Hold          rn_channels_dispatch_sequenced, a code-1 record ahead   *)
(*                 of its epoch and newer than the channel's hold: the old *)
(*                 hold is returned (rn_sequenced_release_slot), then the  *)
(*                 top of the free stack is taken, used = 1, and the       *)
(*                 payload copied in, or nothing is held when the stack is *)
(*                 empty (SEQUENCED_HOLD_FULL);                            *)
(*   DrainDeliver  rn_channels_drain_sequenced, the hold in turn and       *)
(*                 delivered: held_sequenced = NONE, used = 2, the slot    *)
(*                 appended to the delivered list; the view points at the  *)
(*                 slot's payload until the update ends;                   *)
(*   DrainRelease  rn_channels_drain_sequenced, the hold stale, refused or *)
(*                 without delivery room: returned to the free stack;      *)
(*   EndUpdate     rn_channels_end_update_sequenced: every delivered slot  *)
(*                 is returned, in list order, and the list emptied; the   *)
(*                 views expire;                                           *)
(*   Carve         rn_region_carve: the stack full, lowest index on top,   *)
(*                 no slot used, and (rn_channels_bind) no channel holding.*)
(* A return (rn_sequenced_hold_return) is used = 0 and                     *)
(* stack[free_count++] = slot.                                             *)
(*                                                                         *)
(* Abstracted: orderings and sequencing (RNChannelTypes); which of a       *)
(* newer record, a stale one or an in-turn one arrives is a free choice,   *)
(* and only the newer one ahead touches the pool.  The in-turn code-1      *)
(* record is delivered from the batch and never holds.                     *)
(*                                                                         *)
(* Reversed witnesses:                                                     *)
(*   DeliverClearsHold = FALSE  the delivery leaves held_sequenced naming  *)
(*                              the delivered slot: a later supersede      *)
(*                              returns it and the end of the update       *)
(*                              returns it again;                          *)
(*   DeferReturn = FALSE        the delivered slot is returned at once,    *)
(*                              not at the end of the update.              *)
(***************************************************************************)
EXTENDS Integers, FiniteSets

CONSTANTS
    Holds,              \* config->sequenced_messages_held
    C,                  \* channels
    DeliverClearsHold,  \* BOOLEAN
    DeferReturn         \* BOOLEAN

NONE == -1
Slots == 0..Holds-1
Chans == 0..C-1
Cells == 0..2*Holds-1          \* the free stack, then the delivered list

VARIABLES
    P,          \* the pool: [arr, fc, dc, used]
    held,       \* channel_receive[c].held_sequenced
    \* ghost
    views,      \* the slots whose delivered view is live this update
    overwrote,  \* a payload was written while its view was live
    badReturn,  \* a slot was returned that was free or still a channel's hold
    pastStack   \* a return pushed at free_count >= Holds, onto the delivered list

vars == <<P, held, views, overwrote, badReturn, pastStack>>

TypeOK ==
    /\ P \in [arr : [Cells -> Slots], fc : 0..2*Holds, dc : 0..Holds, used : [Slots -> {0, 1, 2}]]
    /\ held \in [Chans -> Slots \cup {NONE}]
    /\ views \subseteq Slots
    /\ overwrote \in BOOLEAN
    /\ badReturn \in BOOLEAN
    /\ pastStack \in BOOLEAN

Carved ==
    [arr  |-> [i \in Cells |-> IF i < Holds THEN Holds - 1 - i ELSE 0],
     fc   |-> Holds,
     dc   |-> 0,
     used |-> [s \in Slots |-> 0]]

Init ==
    /\ P = Carved
    /\ held = [c \in Chans |-> NONE]
    /\ views = {}
    /\ overwrote = FALSE
    /\ badReturn = FALSE
    /\ pastStack = FALSE

OnStack(p, s) == \E i \in 0..(p.fc - 1) : i < Holds /\ p.arr[i] = s

\* rn_sequenced_hold_return.  A push at fc >= 2 * Holds would
\* leave the region; the model keeps the array and records the fault.
Return(p, s) ==
    [p EXCEPT !.used[s] = 0,
              !.arr = IF p.fc < 2 * Holds THEN [@ EXCEPT ![p.fc] = s] ELSE @,
              !.fc = IF p.fc < 2 * Holds THEN @ + 1 ELSE @]

\* a return is wrong when the slot is already free or still some channel's hold
BadReturn(p, h, s) ==
    \/ OnStack(p, s)
    \/ \E c \in Chans : h[c] = s

\* rn_channels_end_update_sequenced: return delivered[i] for
\* i in 0 .. dc-1, each read from the array as the loop reaches it
RECURSIVE EndLoop(_, _, _, _)
EndLoop(p, i, bad, past) ==
    IF i >= p.dc THEN <<p, bad, past>>
    ELSE LET s == p.arr[Holds + i]
         IN  EndLoop(Return(p, s), i + 1, bad \/ BadReturn(p, held, s) \/ p.used[s] # 2, past \/ p.fc >= Holds)

\* rn_channels_dispatch_sequenced: a newer future record
Hold(c) ==
    LET old == held[c]
        p1  == IF old # NONE THEN Return(P, old) ELSE P
        bad == old # NONE /\ BadReturn(P, [held EXCEPT ![c] = NONE], old)
    IN
    /\ badReturn' = (badReturn \/ bad)
    /\ pastStack' = (pastStack \/ (old # NONE /\ P.fc >= Holds))
    /\ IF p1.fc = 0
         THEN /\ P' = p1                                        \* SEQUENCED_HOLD_FULL
              /\ held' = [held EXCEPT ![c] = NONE]
              /\ UNCHANGED overwrote
         ELSE LET s == p1.arr[p1.fc - 1] IN
              /\ P' = [p1 EXCEPT !.fc = @ - 1, !.used[s] = 1]
              /\ held' = [held EXCEPT ![c] = s]
              /\ overwrote' = (overwrote \/ s \in views)       \* the memcpy into the payload
    /\ UNCHANGED views

\* rn_channels_drain_sequenced: the hold delivered
DrainDeliver(c) ==
    /\ held[c] # NONE
    /\ LET s == held[c] IN
       IF DeferReturn
         THEN /\ P' = [P EXCEPT !.used[s] = 2,
                                !.arr = IF Holds + P.dc < 2 * Holds THEN [@ EXCEPT ![Holds + P.dc] = s] ELSE @,
                                !.dc = IF P.dc < Holds THEN @ + 1 ELSE @]
              /\ badReturn' = (badReturn \/ P.dc >= Holds)
              /\ UNCHANGED pastStack
         ELSE /\ P' = Return(P, s)
              /\ badReturn' = (badReturn \/ BadReturn(P, [held EXCEPT ![c] = NONE], s))
              /\ pastStack' = (pastStack \/ P.fc >= Holds)
    /\ held' = IF DeliverClearsHold THEN [held EXCEPT ![c] = NONE] ELSE held
    /\ views' = views \cup {held[c]}
    /\ UNCHANGED overwrote

\* rn_channels_drain_sequenced: the hold stale, refused or without room
DrainRelease(c) ==
    /\ held[c] # NONE
    /\ P' = Return(P, held[c])
    /\ badReturn' = (badReturn \/ BadReturn(P, [held EXCEPT ![c] = NONE], held[c]))
    /\ pastStack' = (pastStack \/ P.fc >= Holds)
    /\ held' = [held EXCEPT ![c] = NONE]
    /\ UNCHANGED <<views, overwrote>>

EndUpdate ==
    LET r == EndLoop(P, 0, FALSE, FALSE) IN
    /\ P' = [r[1] EXCEPT !.dc = 0]
    /\ badReturn' = (badReturn \/ r[2])
    /\ pastStack' = (pastStack \/ r[3])
    /\ views' = {}
    /\ UNCHANGED <<held, overwrote>>

\* rn_region_carve, and rn_channels_bind's empty channels
Carve ==
    /\ P' = Carved
    /\ held' = [c \in Chans |-> NONE]
    /\ views' = {}
    /\ UNCHANGED <<overwrote, badReturn, pastStack>>

Next ==
    \/ \E c \in Chans : Hold(c)
    \/ \E c \in Chans : DrainDeliver(c)
    \/ \E c \in Chans : DrainRelease(c)
    \/ EndUpdate
    \/ Carve

Spec == Init /\ [][Next]_vars

(***************************************************************************)
(* Invariants.                                                             *)
(***************************************************************************)

\* no push past the stack's Holds entries onto the delivered list
StackInBounds == ~pastStack /\ P.fc <= Holds

\* no return of a free or held slot; the stack holds distinct free slots
FreeStackExact ==
    /\ ~badReturn
    /\ StackInBounds
    /\ \A i, j \in 0..(P.fc - 1) : i # j => P.arr[i] # P.arr[j]
    /\ \A i \in 0..(P.fc - 1) : P.used[P.arr[i]] = 0

\* every slot is in exactly one place: the free stack, one channel's hold,
\* or the delivered list
PoolPartition ==
    /\ P.fc + Cardinality({c \in Chans : held[c] # NONE}) + P.dc = Holds
    /\ \A c \in Chans : held[c] # NONE => P.used[held[c]] = 1
    /\ \A c, e \in Chans : c # e /\ held[c] # NONE => held[c] # held[e]
    /\ \A i \in 0..(P.dc - 1) : P.used[P.arr[Holds + i]] = 2
    /\ {P.arr[Holds + i] : i \in 0..(P.dc - 1)} = views

\* a delivered view's payload is never written while the view is live
NoPayloadOverwrite == ~overwrote
=============================================================================
