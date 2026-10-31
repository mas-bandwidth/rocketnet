-------------------------- MODULE RNSentRingLayout --------------------------
(***************************************************************************)
(* The sender's sent-packet ring, its reference ring and the receipt ring *)
(* as sdk/rocketnet_window_ack.cpp lays them out, with the code's own      *)
(* arithmetic, and the receipt serial counter of sdk/rocketnet_endpoint.cpp *)
(* and sdk/rocketnet_message_server.cpp.  RNSentMap, RNSentHistory and     *)
(* RNReceipts check these as maps, sets and sequences; this module checks *)
(* the mapping from those to the arrays, which they cannot see.            *)
(*                                                                         *)
(* sent_packets: P entries, slot k % P for the k-th committed packet,      *)
(* pkt_head and pkt_tail 64-bit counters that never wrap.  An acked entry  *)
(* is a hole (packet_id 0) until it reaches the head; rn_window_ack_       *)
(* reclaim_head advances the head past holes and, when the ring is empty, *)
(* resets ref_head and ref_tail to 0.                                      *)
(*                                                                         *)
(* sent_refs: max_refs entries (16P in the code), each packet's reliable  *)
(* numbers then its receipt serials at ref_tail % max_refs onward;        *)
(* ref_first and receipt_first in the entry.  The commit evicts the oldest *)
(* entry while pkt_tail - pkt_head >= P, or while (ref_tail - ref_head) +  *)
(* total > max_refs.  A reference cell holds here the ghost identity of    *)
(* what was written into it: [k = the packet's write index, j = its        *)
(* position], z = TRUE once a serial is zeroed (a resolved receipt).       *)
(*                                                                         *)
(* receipts: Rq entries, receipt_head and receipt_tail counters, a full   *)
(* ring dropping its oldest, and the update's visible window              *)
(* [receipt_visible_head, receipt_visible_tail) clamped at the head when  *)
(* read.  A cell holds the absolute index of the event written into it, so *)
(* a read is exact when cell i % Rq holds i.                               *)
(*                                                                         *)
(* serial: the endpoint's receipt serial, modulo SerialMod (2^32 in the    *)
(* code), stepping to 1 instead of 0.                                      *)
(*                                                                         *)
(* The receipt ring takes events from the sent rings' resolves and from    *)
(* elsewhere, and nothing else connects them, so Part checks each half    *)
(* on its own (see SentNext and ReceiptNext).                             *)
(*                                                                         *)
(* Reversed witnesses (each flag FALSE is a bug class):                    *)
(*   EvictAtP      evict while pkt_tail - pkt_head > P, not >= P           *)
(*   RefTestExact  evict on refs only past max_refs + 1 (off by one)       *)
(*   ResetEmpty    reset ref_head/ref_tail whenever the head moved, not    *)
(*                 only when the ring is empty                             *)
(*   ReceiptClamp  read from receipt_visible_head without the clamp        *)
(*   SerialSkip    the serial wraps to 0                                   *)
(***************************************************************************)
EXTENDS Integers, Sequences, FiniteSets

CONSTANTS
    P,          \* sent_packets entries (packets_in_flight)
    R,          \* references a packet carries at most (16 in the code)
    MaxRefs,    \* sent_refs entries (16P in the code)
    Rq,         \* receipt ring entries (max_message_receipts)
    SerialMod,  \* the serial counter wraps here (2^32 in the code)
    MaxWrites,  \* the bounded exploration's packets
    MaxEvents,  \* the bounded exploration's receipt events
    Part,       \* "sent": the packet and reference rings; "receipt": the receipt ring and the serial
    EvictAtP, RefTestExact, ResetEmpty, ReceiptClamp, SerialSkip

ASSUME P \in Nat \ {0} /\ R \in Nat \ {0} /\ MaxRefs >= R /\ Rq \in Nat \ {0}
ASSUME SerialMod >= 2
ASSUME Part \in {"sent", "receipt"}
ASSUME \A f \in {EvictAtP, RefTestExact, ResetEmpty, ReceiptClamp, SerialSkip} : f \in BOOLEAN

PSlots == 0..(P - 1)
RSlots == 0..(MaxRefs - 1)
QSlots == 0..(Rq - 1)

Blank == [ id |-> FALSE, gid |-> -1, rfirst |-> 0, rcount |-> 0, unrel |-> 0, rcfirst |-> 0 ]
NoRef == [ k |-> -1, j |-> 0, z |-> TRUE ]

VARIABLES
    pk,         \* sent_packets[i]: [id = packet_id # 0, gid ghost write index, ref_first, ref_count, unreliable_receipts, receipt_first]
    ph, pt,     \* pkt_head, pkt_tail
    rf,         \* sent_refs[i]: ghost identity of the reference written there
    rh, rt,     \* ref_head, ref_tail
    rq,         \* receipts[i]: ghost absolute index of the event written there
    qh, qt,     \* receipt_head, receipt_tail
    vh, vt,     \* receipt_visible_head, receipt_visible_tail
    sweep,      \* receipt_sweep
    serial,     \* the endpoint's serial
    dup,        \* ghost: a resolve reported a serial that was not its packet's
    badRead,    \* ghost: a read reported a cell that did not hold its event
    refEvicted, \* ghost: the reference test, not the packet test, evicted
    zeroSerial  \* ghost: a serial of 0 was issued

vars == << pk, ph, pt, rf, rh, rt, rq, qh, qt, vh, vt, sweep, serial,
           dup, badRead, refEvicted, zeroSerial >>

Max(a, b) == IF a >= b THEN a ELSE b

St == [ pk |-> pk, ph |-> ph, pt |-> pt, rf |-> rf, rh |-> rh, rt |-> rt,
        rq |-> rq, qh |-> qh, qt |-> qt, dup |-> dup,
        refEvicted |-> refEvicted ]

SetSt(s) ==
    /\ pk' = s.pk /\ ph' = s.ph /\ pt' = s.pt /\ rf' = s.rf /\ rh' = s.rh /\ rt' = s.rt
    /\ rq' = s.rq /\ qh' = s.qh /\ qt' = s.qt /\ dup' = s.dup
    /\ refEvicted' = s.refEvicted

-----------------------------------------------------------------------------

\* rn_window_ack_receipt_emit: a full ring drops its oldest
EmitE(s) ==
    LET full == s.qt - s.qh = Rq IN
    IF Part # "receipt" THEN s ELSE
    [ s EXCEPT !.qh = IF full THEN s.qh + 1 ELSE s.qh,
               !.rq = [ s.rq EXCEPT ![s.qt % Rq] = s.qt ],
               !.qt = s.qt + 1 ]

\* rn_window_ack_receipt_resolve: each of the entry's
\* serials not yet zeroed has its outcome, then is zeroed (so a late ack or
\* a second resolve stutters).  dup records an outcome for a serial that is
\* not the entry's own: a cell another packet overwrote.
RECURSIVE ResolveFrom(_, _, _)
ResolveFrom(s, e, i) ==
    IF i >= e.unrel THEN s
    ELSE LET c == (e.rcfirst + i) % MaxRefs
             cell == s.rf[c]
             s1 == IF cell.z THEN s
                   ELSE [ EmitE(s) EXCEPT !.dup = s.dup \/ cell.k # e.gid \/ cell.j # e.rcount + i ]
         IN ResolveFrom([ s1 EXCEPT !.rf = [ s1.rf EXCEPT ![c].z = TRUE ] ], e, i + 1)

Resolve(s, k) == ResolveFrom(s, s.pk[k % P], 0)

\* the head entry leaves: its references are released and its fields cleared
DropHead(s) ==
    LET e == s.pk[s.ph % P] IN
    [ s EXCEPT !.rh = s.rh + e.rcount + e.unrel,
               !.pk = [ s.pk EXCEPT ![s.ph % P] = [ e EXCEPT !.rfirst = 0, !.rcount = 0, !.unrel = 0 ] ],
               !.ph = s.ph + 1 ]

\* rn_window_ack_reclaim_head
RECURSIVE ReclaimLoop(_, _)
ReclaimLoop(s, moved) ==
    IF s.ph < s.pt /\ ~s.pk[s.ph % P].id
    THEN ReclaimLoop(DropHead(s), TRUE)
    ELSE IF s.ph = s.pt \/ (~ResetEmpty /\ moved)
         THEN [ s EXCEPT !.rh = 0, !.rt = 0 ]
         ELSE s

Reclaim(s) == ReclaimLoop(s, FALSE)

PacketsFull(s) == IF EvictAtP THEN s.pt - s.ph >= P ELSE s.pt - s.ph > P
RefsFull(s, total) == IF RefTestExact THEN (s.rt - s.rh) + total > MaxRefs
                                      ELSE (s.rt - s.rh) + total > MaxRefs + 1

\* rn_window_ack_commit step 2: evict the oldest
\* entry, resolving a live one's receipts as LOSS, while a ring needs room
RECURSIVE EvictLoop(_, _)
EvictLoop(s, total) ==
    IF (PacketsFull(s) \/ RefsFull(s, total)) /\ s.ph < s.pt
    THEN LET live == s.pk[s.ph % P].id
             s1 == IF live THEN Resolve(s, s.ph) ELSE s
             s2 == [ s1 EXCEPT !.pk = [ s1.pk EXCEPT ![s.ph % P].id = FALSE ],
                               !.refEvicted = s1.refEvicted \/ ~PacketsFull(s) ]
         IN EvictLoop(Reclaim(DropHead(s2)), total)
    ELSE s

\* rn_window_ack_commit step 3: the references at
\* ref_tail onward, then the entry at pkt_tail % P
WriteE(s, count, rc) ==
    LET total == count + rc
        rfirst == s.rt % MaxRefs
        cells == { (s.rt + i) % MaxRefs : i \in 0..(total - 1) }
        pos(c) == CHOOSE i \in 0..(total - 1) : (s.rt + i) % MaxRefs = c
    IN [ s EXCEPT
           !.rf = [ c \in RSlots |-> IF c \in cells THEN [ k |-> s.pt, j |-> pos(c), z |-> FALSE ] ELSE s.rf[c] ],
           !.rt = s.rt + total,
           !.pk = [ s.pk EXCEPT ![s.pt % P] =
                      [ id |-> TRUE, gid |-> s.pt, rfirst |-> rfirst, rcount |-> count,
                        unrel |-> rc, rcfirst |-> (rfirst + count) % MaxRefs ] ],
           !.pt = s.pt + 1 ]

-----------------------------------------------------------------------------

Init ==
    /\ pk = [ i \in PSlots |-> Blank ]
    /\ ph = 0 /\ pt = 0
    /\ rf = [ i \in RSlots |-> NoRef ]
    /\ rh = 0 /\ rt = 0
    /\ rq = [ i \in QSlots |-> -1 ]
    /\ qh = 0 /\ qt = 0 /\ vh = 0 /\ vt = 0
    /\ sweep = 0
    /\ serial = 0
    /\ dup = FALSE
    /\ badRead = FALSE
    /\ refEvicted = FALSE
    /\ zeroSerial = FALSE

\* rn_window_ack_commit: count reliable numbers and
\* rc receipt serials, at least one reference and at most R
Commit(count, rc) ==
    /\ Part = "sent"
    /\ count + rc \in 1..R
    /\ pt < MaxWrites
    /\ SetSt(WriteE(EvictLoop(Reclaim(St), count + rc), count, rc))
    /\ UNCHANGED << vh, vt, sweep, serial, badRead, zeroSerial >>

\* rn_window_ack_on_ack for an id the ring holds:
\* its receipts ACKED, the entry a hole, the head reclaimed
Ack(k) ==
    /\ Part = "sent"
    /\ k \in ph..(pt - 1)
    /\ pk[k % P].id
    /\ pk[k % P].gid = k
    /\ LET s1 == Resolve(St, k)
       IN SetSt(Reclaim([ s1 EXCEPT !.pk = [ s1.pk EXCEPT ![k % P].id = FALSE ] ]))
    /\ UNCHANGED << vh, vt, sweep, serial, badRead, zeroSerial >>

\* rn_window_ack_receipt_sweep: from the clamped
\* cursor, every live entry past the deadline resolves LOSS, stopping at the
\* first live entry before it.  The deadline is fixed and entries are in
\* send order, so the expired entries are those written before some h.
RECURSIVE SweepLoop(_, _, _)
SweepLoop(s, w, h) ==
    IF w < s.pt /\ ~(s.pk[w % P].id /\ s.pk[w % P].gid >= h)
    THEN SweepLoop(IF s.pk[w % P].id THEN Resolve(s, w) ELSE s, w + 1, h)
    ELSE << s, w >>

Sweep(h) ==
    /\ Part = "sent"
    /\ h \in 0..pt
    /\ LET r == SweepLoop(St, Max(sweep, ph), h)
       IN /\ SetSt(r[1])
          /\ sweep' = r[2]
    /\ UNCHANGED << vh, vt, serial, badRead, zeroSerial >>

\* rn_window_ack_receipt_emit from elsewhere: rn_window_ack_receipt_emit_loss
\* (a failed or over-budget unreliable send) and a reliable message's
\* completion in rn_window_ack_on_ack
EmitOther ==
    /\ Part = "receipt"
    /\ qt < MaxEvents
    /\ LET s1 == EmitE(St) IN SetSt(s1)
    /\ UNCHANGED << vh, vt, sweep, serial, badRead, zeroSerial >>

\* rn_window_ack_receipt_begin_update
BeginUpdate ==
    LET h == IF qh < vt THEN vt ELSE qh IN
    /\ Part = "receipt"
    /\ qh' = h
    /\ vh' = h
    /\ vt' = h
    /\ UNCHANGED << pk, ph, pt, rf, rh, rt, rq, qt, sweep, serial, dup, badRead, refEvicted, zeroSerial >>

\* rn_window_ack_receipt_end_update
EndUpdate ==
    /\ Part = "receipt"
    /\ vh' = qh
    /\ vt' = qt
    /\ UNCHANGED << pk, ph, pt, rf, rh, rt, rq, qh, qt, sweep, serial, dup, badRead, refEvicted, zeroSerial >>

\* rn_window_ack_receipts: every visible event
ReadFirst == IF ReceiptClamp /\ vh < qh THEN qh ELSE vh
ReadReceipts ==
    /\ Part = "receipt"
    /\ badRead' = (badRead \/ \E i \in ReadFirst..(vt - 1) : rq[i % Rq] # i)
    /\ UNCHANGED << pk, ph, pt, rf, rh, rt, rq, qh, qt, vh, vt, sweep, serial, dup, refEvicted, zeroSerial >>

\* the next receipt serial: rn_endpoint_finish,
\* rn_endpoint_in_place_finish and the in-place
\* commit of rn_message_object_finish
IssueSerial ==
    LET n == (serial + 1) % SerialMod
        s == IF SerialSkip /\ n = 0 THEN 1 ELSE n
    IN /\ Part = "receipt"
       /\ serial' = s
       /\ zeroSerial' = (zeroSerial \/ s = 0)
       /\ UNCHANGED << pk, ph, pt, rf, rh, rt, rq, qh, qt, vh, vt, sweep, dup, badRead, refEvicted >>

\* The two rings share only the emit: the sent part checks the packet and
\* reference rings with every outcome recorded (the emit's ring write left
\* out), the receipt part the receipt ring and the serial with emits from
\* anywhere.  Checked apart, their product is not explored.
Counts == 0..R
Writes == 0..MaxWrites

Next ==
    \/ \E c \in Counts, r \in Counts : Commit(c, r)
    \/ \E k \in Writes : Ack(k)
    \/ \E h \in Writes : Sweep(h)
    \/ EmitOther
    \/ BeginUpdate
    \/ EndUpdate
    \/ ReadReceipts
    \/ IssueSerial

Spec == Init /\ [][Next]_vars

-----------------------------------------------------------------------------

TypeOK ==
    /\ pk \in [ PSlots -> [ id : BOOLEAN, gid : -1..MaxWrites, rfirst : RSlots,
                            rcount : 0..R, unrel : 0..R, rcfirst : RSlots ] ]
    /\ ph \in Nat /\ pt \in Nat /\ ph <= pt
    /\ rh \in Int /\ rt \in Int
    /\ qh \in Nat /\ qt \in Nat
    /\ serial \in 0..(SerialMod - 1)

RECURSIVE RefSum(_, _)
RefSum(a, b) == IF a >= b THEN 0
                ELSE pk[a % P].rcount + pk[a % P].unrel + RefSum(a + 1, b)

\* each ring within its capacity
RingBounded ==
    /\ pt - ph <= P
    /\ rh <= rt /\ rt - rh <= MaxRefs
    /\ qh <= qt /\ qt - qh <= Rq

\* every entry from pkt_head to pkt_tail is the packet written at that index
SlotsIntact == \A k \in ph..(pt - 1) : pk[k % P].gid = k

\* a live entry's references are still in their cells; the references in use
\* are exactly the entries', from ref_head
RefsIntact ==
    /\ \A k \in ph..(pt - 1) :
         LET e == pk[k % P] IN
         e.id /\ e.gid = k =>
            /\ e.rcfirst = (e.rfirst + e.rcount) % MaxRefs
            /\ \A j \in 0..(e.rcount + e.unrel - 1) :
                 rf[(e.rfirst + j) % MaxRefs].k = k /\ rf[(e.rfirst + j) % MaxRefs].j = j
    /\ rt - rh = RefSum(ph, pt)
    /\ ph < pt => pk[ph % P].rfirst = rh % MaxRefs

\* every outcome a resolve reports is one of its own packet's serials
OutcomesOwn == ~dup

\* every reported event is the one written at its index
ReadExact == ~badRead

\* the serial is never 0 (rn_window_ack_receipt_emit drops serial 0)
SerialNeverZero == ~zeroSerial

\* at the code's sizing (max_refs = R * P) the reference test never fires
RefEvictionIdle == ~refEvicted

-----------------------------------------------------------------------------

(* The view for the sent part's controls.  pkt_head and pkt_tail move in   *)
(* steps of P without changing any slot, and ref_head and ref_tail in      *)
(* steps of max_refs: the view takes each back to its first lap.  A        *)
(* sent_packets slot outside [pkt_head, pkt_tail) and a sent_refs cell no  *)
(* live entry names are dead: every action writes them before it reads     *)
(* them (the invariants check exactly that), so the view leaves them out.  *)
PBase == ph - (ph % P)
RBase == rh - (rh % MaxRefs)
LiveSlot(i) == \E k \in ph..(pt - 1) : k % P = i
LiveCell(c) == \E k \in ph..(pt - 1) :
                 LET e == pk[k % P] IN
                 e.id /\ \E j \in 0..(e.rcount + e.unrel - 1) : (e.rfirst + j) % MaxRefs = c
View ==
    << [ i \in PSlots |-> IF LiveSlot(i) THEN [ pk[i] EXCEPT !.gid = @ - PBase ] ELSE Blank ],
       ph - PBase, pt - PBase,
       [ c \in RSlots |-> IF LiveCell(c) THEN [ rf[c] EXCEPT !.k = @ - PBase ] ELSE NoRef ],
       rh - RBase, rt - RBase,
       Max(sweep, ph) - PBase,
       dup, refEvicted >>
=============================================================================
