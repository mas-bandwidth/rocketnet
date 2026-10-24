---------------------------- MODULE RNSendBudget ----------------------------
(***************************************************************************)
(* rocketnet: the per-connection send budget (owner rule R6; the code is   *)
(* sdk/rocketnet_send_budget.h).                                           *)
(*                                                                         *)
(* Each connection (the client's one, each server slot) has a send budget  *)
(* in kilobits per second, counting the transport's wire bytes per packet. *)
(* It is a token bucket: every update refills it with rate x elapsed time, *)
(* capped at the bucket's depth (the larger of one maximum packet and      *)
(* 100 ms of budget), and every packet written spends its wire bytes.      *)
(*                                                                         *)
(* Time is in ticks; Rate is the refill per tick, W ticks are one window   *)
(* (one second), so the budget per window is Budget = Rate * W.  Bytes are *)
(* abstract units.  A tick is one update: it refills the bucket and, when  *)
(* the packet writer runs (run = TRUE), opens a send phase in which the    *)
(* writer offers queued items class by class:                              *)
(*   class "rtx"   retransmits and ordered progress (an item that advances *)
(*                 the oldest unacked ordered number), first;              *)
(*   class "rel"   new reliable items, second;                             *)
(*   class "unrel" unreliable items, last.                                 *)
(* A reliable item that does not fit waits for a later update and is never *)
(* dropped; while one waits, no lower class spends the bucket.  An         *)
(* unreliable item that does not fit the bytes left to its class is        *)
(* dropped and counted.  The update ends when nothing more can be sent.    *)
(* A tick with run = FALSE is a stalled update: time passes, the writer    *)
(* does not run, and the cap on the bucket is what stops the next update   *)
(* releasing the stalled time as a burst.                                  *)
(*                                                                         *)
(* Acks are the transport's packet acks (owner rule R1): Ack(r) is the     *)
(* transport acking the packet that carried reliable item r; Lose(r) is    *)
(* that packet lost, which queues r again as a retransmit.  Losses are     *)
(* bounded so that liveness is about the budget, not an endless link.      *)
(*                                                                         *)
(* An uncapped connection (SetKbps with a value of 0 or lower) holds no    *)
(* bucket: every item is sent, nothing is dropped or waits by the budget,  *)
(* and NeverOverspend no longer claims anything about it. A positive value *)
(* caps it again, a fresh full bucket; a value above MaxKbps is refused    *)
(* and changes nothing.                                                    *)
(*                                                                         *)
(* Two constants flip the guarding rules, one per reversed witness:        *)
(*   CheckBudget = FALSE   spend without the check: an item is sent        *)
(*                         whether or not it fits, the bucket saturating   *)
(*                         at zero.  Fails NeverOverspend.                 *)
(*   ReliableFirst = FALSE the writer offers unreliable items before the   *)
(*                         reliable classes.  Under a steady unreliable    *)
(*                         load the refill is spent every update before a  *)
(*                         reliable item larger than one tick's refill     *)
(*                         fits.  Fails EveryReliableEventuallySent.       *)
(*   RefundOnFail = FALSE  a packet the transport refuses at finish keeps  *)
(*                         the charge it was admitted at.  Fails           *)
(*                         FailedSendCostsNothing.                         *)
(*                                                                         *)
(* A failed send (SendFailed) is the transport refusing a packet the       *)
(* writer charged: its finish returns no wire bytes.  The charge is        *)
(* refunded whole (settle with actual 0), a reliable item is queued again  *)
(* as a retransmit and an unreliable one is counted failed.  Failures are  *)
(* bounded by MaxFail.                                                     *)
(***************************************************************************)
EXTENDS Naturals, Sequences, FiniteSets

CONSTANTS
    W,              \* ticks per window (one second)
    Rate,           \* refill per tick, in bytes
    Depth,          \* the bucket's depth, in bytes
    MaxPkt,         \* the largest packet's wire bytes
    RelSize,        \* a reliable item's wire bytes
    UnrelSize,      \* an unreliable item's wire bytes
    UPerTick,       \* unreliable items arriving per update, at most
    MaxRel,         \* reliable items that ever arrive (finite arrivals)
    MaxLoss,        \* transport packet losses, at most
    CheckBudget,    \* TRUE: send only what fits (FALSE: BrokenOverspend)
    ReliableFirst,  \* TRUE: the class order (FALSE: BrokenUnreliableFirst)
    RefundOnFail,   \* TRUE: a failed send is refunded whole (FALSE: BrokenNoRefund)
    MaxFail         \* sends the transport refuses at finish, at most

ASSUME /\ W \in Nat \ {0, 1} /\ Rate \in Nat \ {0}
       /\ MaxPkt \in Nat \ {0}
       /\ RelSize \in 1..MaxPkt /\ UnrelSize \in 1..MaxPkt
       \* the depth rule: at least one maximum packet, so every packet fits
       \* an idle bucket (a reliable item never waits forever), and at most a
       \* stated bound, so a stall releases no more than Depth at once
       /\ Depth \in Nat /\ Depth >= MaxPkt
       /\ UPerTick \in Nat /\ MaxRel \in Nat /\ MaxLoss \in Nat
       /\ CheckBudget \in BOOLEAN /\ ReliableFirst \in BOOLEAN
       /\ RefundOnFail \in BOOLEAN /\ MaxFail \in Nat

\* the largest capped rate SetKbps accepts. A definition, not a constant, so
\* the existing witness configurations need no new line.
MaxKbps == 2
\* TRUE: an uncapped connection holds no bucket. A definition so the reversed
\* witness MCRNSendBudgetBrokenUncapped.cfg can replace it with FALSE (an
\* uncapped connection that still consults the bucket) and no other cfg
\* needs a new line.
UncapHonoured == TRUE
Budget == Rate * W
DropCap == 2
Min(a, b) == IF a < b THEN a ELSE b
Max(a, b) == IF a > b THEN a ELSE b

RECURSIVE SeqSum(_)
SeqSum(s) == IF s = <<>> THEN 0 ELSE Head(s) + SeqSum(Tail(s))

Range(s) == { s[i] : i \in DOMAIN s }

VARIABLES
    tokens,     \* the bucket, in bytes
    hist,       \* bytes sent in each of the previous W - 1 ticks, oldest first
    cur,        \* bytes sent in the current tick
    phase,      \* "idle" between updates, "send" while the writer runs
    rtx,        \* queue of reliable ids to retransmit (or ordered progress)
    rel,        \* queue of new reliable ids
    unrel,      \* unreliable items queued for this update
    nextRel,    \* the next reliable id to arrive
    inflight,   \* reliable ids sent, the carrying packet not yet acked or lost
    acked,      \* reliable ids whose carrying packet the transport acked
    losses,     \* transport packet losses so far
    drops,      \* unreliable drops counted (capped at DropCap)
    orderBad,   \* ghost: a send took a class while a higher class waited
    dropBad,    \* ghost: an unreliable drop happened although it fit
    uncapped,   \* TRUE: this connection's budget does not apply
    uncapBad,   \* ghost: an uncapped connection dropped or waited by the budget
    fails,      \* sends the transport refused at finish so far
    failBad     \* ghost: a failed send left tokens or cur other than before it

vars == << tokens, hist, cur, phase, rtx, rel, unrel, nextRel, inflight,
           acked, losses, drops, orderBad, dropBad, uncapped, uncapBad,
           fails, failBad >>

Classes == << "rtx", "rel", "unrel" >>
Order == IF ReliableFirst THEN << "rtx", "rel", "unrel" >>
                          ELSE << "unrel", "rtx", "rel" >>

Queued(c) == CASE c = "rtx"   -> rtx # << >>
               [] c = "rel"   -> rel # << >>
               [] c = "unrel" -> unrel > 0

SizeOf(c) == IF c = "unrel" THEN UnrelSize ELSE RelSize

AnyQueued == \E i \in 1..3 : Queued(Classes[i])

\* the class the writer offers next: the first queued class in its order
NextClass == Order[CHOOSE i \in 1..3 : Queued(Order[i]) /\
                                       \A j \in 1..(i - 1) : ~Queued(Order[j])]

ReliableWaiting == rtx # << >> \/ rel # << >>

\* the bytes the unreliable class may spend: none while a reliable item waits
AvailUnrel == IF ReliableWaiting THEN 0 ELSE tokens

\* the budget does not apply: uncapped, and the model honours it
Bypass == uncapped /\ UncapHonoured

Fits(c) == Bypass \/ ~CheckBudget \/ SizeOf(c) <= tokens

\* a class sent while a class ahead of it in the design order was queued
HigherQueued(c) == \E i, j \in 1..3 : Classes[i] = c /\ j < i /\ Queued(Classes[j])

TypeOK ==
    /\ tokens \in 0..Depth
    /\ hist \in Seq(Nat) /\ Len(hist) = W - 1
    /\ cur \in Nat
    /\ phase \in {"idle", "send"}
    /\ rtx \in Seq(1..MaxRel) /\ rel \in Seq(1..MaxRel)
    /\ unrel \in 0..UPerTick
    /\ nextRel \in 1..(MaxRel + 1)
    /\ inflight \subseteq 1..MaxRel /\ acked \subseteq 1..MaxRel
    /\ losses \in 0..MaxLoss /\ drops \in 0..DropCap
    /\ orderBad \in BOOLEAN /\ dropBad \in BOOLEAN
    /\ uncapped \in BOOLEAN /\ uncapBad \in BOOLEAN
    /\ fails \in 0..MaxFail /\ failBad \in BOOLEAN

Init ==
    /\ tokens = Depth
    /\ hist = [i \in 1..(W - 1) |-> 0]
    /\ cur = 0
    /\ phase = "idle"
    /\ rtx = << >> /\ rel = << >> /\ unrel = 0
    /\ nextRel = 1
    /\ inflight = {} /\ acked = {}
    /\ losses = 0 /\ drops = 0
    /\ orderBad = FALSE /\ dropBad = FALSE
    /\ uncapped = FALSE /\ uncapBad = FALSE
    /\ fails = 0 /\ failBad = FALSE

(***************************************************************************)
(* Tick(run): one update.  refill(now): tokens += Rate, capped at Depth.   *)
(* The window slides by one tick.  With run, the writer runs: unreliable   *)
(* items for this update and at most one new reliable item arrive, and the *)
(* send phase opens.  Without run the update stalls: time passes only.     *)
(***************************************************************************)
Tick(run) ==
    /\ phase = "idle"
    /\ tokens' = Min(Depth, tokens + Rate)
    /\ hist' = Append(Tail(hist), cur)
    /\ cur' = 0
    /\ IF run
         THEN /\ phase' = "send"
              /\ \E k \in 0..UPerTick : unrel' = k
              /\ \/ /\ nextRel <= MaxRel
                    /\ rel' = Append(rel, nextRel)
                    /\ nextRel' = nextRel + 1
                 \/ UNCHANGED << rel, nextRel >>
         ELSE /\ UNCHANGED << phase, unrel, rel, nextRel >>
    /\ UNCHANGED << rtx, inflight, acked, losses, drops, orderBad, dropBad,
                    uncapped, uncapBad, fails, failBad >>

(***************************************************************************)
(* Send: can_send(wire_bytes) then spend(wire_bytes) for the head of the   *)
(* next class.  The reliable classes move their id in flight.              *)
(***************************************************************************)
Send ==
    /\ phase = "send"
    /\ AnyQueued
    /\ LET c == NextClass
           s == SizeOf(c)
       IN /\ Fits(c)
          /\ tokens' = IF Bypass THEN tokens ELSE Max(0, tokens - s)
          /\ cur' = IF Bypass THEN cur ELSE cur + s
          /\ orderBad' = (orderBad \/ HigherQueued(c))
          /\ CASE c = "rtx" ->
                    /\ inflight' = inflight \cup {Head(rtx)}
                    /\ rtx' = Tail(rtx)
                    /\ UNCHANGED << rel, unrel >>
               [] c = "rel" ->
                    /\ inflight' = inflight \cup {Head(rel)}
                    /\ rel' = Tail(rel)
                    /\ UNCHANGED << rtx, unrel >>
               [] c = "unrel" ->
                    /\ unrel' = unrel - 1
                    /\ UNCHANGED << rtx, rel, inflight >>
    /\ UNCHANGED << hist, phase, nextRel, acked, losses, drops, dropBad,
                    uncapped, uncapBad, fails, failBad >>

(***************************************************************************)
(* SendFailed: the head of the next class is admitted and charged as Send  *)
(* charges it, then the transport refuses the packet at finish and nothing *)
(* leaves.  With RefundOnFail the whole charge is settled back (settle     *)
(* with actual 0), so tokens and cur are as they were.  A reliable item is *)
(* queued again as a retransmit, first; an unreliable one is counted       *)
(* failed and gone.  In the code: rn_endpoint_packet_failed,               *)
(* rn_endpoint_in_place_failed and rn_message_flush_unreliable's failure   *)
(* branch.                                                                 *)
(***************************************************************************)
SendFailed ==
    /\ phase = "send"
    /\ AnyQueued
    /\ fails < MaxFail
    /\ LET c == NextClass
           s == SizeOf(c)
           chargedTokens == IF Bypass THEN tokens ELSE Max(0, tokens - s)
           chargedCur == IF Bypass THEN cur ELSE cur + s
       IN /\ Fits(c)
          /\ tokens' = IF RefundOnFail THEN chargedTokens + (tokens - chargedTokens)
                                       ELSE chargedTokens
          /\ cur' = IF RefundOnFail THEN chargedCur - (chargedCur - cur)
                                    ELSE chargedCur
          /\ failBad' = (failBad \/ tokens' # tokens \/ cur' # cur)
          /\ orderBad' = (orderBad \/ HigherQueued(c))
          /\ CASE c = "rtx" ->
                    UNCHANGED << rtx, rel, unrel >>
               [] c = "rel" ->
                    /\ rtx' = << Head(rel) >> \o rtx
                    /\ rel' = Tail(rel)
                    /\ UNCHANGED unrel
               [] c = "unrel" ->
                    /\ unrel' = unrel - 1
                    /\ UNCHANGED << rtx, rel >>
    /\ fails' = fails + 1
    /\ UNCHANGED << hist, phase, nextRel, inflight, acked, losses, drops,
                    dropBad, uncapped, uncapBad >>

(***************************************************************************)
(* Drop: the next class is unreliable and its head does not fit: dropped   *)
(* and counted.                                                            *)
(***************************************************************************)
Drop ==
    /\ phase = "send"
    /\ AnyQueued
    /\ NextClass = "unrel"
    /\ ~Bypass
    /\ ~Fits("unrel")
    /\ unrel' = unrel - 1
    /\ drops' = Min(DropCap, drops + 1)
    /\ dropBad' = (dropBad \/ UnrelSize <= AvailUnrel)
    /\ uncapBad' = (uncapBad \/ uncapped)
    /\ UNCHANGED << tokens, hist, cur, phase, rtx, rel, nextRel, inflight,
                    acked, losses, orderBad, uncapped, fails, failBad >>

(***************************************************************************)
(* EndUpdate: nothing more can be sent: every queue is empty, or the next  *)
(* class is reliable and its head does not fit, so it waits.  The          *)
(* unreliable items still queued behind a waiting reliable item are        *)
(* dropped and counted (the bucket is held for the reliable item).         *)
(***************************************************************************)
EndUpdate ==
    /\ phase = "send"
    /\ IF AnyQueued THEN NextClass # "unrel" /\ ~Fits(NextClass) ELSE TRUE
    /\ phase' = "idle"
    /\ unrel' = 0
    /\ drops' = IF unrel > 0 THEN Min(DropCap, drops + 1) ELSE drops
    /\ dropBad' = (dropBad \/ (unrel > 0 /\ UnrelSize <= AvailUnrel))
    /\ uncapBad' = (uncapBad \/ (uncapped /\ AnyQueued))
    /\ UNCHANGED << tokens, hist, cur, rtx, rel, nextRel, inflight, acked,
                    losses, orderBad, uncapped, fails, failBad >>

(***************************************************************************)
(* Ack(r): the transport acks the packet that carried r (R1).              *)
(* Lose(r): that packet is lost; r is queued again as a retransmit.        *)
(***************************************************************************)
Ack(r) ==
    /\ r \in inflight
    /\ inflight' = inflight \ {r}
    /\ acked' = acked \cup {r}
    /\ UNCHANGED << tokens, hist, cur, phase, rtx, rel, unrel, nextRel,
                    losses, drops, orderBad, dropBad, uncapped, uncapBad,
                    fails, failBad >>

Lose(r) ==
    /\ r \in inflight
    /\ losses < MaxLoss
    /\ inflight' = inflight \ {r}
    /\ rtx' = Append(rtx, r)
    /\ losses' = losses + 1
    /\ UNCHANGED << tokens, hist, cur, phase, rel, unrel, nextRel, acked,
                    drops, orderBad, dropBad, uncapped, uncapBad, fails,
                    failBad >>

(* SetKbps(k): the runtime override. 0 or lower uncaps, a positive value up  *)
(* to MaxKbps caps (a fresh bucket when it was uncapped), above the maximum  *)
(* is refused and changes nothing. In the code this is                         *)
(* rn_endpoint_set_send_kbps.                                                 *)
SetKbps(k) ==
    /\ k \in 0..(MaxKbps + 1)
    /\ \/ /\ k = 0
          /\ uncapped' = TRUE
          /\ hist' = [i \in 1..(W - 1) |-> 0]
          /\ cur' = 0
          /\ UNCHANGED << tokens >>
       \/ /\ k >= 1 /\ k <= MaxKbps
          /\ uncapped' = FALSE
          /\ tokens' = IF uncapped THEN Depth ELSE tokens
          /\ UNCHANGED << hist, cur >>
       \/ /\ k > MaxKbps
          /\ UNCHANGED << tokens, hist, cur, uncapped >>
    /\ UNCHANGED << phase, rtx, rel, unrel, nextRel, inflight, acked,
                    losses, drops, orderBad, dropBad, uncapBad, fails,
                    failBad >>

Next ==
    \/ \E run \in BOOLEAN : Tick(run)
    \/ \E k \in 0..(MaxKbps + 1) : SetKbps(k)
    \/ Send
    \/ SendFailed
    \/ Drop
    \/ EndUpdate
    \/ \E r \in 1..MaxRel : Ack(r) \/ Lose(r)

\* The writer runs infinitely often, the update finishes what it can, and the
\* transport eventually acks every packet in flight.  The environment chooses
\* the arrivals, so a steady unreliable load is one of the behaviours checked.
Fairness ==
    /\ WF_vars(Tick(TRUE))
    /\ WF_vars(Send)
    /\ WF_vars(Drop)
    /\ WF_vars(EndUpdate)
    /\ \A r \in 1..MaxRel : WF_vars(Ack(r))

Spec == Init /\ [][Next]_vars /\ Fairness

(***************************************************************************)
(* Properties.                                                             *)
(***************************************************************************)

\* bytes sent in any window of W consecutive ticks: at most the budget plus
\* the bucket depth. An uncapped connection holds no bucket, so it claims
\* nothing here.
NeverOverspend == uncapped \/ SeqSum(hist) + cur <= Budget + Depth

\* an uncapped connection never drops or waits by the budget: nothing is
\* dropped and no send phase ends with work queued
UncappedNeverDropsOrWaits == ~uncapBad

\* a reliable item that arrived is queued, in flight or acked: never dropped
ReliableNeverDropped ==
    \A r \in 1..(nextRel - 1) :
        r \in Range(rtx) \cup Range(rel) \cup inflight \cup acked

\* an unreliable item is dropped only when it does not fit the bytes its
\* class may spend
UnreliableDroppedOnlyOverBudget == ~dropBad

\* retransmits and ordered progress, then new reliable, then unreliable
ClassOrder == ~orderBad

\* a send the transport refused at finish costs the bucket nothing: tokens
\* and the window's bytes are as they were before it
FailedSendCostsNothing == ~failBad

\* LIVE: with a positive budget and finite reliable arrivals, every reliable
\* item that arrives is eventually sent and its carrying packet acked
EveryReliableEventuallySent ==
    \A r \in 1..MaxRel : (r < nextRel) ~> (r \in acked)

=============================================================================
