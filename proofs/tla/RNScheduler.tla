---------------------------- MODULE RNScheduler ----------------------------
(***************************************************************************)
(* rocketnet: layer 6's scheduler, the order in which queued items are     *)
(* first written into packets (docs/CHANNELS.md 9.2; the code is           *)
(* rn_packet_builder_write in sdk/rocketnet_packet_builder.cpp).           *)
(*                                                                         *)
(* Three rules nest, outermost first:                                      *)
(*   1. The send budget's class order (owner rule R6): retransmits and     *)
(*      ordered progress ("rtx"), then new reliable items ("rel"), then    *)
(*      unreliable items ("unrel").  A lower class is taken only when no   *)
(*      higher class has an item it may write now, whatever the            *)
(*      priorities.                                                        *)
(*   2. The four priority levels inside a class: the level with the       *)
(*      smallest virtual clock goes next and its clock advances by its     *)
(*      stride, 1, 5, 14, 35 for IMMEDIATE, HIGH, MEDIUM, LOW.  A level    *)
(*      whose clock fell behind the last level taken is pulled forward to  *)
(*      it.  When nothing is queued at all the clocks reset to their      *)
(*      seeds.                                                             *)
(*   3. Round robin across channels inside a level, one item per turn.     *)
(*                                                                         *)
(* Channel FIFO: a channel's new reliable items are first written in the   *)
(* order the application queued them.  Only a channel's head is offered,   *)
(* at the best priority among the channel's queued items: per-send         *)
(* priority boosts the whole channel (17.3 item 3).                        *)
(* The implementation refines IsHead/Boost into one stable flat sequence  *)
(* per call: contiguous channel/class ranges, with the suffix priority     *)
(* minimum at each range position. The plan is zeroed on the call's stack *)
(* each call, so no earlier call's entry adds a candidate to Cands, and    *)
(* the plan does not change this module's traces.                          *)
(*                                                                         *)
(* The window stall: a new reliable item takes a message number when it   *)
(* is first written, and at most W numbers are outstanding (numbered, not  *)
(* yet acked).  A full window holds the "rel" class back; it never holds   *)
(* back "rtx" (already numbered) or "unrel" (never numbered, I13).         *)
(*                                                                         *)
(* The budget: Cap items per update.  An update ends when nothing more can *)
(* be written; the unreliable items it did not write are dropped and       *)
(* counted, the reliable ones wait.  UPerTick unreliable items arrive with *)
(* every update: a steady unreliable load.                                 *)
(*                                                                         *)
(* Acks are the transport's packet acks (owner rule R1): Ack(i) is the     *)
(* transport acking the packet that carried reliable item i; Lose(i) is    *)
(* that packet lost, which queues i again as a retransmit.  Losses are     *)
(* bounded.                                                                *)
(*                                                                         *)
(* The reliable items are a constant scenario, Msgs, queued by the         *)
(* application in that order; each names its channel and priority level.  *)
(* An item's ordering on its channel is its position among the items of    *)
(* that channel.                                                           *)
(*                                                                         *)
(* Two constants flip the guarding rules, one per reversed witness:        *)
(*   ChannelFIFO = FALSE     per-message priority: every queued item is    *)
(*                           offered at its own level, not only the        *)
(*                           channel's head.  Fails                        *)
(*                           FirstWriteInOrderPerChannel.                  *)
(*   UnreliableLast = FALSE  the unreliable class is offered first.  Under *)
(*                           a steady unreliable load the budget is spent  *)
(*                           every update before any reliable item.        *)
(*                           Fails NoStarvation.                           *)
(***************************************************************************)
EXTENDS Naturals, Sequences, FiniteSets

CONSTANTS
    NC,             \* channels, numbered 0..NC-1
    Msgs,           \* the reliable items, in queue order: [ch, lv]
    ULevel,         \* the unreliable items' priority level
    W,              \* the window: numbers outstanding, at most
    Cap,            \* items one update may write (the budget)
    UPerTick,       \* unreliable items arriving with every update
    MaxLoss,        \* transport packet losses, at most
    ChannelFIFO,    \* TRUE: only a channel's head is offered (FALSE: BrokenPerMessagePriority)
    UnreliableLast  \* TRUE: the class order (FALSE: BrokenUnreliableStarvesOrdered)

Levels == 0..3
N == Len(Msgs)
Ids == 1..N

ASSUME /\ NC \in Nat \ {0}
       /\ \A i \in Ids : Msgs[i].ch \in 0..(NC - 1) /\ Msgs[i].lv \in Levels
       /\ ULevel \in Levels
       /\ W \in Nat \ {0} /\ Cap \in Nat \ {0}
       /\ UPerTick \in Nat /\ MaxLoss \in Nat
       /\ ChannelFIFO \in BOOLEAN /\ UnreliableLast \in BOOLEAN

\* The stride and seed per level
Pow2(p) == CASE p = 0 -> 1 [] p = 1 -> 2 [] p = 2 -> 4 [] p = 3 -> 8
Stride(p) == Pow2(p) * (p + 1) + p          \* 1, 5, 14, 35
Seed(p) == Pow2(p) * p + p                  \* 0, 3, 10, 27
Seeds == [p \in Levels |-> Seed(p)]
MaxClock == 35

Min(a, b) == IF a < b THEN a ELSE b
Max(a, b) == IF a > b THEN a ELSE b

VARIABLES
    sent,       \* items the application has queued: Msgs[1..sent]
    st,         \* per item: "none", "queued", "inflight", "lost", "acked"
    clock,      \* per level: the virtual clock, relative to the last take
    rr,         \* the channel the last item was taken from
    unrelQ,     \* unreliable items queued for this update
    budget,     \* items this update may still write
    losses,     \* transport packet losses so far
    drops,      \* unreliable drops counted (capped)
    written,    \* ghost: per channel, the orderings first-written, in order
    classBad    \* ghost: a write took a class while a higher class had an item

vars == << sent, st, clock, rr, unrelQ, budget, losses, drops, written,
           classBad >>

DropCap == 2

DesignOrder == << "rtx", "rel", "unrel" >>
Order == IF UnreliableLast THEN DesignOrder ELSE << "unrel", "rtx", "rel" >>

\* the ordering of item i on its channel: its position among that channel's items
Ordering(i) == Cardinality({ j \in Ids : j <= i /\ Msgs[j].ch = Msgs[i].ch }) - 1

Outstanding == Cardinality({ i \in Ids : st[i] \in {"inflight", "lost"} })
WindowOpen == Outstanding < W

\* a channel's head: its earliest queued item
IsHead(i) == st[i] = "queued" /\
             \A j \in Ids : (j < i /\ Msgs[j].ch = Msgs[i].ch) => st[j] # "queued"

\* the boost: the best (lowest) level among a channel's queued items
Boost(c) == CHOOSE l \in Levels :
                /\ \E j \in Ids : st[j] = "queued" /\ Msgs[j].ch = c /\ Msgs[j].lv = l
                /\ \A j \in Ids : (st[j] = "queued" /\ Msgs[j].ch = c) => l <= Msgs[j].lv

\* the items class c may write now, as [id, ch, lv]; id 0 is an unreliable item
Cands(c) ==
    CASE c = "rtx" ->
            { [id |-> i, ch |-> Msgs[i].ch, lv |-> Msgs[i].lv] : i \in { j \in Ids : st[j] = "lost" } }
      [] c = "rel" ->
            IF ~WindowOpen THEN {}
            ELSE IF ChannelFIFO
                 THEN { [id |-> i, ch |-> Msgs[i].ch, lv |-> Boost(Msgs[i].ch)] : i \in { j \in Ids : IsHead(j) } }
                 ELSE { [id |-> i, ch |-> Msgs[i].ch, lv |-> Msgs[i].lv] : i \in { j \in Ids : st[j] = "queued" } }
      [] c = "unrel" ->
            IF unrelQ > 0 THEN { [id |-> 0, ch |-> 0, lv |-> ULevel] } ELSE {}

HasItem(c) == Cands(c) # {}

\* 1. the class: the first class in the order with an item it may write
ClassIdx == CHOOSE k \in 1..3 : HasItem(Order[k]) /\ \A j \in 1..(k - 1) : ~HasItem(Order[j])

\* 2. the level: the smallest clock among the levels the class offers, ties
\* to the more urgent level
PickLevel(S) == CHOOSE l \in { x.lv : x \in S } :
                    \A x \in S : clock[l] < clock[x.lv] \/ (clock[l] = clock[x.lv] /\ l <= x.lv)

\* 3. the channel: round robin from the channel after rr; within one channel
\* at one level (only when ChannelFIFO is off) the earliest item
RRDist(c) == (c + NC - rr - 1) % NC
PickIn(S) == CHOOSE x \in S :
                \A y \in S : RRDist(x.ch) < RRDist(y.ch) \/ (x.ch = y.ch /\ x.id <= y.id)

CanWrite == budget > 0 /\ \E k \in 1..3 : HasItem(Order[k])

Pick ==
    LET S == Cands(Order[ClassIdx])
        l == PickLevel(S)
    IN PickIn({ x \in S : x.lv = l })

TypeOK ==
    /\ sent \in 0..N
    /\ st \in [Ids -> {"none", "queued", "inflight", "lost", "acked"}]
    /\ clock \in [Levels -> 0..MaxClock]
    /\ rr \in 0..(NC - 1)
    /\ unrelQ \in 0..UPerTick
    /\ budget \in 0..Cap
    /\ losses \in 0..MaxLoss
    /\ drops \in 0..DropCap
    /\ written \in [0..(NC - 1) -> Seq(Nat)]
    /\ classBad \in BOOLEAN

Init ==
    /\ sent = 0
    /\ st = [i \in Ids |-> "none"]
    /\ clock = Seeds
    /\ rr = NC - 1
    /\ unrelQ = UPerTick
    /\ budget = Cap
    /\ losses = 0
    /\ drops = 0
    /\ written = [c \in 0..(NC - 1) |-> << >>]
    /\ classBad = FALSE

(***************************************************************************)
(* Queue(i): the application queues the next item of the scenario.         *)
(***************************************************************************)
Queue ==
    /\ sent < N
    /\ sent' = sent + 1
    /\ st' = [st EXCEPT ![sent + 1] = "queued"]
    /\ UNCHANGED << clock, rr, unrelQ, budget, losses, drops, written, classBad >>

(***************************************************************************)
(* Write: the builder takes the next item.  The level taken becomes the    *)
(* clocks' origin: its clock advances by its stride, every other clock is  *)
(* measured from the taken one and a clock behind it is pulled forward to  *)
(* zero (the pull-forward).                                                *)
(***************************************************************************)
Write ==
    /\ CanWrite
    /\ LET x == Pick
           c == Order[ClassIdx]
           e == clock[x.lv]
       IN
       /\ classBad' = (classBad \/ \E k \in 1..3 :
                            DesignOrder[k] = c /\ \E j \in 1..(k - 1) : HasItem(DesignOrder[j]))
       /\ clock' = [p \in Levels |-> IF p = x.lv THEN Stride(p) ELSE Max(clock[p] - e, 0)]
       /\ rr' = x.ch
       /\ budget' = budget - 1
       /\ IF c = "unrel"
          THEN /\ unrelQ' = unrelQ - 1
               /\ UNCHANGED << st, written >>
          ELSE /\ st' = [st EXCEPT ![x.id] = "inflight"]
               /\ unrelQ' = unrelQ
               /\ IF c = "rel"
                  THEN written' = [written EXCEPT ![x.ch] = Append(@, Ordering(x.id))]
                  ELSE written' = written
    /\ UNCHANGED << sent, losses, drops >>

(***************************************************************************)
(* Tick: the update ends when nothing more can be written.  The unreliable *)
(* items left are dropped and counted, the budget refills, the next        *)
(* update's unreliable items arrive.  With nothing queued at all the       *)
(* clocks reset to the seeds.                                              *)
(***************************************************************************)
Tick ==
    /\ ~CanWrite
    /\ drops' = Min(drops + unrelQ, DropCap)
    /\ unrelQ' = UPerTick
    /\ budget' = Cap
    /\ clock' = IF \A i \in Ids : st[i] # "queued" /\ st[i] # "lost" THEN Seeds ELSE clock
    /\ UNCHANGED << sent, st, rr, losses, written, classBad >>

Ack(i) ==
    /\ st[i] = "inflight"
    /\ st' = [st EXCEPT ![i] = "acked"]
    /\ UNCHANGED << sent, clock, rr, unrelQ, budget, losses, drops, written, classBad >>

Lose(i) ==
    /\ st[i] = "inflight"
    /\ losses < MaxLoss
    /\ st' = [st EXCEPT ![i] = "lost"]
    /\ losses' = losses + 1
    /\ UNCHANGED << sent, clock, rr, unrelQ, budget, drops, written, classBad >>

Next ==
    \/ Queue
    \/ Write
    \/ Tick
    \/ \E i \in Ids : Ack(i) \/ Lose(i)

\* The application queues its items, the builder writes what it can, updates
\* keep coming, and the transport eventually acks every packet in flight.
Fairness ==
    /\ WF_vars(Queue)
    /\ WF_vars(Write)
    /\ WF_vars(Tick)
    /\ \A i \in Ids : WF_vars(Ack(i))

Spec == Init /\ [][Next]_vars /\ Fairness

(***************************************************************************)
(* Properties.                                                             *)
(***************************************************************************)

\* within each channel, first writes follow the ordering
FirstWriteInOrderPerChannel ==
    \A c \in 0..(NC - 1) : \A k \in 1..Len(written[c]) : written[c][k] = k - 1

\* a closed window never stops an unreliable item that has budget
UnreliableNotBlockedByWindow ==
    (budget > 0 /\ unrelQ > 0 /\ ~WindowOpen) => CanWrite

\* the class order is the outer rule: no write ever took a class while a
\* higher class had an item it could write, whatever the priorities
ClassOrderBeforePriority == ~classBad

\* LIVE: every queued reliable item is eventually first-written
NoStarvation == \A i \in Ids : (st[i] = "queued") ~> (st[i] \in {"inflight", "lost", "acked"})

=============================================================================
