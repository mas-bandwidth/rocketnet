------------------------- MODULE RNResendFifoLayout -------------------------
(***************************************************************************)
(* The resend FIFO as sdk/rocketnet_window_ack.cpp lays it out: two        *)
(* intrusive doubly linked sequences, small records and block chunks,      *)
(* threaded through the one array region->resend_fifo of W entries, each   *)
(* entry the slot of message number n & (W - 1).  RNResendTiming and       *)
(* RNResendGenerations check the sequences and the names abstractly; this *)
(* checks the mapping from those sequences to the array, which the         *)
(* sequence models cannot see.                                             *)
(*                                                                         *)
(* An entry holds two 32-bit fields.  prev: bit 31 is the linked flag      *)
(* (RN_RESEND_LINKED), the low 31 bits the previous slot.  next: bit 31 is *)
(* the class (RN_RESEND_CHUNK), the low 31 bits the next slot.  The end   *)
(* marker 0x7FFFFFFF (RN_RESEND_END) reads as RN_RESEND_NONE.  Here a      *)
(* field is a record of its flag bit and its index, and the index W is    *)
(* the end marker and NONE alike (rn_resend_index, rn_resend_field).  The  *)
(* heads and tails are ack->resend_head / resend_tail ("s") and            *)
(* chunk_head / chunk_tail ("c"); cnt is ack->resend_count.                *)
(*                                                                         *)
(* A slot's class at the time it is linked is rn_window_ack_slot_is_chunk, *)
(* read off its descriptor; the descriptor can be freed, or attached,      *)
(* while the slot is linked, so the environment changes isChunk[s] at any  *)
(* time (SetClass).  The class bit stored at link time is what the unlink  *)
(* follows.                                                                *)
(*                                                                         *)
(* The ghost seq[c] is the abstract sequence of each class, updated by     *)
(* each operation's contract.  ListsMatch says the array, the heads, the   *)
(* tails, the flag bits and the count are exactly those two sequences.     *)
(*                                                                         *)
(* The sent mark.  Each sequence is its numbers never sent, then its       *)
(* numbers on the wire: sent, or a resend the transport refused, which    *)
(* keeps its first send and carries a due-now stamp below every real send *)
(* time.  park and push_head link the first kind (push_head: a refused     *)
(* first send), push_tail and push_mark the second.  mk[c] is              *)
(* ack->resend_sent ("s") / ack->chunk_sent ("c"), the slot of the class's *)
(* first number on the wire, where the walk of the numbers on the wire     *)
(* starts and where the walk of the numbers never sent ends; the ghost     *)
(* sent[s] is "on the wire" (last_send is not RN_SEND_NUMBER_NEVER_SENT).  *)
(* The unlink moves the mark to the slot's next when the slot is the mark, *)
(* push_tail makes the slot the mark when its class had none, push_mark    *)
(* links a refused resend right before the mark (at the tail when the      *)
(* class has none) and makes it the mark; park and push_head link before   *)
(* the mark and leave it.  MarkMatch says mk is the first sent slot of its *)
(* sequence.                                                               *)
(*                                                                         *)
(* The order of last send.  The walk of the numbers on the wire stops at   *)
(* the first one not yet due, which is right only if they are in order of  *)
(* last send.  The ghost order is every slot on the wire in order of last  *)
(* send: push_tail appends (a send now is the latest), push_mark puts the  *)
(* slot first (the due-now stamp is below every send time; of two refused  *)
(* in one packet the code's reverse loop leaves the first first, as here), *)
(* an unlink or a new number removes it.  Ordered says each class's        *)
(* numbers on the wire are in that order.                                  *)
(*                                                                         *)
(* An entry is prev and next only (rn_resend_link_t, 8 bytes); which      *)
(* number an ack may unlink is RNResendGenerations.tla's, and Unlink of    *)
(* any slot at any time covers it.                                         *)
(*                                                                         *)
(* Reversed witnesses (each flag FALSE is a bug class):                    *)
(*   SelfTailFix       push_tail without "if ( tail == slot ) tail = prev" *)
(*   TailOnEnd         insert_after without "else *tail = slot"            *)
(*   UnlinkClearsClass unlink leaves the class bit in next                 *)
(*   InsertSetsClass   insert_after ORs the class bit in, never clears it  *)
(* the last two are one witness: either alone is masked by the other.     *)
(*   MarkOnUnlink      unlink leaves the mark on the slot it removes       *)
(*   MarkOnPushTail    push_tail never starts the sent part of a class     *)
(*   MarkInitNone      init leaves the mark at 0 (the memset) for NONE     *)
(*   RefusedTakesMark  push_mark links before the mark but leaves the mark *)
(*   RefusedBeforeMark push_mark links at the class's tail, as a send now  *)
(*   ParkContract      park after a number on the wire (the walk's         *)
(*                     last_unsent wrong)                                  *)
(***************************************************************************)
EXTENDS Integers, Sequences, FiniteSets

CONSTANTS
    W,                  \* window entries (a power of two in the code)
    SelfTailFix,        \* rn_resend_fifo_push_tail steps back when slot is the tail
    TailOnEnd,          \* rn_resend_fifo_insert_after sets the tail when next is NONE
    UnlinkClearsClass,  \* rn_resend_fifo_unlink writes next = 0
    InsertSetsClass,    \* rn_resend_fifo_insert_after writes next = chunk ? CHUNK : 0
    MarkOnUnlink,       \* rn_resend_fifo_unlink moves the class's mark off the slot
    MarkOnPushTail,     \* rn_resend_fifo_push_tail sets the mark of a class with none
    MarkInitNone,       \* rn_window_ack_init sets both marks to RN_RESEND_NONE
    RefusedTakesMark,   \* rn_resend_fifo_push_mark makes the slot the mark
    RefusedBeforeMark,  \* rn_resend_fifo_push_mark links right before the mark
    ParkContract        \* rn_window_ack_park's `after` is not on the wire

ASSUME W \in Nat \ {0}
ASSUME \A f \in {SelfTailFix, TailOnEnd, UnlinkClearsClass, InsertSetsClass,
                 MarkOnUnlink, MarkOnPushTail, MarkInitNone,
                 RefusedTakesMark, RefusedBeforeMark, ParkContract} : f \in BOOLEAN

Slots == 0..(W - 1)
NONE == W                       \* RN_RESEND_NONE, and RN_RESEND_END in a field
Classes == {"s", "c"}
ClassOf(chunk) == IF chunk THEN "c" ELSE "s"

Cleared == [ pf |-> [ s \in Slots |-> [ lk |-> FALSE, ix |-> 0 ] ],
             nf |-> [ s \in Slots |-> [ ck |-> FALSE, ix |-> 0 ] ] ]

VARIABLES
    pf,         \* pf[s]: entry s's prev field, [lk = bit 31, ix = low bits]
    nf,         \* nf[s]: entry s's next field, [ck = bit 31, ix = low bits]
    hd,         \* hd[c]: the head of class c
    tl,         \* tl[c]: the tail of class c
    cnt,        \* ack->resend_count
    mk,         \* mk[c]: ack->resend_sent / chunk_sent, the class's first sent slot
    isChunk,    \* isChunk[s]: rn_window_ack_slot_is_chunk( ack, s ) now
    seq,        \* ghost: seq[c], the abstract sequence of class c
    sent,       \* ghost: sent[s], the slot's number is on the wire
    order       \* ghost: the slots on the wire in order of last send

vars == << pf, nf, hd, tl, cnt, mk, isChunk, seq, sent, order >>

L == [ pf |-> pf, nf |-> nf, hd |-> hd, tl |-> tl, cnt |-> cnt, mk |-> mk ]

Remove(q, s) == SelectSeq(q, LAMBDA x : x # s)
InSeq(q, s) == \E k \in 1..Len(q) : q[k] = s
Pos(q, s) == CHOOSE k \in 1..Len(q) : q[k] = s
InsertAt(q, k, s) == SubSeq(q, 1, k) \o << s >> \o SubSeq(q, k + 1, Len(q))

-----------------------------------------------------------------------------
(* The code's operations on the layout, each a function of the state     *)
(* record st = [pf, nf, hd, tl, cnt, mk].                                 *)

\* rn_resend_fifo_unlink
UnlinkL(st, s) ==
    IF ~st.pf[s].lk THEN st
    ELSE
    LET c == ClassOf(st.nf[s].ck)
        p == st.pf[s].ix
        n == st.nf[s].ix
        nf1 == IF p # NONE THEN [ st.nf EXCEPT ![p].ix = n ] ELSE st.nf
        hd1 == IF p # NONE THEN st.hd ELSE [ st.hd EXCEPT ![c] = n ]
        pf1 == IF n # NONE THEN [ st.pf EXCEPT ![n] = [ lk |-> TRUE, ix |-> p ] ] ELSE st.pf
        tl1 == IF n # NONE THEN st.tl ELSE [ st.tl EXCEPT ![c] = p ]
        mk1 == IF MarkOnUnlink /\ st.mk[c] = s THEN [ st.mk EXCEPT ![c] = n ] ELSE st.mk
    IN [ pf |-> [ pf1 EXCEPT ![s] = [ lk |-> FALSE, ix |-> 0 ] ],
         nf |-> [ nf1 EXCEPT ![s] = [ ck |-> IF UnlinkClearsClass THEN FALSE ELSE nf1[s].ck, ix |-> 0 ] ],
         hd |-> hd1,
         tl |-> tl1,
         cnt |-> st.cnt - 1,
         mk |-> mk1 ]

\* rn_resend_fifo_insert_after, after its unlink:
\* links s right after `after` (NONE: at the head) of class c
LinkAfter(st, after, s, chunk) ==
    LET c == ClassOf(chunk)
        n == IF after = NONE THEN st.hd[c] ELSE st.nf[after].ix
        ck0 == IF InsertSetsClass THEN chunk ELSE st.nf[s].ck \/ chunk
        nf1 == [ st.nf EXCEPT ![s] = [ ck |-> ck0, ix |-> n ] ]
        pf1 == [ st.pf EXCEPT ![s] = [ lk |-> TRUE, ix |-> after ] ]
        nf2 == IF after # NONE THEN [ nf1 EXCEPT ![after].ix = s ] ELSE nf1
        hd2 == IF after # NONE THEN st.hd ELSE [ st.hd EXCEPT ![c] = s ]
        pf2 == IF n # NONE THEN [ pf1 EXCEPT ![n] = [ lk |-> TRUE, ix |-> s ] ] ELSE pf1
        tl2 == IF n # NONE \/ ~TailOnEnd THEN st.tl ELSE [ st.tl EXCEPT ![c] = s ]
    IN [ pf |-> pf2, nf |-> nf2, hd |-> hd2, tl |-> tl2, cnt |-> st.cnt + 1, mk |-> st.mk ]

InsertAfterL(st, after, s, chunk) == LinkAfter(UnlinkL(st, s), after, s, chunk)

SetL(st) ==
    /\ pf' = st.pf
    /\ nf' = st.nf
    /\ hd' = st.hd
    /\ tl' = st.tl
    /\ cnt' = st.cnt
    /\ mk' = st.mk

-----------------------------------------------------------------------------

Init ==
    /\ pf = Cleared.pf
    /\ nf = Cleared.nf
    /\ hd = [ c \in Classes |-> NONE ]
    /\ tl = [ c \in Classes |-> NONE ]
    /\ cnt = 0
    /\ mk = [ c \in Classes |-> IF MarkInitNone THEN NONE ELSE 0 ]
    /\ isChunk \in [ Slots -> BOOLEAN ]
    /\ seq = [ c \in Classes |-> << >> ]
    /\ sent = [ s \in Slots |-> FALSE ]
    /\ order = << >>

\* rn_resend_fifo_push_tail, from rn_window_ack_commit
\* step 1: a sent number joins the tail of its class, and is the class's
\* mark when the class had no number on the wire
PushTail(s) ==
    LET chunk == isChunk[s]
        c == ClassOf(chunk)
        t0 == tl[c]
        t == IF SelfTailFix /\ t0 = s THEN pf[s].ix ELSE t0
        st == InsertAfterL(L, t, s, chunk)
    IN /\ SetL([ st EXCEPT !.mk = IF MarkOnPushTail /\ st.mk[c] = NONE THEN [ st.mk EXCEPT ![c] = s ] ELSE st.mk ])
       /\ seq' = [ k \in Classes |-> IF k = c THEN Append(Remove(seq[k], s), s) ELSE Remove(seq[k], s) ]
       /\ sent' = [ sent EXCEPT ![s] = TRUE ]
       /\ order' = Append(Remove(order, s), s)
       /\ UNCHANGED isChunk

\* rn_resend_fifo_push_head, from rn_window_ack_packet_failed: a refused
\* first send stays never sent and goes to the head of its class
PushHead(s) ==
    LET chunk == isChunk[s]
        c == ClassOf(chunk)
    IN /\ ~sent[s]
       /\ SetL(InsertAfterL(L, NONE, s, chunk))
       /\ seq' = [ k \in Classes |-> IF k = c THEN << s >> \o Remove(seq[k], s) ELSE Remove(seq[k], s) ]
       /\ UNCHANGED << isChunk, sent, order >>

\* the slots of q not on the wire, all ahead of the others (Partitioned)
Unsent(q) == Len(SelectSeq(q, LAMBDA x : ~sent[x]))

\* rn_resend_fifo_push_mark, from rn_window_ack_packet_failed: a refused
\* resend keeps its first send, takes the due-now stamp and goes back on the
\* wire first: unlink, then link after the mark's prev (after the tail when
\* the class has no mark), then the mark is the slot
PushMark(s) ==
    LET chunk == isChunk[s]
        c == ClassOf(chunk)
        st0 == UnlinkL(L, s)
        after == IF ~RefusedBeforeMark \/ st0.mk[c] = NONE THEN st0.tl[c] ELSE st0.pf[st0.mk[c]].ix
        st1 == LinkAfter(st0, after, s, chunk)
        mk1 == IF ~RefusedBeforeMark
               THEN IF st1.mk[c] = NONE THEN [ st1.mk EXCEPT ![c] = s ] ELSE st1.mk
               ELSE IF RefusedTakesMark THEN [ st1.mk EXCEPT ![c] = s ] ELSE st1.mk
        q == Remove(seq[c], s)
    IN /\ sent[s]
       /\ SetL([ st1 EXCEPT !.mk = mk1 ])
       /\ seq' = [ k \in Classes |->
                     IF k # c THEN Remove(seq[k], s)
                     ELSE IF RefusedBeforeMark THEN InsertAt(q, Unsent(q), s) ELSE Append(q, s) ]
       /\ order' = << s >> \o Remove(order, s)
       /\ UNCHANGED << isChunk, sent >>

\* rn_window_ack_park, from rn_ordered_write
\* (channel.cpp): a newly numbered slot joins after the last never-sent
\* number of its class (after = NONE: at the head).  The caller's
\* contract: after is linked in s's class and is not s (both asserted
\* there), and is not on the wire (the walk's last never-sent number).
Park(after, s) ==
    LET chunk == isChunk[s]
        c == ClassOf(chunk)
    IN /\ after # s
       /\ after = NONE \/ (InSeq(seq[c], after) /\ (~sent[after] \/ ~ParkContract))
       /\ SetL(InsertAfterL(L, after, s, chunk))
       /\ seq' = [ k \in Classes |->
                     IF k # c THEN Remove(seq[k], s)
                     ELSE LET q == Remove(seq[k], s)
                          IN IF after = NONE THEN << s >> \o q ELSE InsertAt(q, Pos(q, after), s) ]
       /\ sent' = [ sent EXCEPT ![s] = FALSE ]
       /\ order' = Remove(order, s)
       /\ UNCHANGED isChunk

\* rn_resend_fifo_unlink, from rn_window_ack_on_ack: the acked number leaves
\* its class
Unlink(s) ==
    /\ SetL(UnlinkL(L, s))
    /\ seq' = [ k \in Classes |-> Remove(seq[k], s) ]
    /\ order' = Remove(order, s)
    /\ UNCHANGED << isChunk, sent >>

\* the environment: the slot's descriptor is freed, or another attached,
\* so rn_window_ack_slot_is_chunk changes, linked or not
SetClass(s) ==
    /\ isChunk' = [ isChunk EXCEPT ![s] = ~isChunk[s] ]
    /\ UNCHANGED << pf, nf, hd, tl, cnt, mk, seq, sent, order >>

AfterSlots == Slots \cup {NONE}

Next ==
    \/ \E s \in Slots : PushTail(s) \/ PushHead(s) \/ PushMark(s) \/ Unlink(s) \/ SetClass(s)
    \/ \E a \in AfterSlots, s \in Slots : Park(a, s)

Spec == Init /\ [][Next]_vars

-----------------------------------------------------------------------------

Field == [ lk : BOOLEAN, ix : 0..W ]

TypeOK ==
    /\ \A s \in Slots : pf[s].lk \in BOOLEAN /\ pf[s].ix \in 0..W
    /\ \A s \in Slots : nf[s].ck \in BOOLEAN /\ nf[s].ix \in 0..W
    /\ hd \in [ Classes -> 0..W ]
    /\ tl \in [ Classes -> 0..W ]
    /\ cnt \in Int
    /\ mk \in [ Classes -> 0..W ]
    /\ sent \in [ Slots -> BOOLEAN ]
    /\ order \in Seq(Slots)

\* the array, heads, tails, flag bits and count are exactly the two sequences
ListsMatch ==
    /\ \A c \in Classes :
         LET q == seq[c] IN
         /\ hd[c] = (IF q = << >> THEN NONE ELSE q[1])
         /\ tl[c] = (IF q = << >> THEN NONE ELSE q[Len(q)])
         /\ \A k \in 1..Len(q) :
              /\ pf[q[k]].lk
              /\ nf[q[k]].ck = (c = "c")
              /\ pf[q[k]].ix = (IF k = 1 THEN NONE ELSE q[k - 1])
              /\ nf[q[k]].ix = (IF k = Len(q) THEN NONE ELSE q[k + 1])
    /\ \A s \in Slots : (~InSeq(seq["s"], s) /\ ~InSeq(seq["c"], s)) => ~pf[s].lk
    /\ cnt = Len(seq["s"]) + Len(seq["c"])

\* a slot is in at most one sequence, once
Disjoint ==
    /\ \A c \in Classes : Cardinality({ seq[c][k] : k \in 1..Len(seq[c]) }) = Len(seq[c])
    /\ \A s \in Slots : ~(InSeq(seq["s"], s) /\ InSeq(seq["c"], s))

\* each sequence is its numbers not on the wire, then its numbers on the wire
Partitioned ==
    \A c \in Classes : \A i, j \in 1..Len(seq[c]) : (i < j /\ sent[seq[c][i]]) => sent[seq[c][j]]

\* the first slot of q on the wire, or NONE
FirstSent(q) ==
    IF \E k \in 1..Len(q) : sent[q[k]]
    THEN q[CHOOSE k \in 1..Len(q) : sent[q[k]] /\ \A j \in 1..(k - 1) : ~sent[q[j]]]
    ELSE NONE

\* the code's mark is each class's first number on the wire
MarkMatch ==
    \A c \in Classes : mk[c] = FirstSent(seq[c])

\* each class's numbers on the wire are in order of last send, so the walk
\* from the mark may stop at the first one not yet due
Ordered ==
    \A c \in Classes :
        SelectSeq(seq[c], LAMBDA x : sent[x]) = SelectSeq(order, LAMBDA x : InSeq(seq[c], x))
=============================================================================
