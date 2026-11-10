--------------------------- MODULE RNSequencedSlot ---------------------------
(***************************************************************************)
(* The receive dispatch of a reliable-sequenced (code 4) block superseded *)
(* by a later ordered epoch, on the receiver's descriptor slots, against  *)
(* an honest sender bounded by its own window.                            *)
(*                                                                         *)
(* The sender's workload is fixed, one channel, in channel FIFO order:     *)
(*   0      Y, reliable ordered, ordering index 0                          *)
(*   1, 2   O, a two-fragment code-4 block, ordering 1, sequencing 0       *)
(*   3      Z, reliable ordered, ordering index 1                          *)
(*   4      N, a one-record code-4 state, ordering 2, sequencing 0         *)
(*   5..    plain reliable records (code 2)                                *)
(* (rn_channels_send_ordered stamps ordered_write and resets               *)
(* sequenced_write; rn_channels_send_sequenced stamps ordered_write and    *)
(* sequenced_write, sdk/rocketnet_channel.cpp.)  Number reserves a block's *)
(* numbers at once and admits next + count <= U' + W, U' the first number *)
(* of the block holding the lowest unacked number (rn_window_send_admit,  *)
(* rn_window_send_first_write_block, sdk/rocketnet_window_send.cpp).       *)
(* Packets arrive in any order, one number each; a number becomes          *)
(* ackable at the first EndUpdate after it was accepted (a delivered view  *)
(* lives until then, P6).                                                  *)
(*                                                                         *)
(* The receiver's state is the code's: desc[s] for s in 0..W-1 is          *)
(* receive_descriptors[s] (live, the number it holds, block_count,         *)
(* block_have; the code and ordering of a descriptor are the workload's    *)
(* for its number), and a released descriptor keeps its number, as        *)
(* live = 0 does in the code.  ord and wm are ordered_read and             *)
(* sequenced_watermark; hs/hsf are held_sequenced and held_sequenced_first *)
(* (a slot, or NONE); bit/bitf are block_in_turn and block_in_turn_first;  *)
(* hord the ordered records held ahead of their turn (the held index);     *)
(* dl the delivery table's descriptor slots; viol the violations counted.  *)
(*                                                                         *)
(* Receive(n) is rn_window_receive_accept (sdk/rocketnet_window_receive.   *)
(* cpp: a live descriptor for another number at n % W is SLOT_COLLISION,   *)
(* else the descriptor is claimed and R advances) followed by the code's   *)
(* dispatch: rn_channels_dispatch_sequenced, rn_sequenced_in_turn and      *)
(* rn_sequenced_block (sdk/rocketnet_channel_sequenced.cpp) for code 4;    *)
(* rn_channels_dispatch_ordered with rn_channels_deliver_ordered and the   *)
(* rn_channels_drain loop, which runs rn_channels_drain_sequenced before   *)
(* each held successor (sdk/rocketnet_channel.cpp), for an ordered record; *)
(* an at-once delivery for code 2.  EndUpdate is rn_channels_end_update:   *)
(* each delivery's rn_channels_release frees its number, or a block's      *)
(* every fragment.  Every operator below names the code it follows.        *)
(*                                                                         *)
(* CheckAcceptedSpan is the fix in rn_sequenced_block: a fragment that     *)
(* would open a new block while a sibling number of its span is a code-4   *)
(* descriptor that was accepted and is no longer live (released when the  *)
(* block was superseded) is stale.  FALSE removes it: such a fragment      *)
(* anchors an orphan in-turn block whose descriptor is never completed,    *)
(* and a later number on the same slot meets SLOT_COLLISION (the reversed  *)
(* witness MCRNSequencedSlotBrokenSpan.cfg).                               *)
(*                                                                         *)
(* Abstract here: the 24-bit wire arithmetic (the numbers stay far below   *)
(* the half range; RNSequencingWrap.tla carries it), the ring bytes and    *)
(* block shape checks (an honest sender passes them), delivery-table room  *)
(* (never short here), code-1 holds, more than one channel, packet ids,    *)
(* several records per packet, duplicates and retransmission (a packet    *)
(* arrives at most once).  The workload is fixed: the model checks every   *)
(* arrival order, update end and ACK of this one composition.              *)
(***************************************************************************)
EXTENDS Integers, FiniteSets

CONSTANTS W, MaxNum, CheckAcceptedSpan
ASSUME /\ W \in {4, 8}
       /\ MaxNum \in 6..10
       /\ CheckAcceptedSpan \in BOOLEAN

NONE == -1
Nums == 0..MaxNum
Slots == 0..W-1

\* the sender's workload, by number
Rec(n) ==
    CASE n = 0 -> [code |-> "ord", ordering |-> 0, seq |-> 0, frag |-> 0, count |-> 1]
      [] n = 1 -> [code |-> "seq", ordering |-> 1, seq |-> 0, frag |-> 0, count |-> 2]
      [] n = 2 -> [code |-> "seq", ordering |-> 1, seq |-> 0, frag |-> 1, count |-> 2]
      [] n = 3 -> [code |-> "ord", ordering |-> 1, seq |-> 0, frag |-> 0, count |-> 1]
      [] n = 4 -> [code |-> "seq", ordering |-> 2, seq |-> 0, frag |-> 0, count |-> 1]
      [] OTHER -> [code |-> "rel", ordering |-> 0, seq |-> 0, frag |-> 0, count |-> 1]

VARIABLES next, acked, ackable, acc, R, rv
vars == <<next, acked, ackable, acc, R, rv>>

EmptyDesc == [live |-> FALSE, num |-> NONE, bc |-> 0, bh |-> 0]
InitRv == [desc |-> [s \in Slots |-> EmptyDesc], ord |-> 0, wm |-> 0,
           hs |-> NONE, hsf |-> 0, bit |-> NONE, bitf |-> 0,
           hord |-> {}, dl |-> {}, viol |-> {}]

-----------------------------------------------------------------------------
\* The sender

Unacked == (0..next-1) \ acked
U == IF Unacked = {} THEN next ELSE CHOOSE x \in Unacked : \A y \in Unacked : x <= y
UPrime == IF U <= MaxNum THEN U - Rec(U).frag ELSE U

RECURSIVE Advance(_, _)
Advance(s, r) == IF r \in s THEN Advance(s, r + 1) ELSE r

Init ==
    /\ next = 0 /\ acked = {} /\ ackable = {} /\ acc = {} /\ R = 0
    /\ rv = InitRv

Number ==
    /\ next <= MaxNum
    /\ next + Rec(next).count <= UPrime + W
    /\ next' = next + Rec(next).count
    /\ UNCHANGED <<acked, ackable, acc, R, rv>>

Ack(n) ==
    /\ n \in ackable \ acked
    /\ acked' = acked \cup {n}
    /\ UNCHANGED <<next, ackable, acc, R, rv>>

-----------------------------------------------------------------------------
\* Descriptor helpers

SeqDesc(S, s) == S.desc[s].num # NONE /\ Rec(S.desc[s].num).code = "seq"
Bad(S, v) == [S EXCEPT !.viol = @ \cup {v}]

\* rn_sequenced_release_number: only the exact live number
ReleaseNumber(S, n) ==
    IF S.desc[n % W].live /\ S.desc[n % W].num = n
    THEN [S EXCEPT !.desc[n % W].live = FALSE] ELSE S

\* rn_sequenced_hold_valid: a code-4 hold names a live code-4 descriptor
HoldValid(S, h) == h = NONE \/ (S.desc[h].live /\ SeqDesc(S, h))

\* rn_sequenced_block_owns: a code-4 descriptor with the anchor's ordering
\* and sequencing
Owns(S, a, d) ==
    /\ SeqDesc(S, d)
    /\ Rec(S.desc[d].num).ordering = Rec(S.desc[a].num).ordering
    /\ Rec(S.desc[d].num).seq = Rec(S.desc[a].num).seq

\* rn_sequenced_release_slot on hold field f ("hs" or "bit") with its span's
\* first number: frees the owned live descriptors of the anchor's span; a
\* live one it does not own is BAD_BLOCK.  [st |-> state, ok |-> owned]
ReleaseSlot(S, f, first) ==
    LET h == S[f] IN
    IF h = NONE THEN [st |-> S, ok |-> TRUE]
    ELSE IF S.desc[h].live /\ SeqDesc(S, h)
    THEN LET count == IF S.desc[h].bc > 0 THEN S.desc[h].bc ELSE 1
             xs == {first + i : i \in 0..count-1}
             hit == {x \in xs : S.desc[x % W].live /\ S.desc[x % W].num = x}
             own == {x \in hit : Owns(S, h, x % W)}
             S1 == [S EXCEPT !.desc = [s \in Slots |->
                                IF \E x \in own : x % W = s
                                THEN [S.desc[s] EXCEPT !.live = FALSE]
                                ELSE S.desc[s]],
                             ![f] = NONE]
         IN IF own = hit THEN [st |-> S1, ok |-> TRUE]
            ELSE [st |-> Bad(S1, "BAD_BLOCK"), ok |-> FALSE]
    ELSE [st |-> Bad([S EXCEPT ![f] = NONE], "BAD_BLOCK"), ok |-> FALSE]

\* rn_channels_deliver_sequenced: only in this epoch, at or past the watermark
DeliverSeqOK(S, o, q) == o = S.ord /\ q - S.wm >= 0

-----------------------------------------------------------------------------
\* rn_sequenced_block, from "every chunk is present" on: a's block_have is
\* raised; the complete block moves to its first slot and is delivered in
\* turn (delta = 0) or stays held

Collect(S, n, f, ff, delta, a) ==
    LET c == Rec(n).count
        S1 == [S EXCEPT !.desc[a].bh = @ + 1]
    IN IF S1.desc[a].bh < c THEN S1
       ELSE LET first == S1[ff]
                fs == first % W
                fin == first + c - 1
            IN IF ~(/\ S1.desc[fs].live /\ S1.desc[fs].num = first
                    /\ S1.desc[fin % W].live /\ S1.desc[fin % W].num = fin)
               THEN Bad(S1, "BAD_BLOCK")
               ELSE LET S2 == IF a # fs THEN [S1 EXCEPT !.desc[a].bc = 0] ELSE S1
                        S3 == [S2 EXCEPT !.desc[fs].bc = c, !.desc[fs].bh = c, ![f] = fs]
                    IN IF delta # 0 THEN S3
                       ELSE IF DeliverSeqOK(S3, Rec(n).ordering, Rec(n).seq)
                            THEN [S3 EXCEPT !.dl = @ \cup {fs}, !.wm = Rec(n).seq + 1, ![f] = NONE]
                            ELSE S3

\* rn_sequenced_block: the first fragment to arrive anchors the block in
\* hold field f (unless CheckAcceptedSpan finds a released sibling: stale),
\* a later one joins it
Block(S, n, f, ff, delta) ==
    LET r == Rec(n)
        s == n % W
        first == n - r.frag
        cur == S[f]
    IN IF cur = NONE
       THEN IF /\ CheckAcceptedSpan
               /\ \E i \in 0..r.count-1 :
                    LET x == first + i IN
                    /\ x # n
                    /\ ~S.desc[x % W].live /\ S.desc[x % W].num = x /\ SeqDesc(S, x % W)
            THEN ReleaseNumber(S, n)
            ELSE Collect([S EXCEPT ![f] = s, ![ff] = first,
                                   !.desc[s].bc = r.count, !.desc[s].bh = 0],
                         n, f, ff, delta, s)
       ELSE IF ~(/\ S.desc[cur].live /\ S.desc[cur].bc = r.count
                 /\ S.desc[cur].bh < r.count /\ S[ff] = first
                 /\ Rec(S.desc[cur].num).ordering = r.ordering
                 /\ Rec(S.desc[cur].num).seq = r.seq)
            THEN Bad(S, "BAD_BLOCK")
            ELSE Collect(S, n, f, ff, delta, cur)

\* rn_sequenced_in_turn (delta = 0)
InTurn(S, n) ==
    LET r == Rec(n)
        block == r.count > 1
        cur == S.bit
    IN IF cur # NONE /\ ~HoldValid(S, cur)
       THEN Bad([S EXCEPT !.bit = NONE], "PROTOCOL_VIOLATION")
       ELSE LET hg == IF cur = NONE THEN 0 ELSE r.seq - Rec(S.desc[cur].num).seq
                other == cur # NONE /\ ~(block /\ hg = 0)
            IN IF other /\ hg <= 0 THEN ReleaseNumber(S, n)
               ELSE LET R1 == IF other THEN ReleaseSlot(S, "bit", S.bitf)
                              ELSE [st |-> S, ok |-> TRUE]
                        S1 == R1.st
                    IN IF ~R1.ok THEN S1
                       ELSE IF r.seq - S1.wm < 0
                       THEN ReleaseSlot(ReleaseNumber(S1, n), "bit", S1.bitf).st
                       ELSE IF block THEN Block(S1, n, "bit", "bitf", 0)
                       ELSE IF DeliverSeqOK(S1, r.ordering, r.seq)
                            THEN [S1 EXCEPT !.dl = @ \cup {n % W}, !.wm = r.seq + 1]
                            ELSE S1

\* rn_channels_dispatch_sequenced from the future-state slot on (delta > 0)
Place(S, n, delta) ==
    IF Rec(n).count > 1 THEN Block(S, n, "hs", "hsf", delta)
    ELSE [S EXCEPT !.hs = n % W, !.hsf = n]

Future(S, n, delta) ==
    LET r == Rec(n)
        cur == S.hs
    IN IF cur # NONE /\ ~HoldValid(S, cur)
       THEN Bad([S EXCEPT !.hs = NONE], "PROTOCOL_VIOLATION")
       ELSE IF cur = NONE THEN Place(S, n, delta)
       ELSE LET hr == Rec(S.desc[cur].num)
                gap == r.ordering - hr.ordering
                sg == r.seq - hr.seq
                cont == r.count > 1 /\ gap = 0 /\ sg = 0 /\ S.desc[cur].bc > 0
            IN IF gap < 0 \/ (gap = 0 /\ (sg < 0 \/ (sg = 0 /\ ~cont)))
               THEN ReleaseNumber(S, n)
               ELSE IF cont THEN Place(S, n, delta)
               ELSE LET R1 == ReleaseSlot(S, "hs", S.hsf)
                    IN IF R1.ok THEN Place(R1.st, n, delta) ELSE R1.st

\* rn_channels_dispatch_sequenced for a code-4 record
DispatchSeq(S, n) ==
    LET delta == Rec(n).ordering - S.ord IN
    IF delta < 0 THEN ReleaseNumber(S, n)
    ELSE IF delta = 0 THEN InTurn(S, n)
    ELSE Future(S, n, delta)

\* rn_channels_drain_sequenced, after an ordered delivery opened an epoch
DrainSeq(S) ==
    IF ~HoldValid(S, S.bit) \/ ~HoldValid(S, S.hs)
    THEN Bad([S EXCEPT !.bit = NONE, !.hs = NONE], "PROTOCOL_VIOLATION")
    ELSE LET R1 == IF S.bit # NONE /\ Rec(S.desc[S.bit].num).ordering # S.ord
                   THEN ReleaseSlot(S, "bit", S.bitf)
                   ELSE [st |-> S, ok |-> TRUE]
             S1 == R1.st
             h == S1.hs
         IN IF ~R1.ok \/ h = NONE THEN S1
            ELSE LET hr == Rec(S1.desc[h].num)
                     gap == hr.ordering - S1.ord
                 IN IF gap # 0
                    THEN IF gap < 0 THEN ReleaseSlot(S1, "hs", S1.hsf).st ELSE S1
                    ELSE IF S1.desc[h].bc > 0 /\ S1.desc[h].bh < S1.desc[h].bc
                    THEN IF S1.bit = NONE
                         THEN [S1 EXCEPT !.bit = h, !.bitf = S1.hsf, !.hs = NONE]
                         ELSE S1
                    ELSE IF DeliverSeqOK(S1, hr.ordering, hr.seq)
                    THEN [S1 EXCEPT !.hs = NONE, !.dl = @ \cup {h}, !.wm = hr.seq + 1]
                    ELSE ReleaseSlot(S1, "hs", S1.hsf).st

\* rn_channels_deliver_ordered
DeliverOrdered(S, k) == [S EXCEPT !.ord = k + 1, !.wm = 0]

\* rn_channels_drain: the sequenced drain, then the held successor, again
RECURSIVE DrainLoop(_, _)
DrainLoop(S, fuel) ==
    LET S1 == DrainSeq(S)
        z == {m \in S1.hord : Rec(m).ordering = S1.ord}
    IN IF fuel = 0 \/ z = {} THEN S1
       ELSE LET m == CHOOSE m \in z : TRUE IN
            IF ~(S1.desc[m % W].live /\ S1.desc[m % W].num = m)
            THEN Bad(S1, "PROTOCOL_VIOLATION")
            ELSE DrainLoop(DeliverOrdered([S1 EXCEPT !.hord = @ \ {m}, !.dl = @ \cup {m % W}],
                                          Rec(m).ordering), fuel - 1)

\* rn_channels_dispatch_ordered
DispatchOrd(S, n) ==
    LET k == Rec(n).ordering
        delta == k - S.ord
    IN IF delta < 0 THEN Bad(S, "ORDERING_BEHIND")
       ELSE IF delta = 0
       THEN DrainLoop(DeliverOrdered([S EXCEPT !.dl = @ \cup {n % W}], k), 4)
       ELSE [S EXCEPT !.hord = @ \cup {n}]

Dispatch(S, n) ==
    CASE Rec(n).code = "seq" -> DispatchSeq(S, n)
      [] Rec(n).code = "ord" -> DispatchOrd(S, n)
      [] OTHER -> [S EXCEPT !.dl = @ \cup {n % W}]

\* rn_window_receive_accept, then the dispatch.  A violation ends the
\* connection: nothing more is received.
Receive(n) ==
    /\ rv.viol = {}
    /\ n \in 0..next-1 /\ n \notin acc /\ n >= R
    /\ IF n >= R + W
       THEN /\ rv' = Bad(rv, "NUMBER_OUTSIDE_WINDOW")
            /\ UNCHANGED <<acc, R>>
       ELSE IF rv.desc[n % W].live /\ rv.desc[n % W].num # n
       THEN /\ rv' = Bad(rv, "SLOT_COLLISION")
            /\ UNCHANGED <<acc, R>>
       ELSE /\ rv' = Dispatch([rv EXCEPT !.desc[n % W] = [live |-> TRUE, num |-> n, bc |-> 0, bh |-> 0]], n)
            /\ acc' = acc \cup {n}
            /\ R' = Advance(acc \cup {n}, R)
    /\ UNCHANGED <<next, acked, ackable>>

\* rn_channels_release for one delivery slot
ReleaseDelivery(S, s) ==
    IF ~S.desc[s].live THEN S
    ELSE LET num == S.desc[s].num
             count == IF S.desc[s].bc > 0 THEN S.desc[s].bc ELSE 1
             xs == {num + i : i \in 0..count-1}
         IN [S EXCEPT !.desc = [t \in Slots |->
                IF \E x \in xs : x % W = t /\ S.desc[t].live /\ S.desc[t].num = x
                THEN [S.desc[t] EXCEPT !.live = FALSE, !.bc = 0, !.bh = 0]
                ELSE S.desc[t]]]

RECURSIVE ReleaseAll(_, _)
ReleaseAll(S, D) ==
    IF D = {} THEN S
    ELSE LET s == CHOOSE s \in D : \A t \in D : s <= t
         IN ReleaseAll(ReleaseDelivery(S, s), D \ {s})

\* rn_channels_end_update: every delivery's descriptors are released, and
\* the numbers accepted so far become ackable
EndUpdate ==
    /\ rv' = [ReleaseAll(rv, rv.dl) EXCEPT !.dl = {}]
    /\ ackable' = acc
    /\ UNCHANGED <<next, acked, acc, R>>

Next ==
    \/ Number
    \/ EndUpdate
    \/ \E n \in Nums : Receive(n)
    \/ \E n \in Nums : Ack(n)

Spec == Init /\ [][Next]_vars

-----------------------------------------------------------------------------

\* An honest sender never drives the receiver to SLOT_COLLISION (check 10).
NoSlotCollision == "SLOT_COLLISION" \notin rv.viol

\* Nor to any other violation this model reaches.
NoViolation == rv.viol = {}

TypeOK ==
    /\ next \in 0..MaxNum + 1
    /\ acked \subseteq ackable /\ ackable \subseteq acc /\ acc \subseteq 0..next-1
    /\ R \in 0..MaxNum + 1
    /\ \A s \in Slots : /\ rv.desc[s].live \in BOOLEAN
                        /\ rv.desc[s].num \in Nums \cup {NONE}
                        /\ rv.desc[s].bc \in 0..2 /\ rv.desc[s].bh \in 0..2
    /\ rv.ord \in 0..2 /\ rv.wm \in 0..1
    /\ rv.hs \in Slots \cup {NONE} /\ rv.bit \in Slots \cup {NONE}
    /\ rv.hord \subseteq Nums /\ rv.dl \subseteq Slots
=============================================================================
