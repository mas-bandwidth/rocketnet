---------------------------- MODULE RNScheduler ----------------------------
(***************************************************************************)
(* rocketnet: layer 6's scheduler, the order in which queued items are     *)
(* first written into packets (docs/CHANNELS.md 9.2).  Two pieces of code  *)
(* make it: rn_ordered_write in sdk/rocketnet_channel.cpp numbers new      *)
(* messages, the ready channels taking turns, and plans each packet's      *)
(* items; rn_packet_builder_write in sdk/rocketnet_packet_builder.cpp     *)
(* orders the planned items.                                              *)
(*                                                                         *)
(* The turns across packets (rn_ordered_write, action Number): a channel   *)
(* joins the back of the ready list when its FIFO goes from empty to       *)
(* non-empty (rn_channels_send's rn_ready_push).  Each numbering turn      *)
(* takes the ready list's head channel, numbers its FIFO head, pops it     *)
(* and the channel rejoins the back if it has more.  Numbering stops while *)
(* the window is closed (W numbers outstanding: numbered, not yet acked)   *)
(* or the call's room is planned; the retransmits are planned first, so    *)
(* they take room before any new number.                                   *)
(*                                                                         *)
(* The builder's order over the planned items (action Write), three rules  *)
(* nested, outermost first:                                                *)
(*   1. The send budget's class order (owner rule R6): retransmits and     *)
(*      ordered progress ("rtx"), then new reliable items ("rel"), then    *)
(*      unreliable items ("unrel").  A lower class is taken only when no   *)
(*      higher class has an item it may write now, whatever the            *)
(*      priorities.                                                        *)
(*   2. The four priority levels inside a class: the level with the       *)
(*      smallest virtual clock goes next and its clock advances by its     *)
(*      stride, 1, 5, 14, 35 for IMMEDIATE, HIGH, MEDIUM, LOW.  A level    *)
(*      whose clock fell behind the last level taken is pulled forward to  *)
(*      it.  The clocks reset to their seeds at the end of each builder    *)
(*      call whose plan was all taken (no packet refused): the plan holds  *)
(*      only the lost and the numbered items, so the reset comes while     *)
(*      messages still wait queued behind a stalled window or the room,    *)
(*      and, the endpoint calling rn_ordered_write once per packet, in     *)
(*      the middle of an update.  Write resets when a reliable write       *)
(*      leaves no item numbered or lost.  A loss arriving between two      *)
(*      writes of one call can only delay the model's reset, never add    *)
(*      one.                                                               *)
(*   3. Round robin across channels inside a level, one item per turn.     *)
(*                                                                         *)
(* Priorities in v1.1.x: every reliable message is MEDIUM                  *)
(* (rn_channels_send and the endpoint's routes set                         *)
(* ROCKETNET_PRIORITY_MEDIUM; the public API takes no priority) and only   *)
(* CONFIG, at the head of channel 0, is IMMEDIATE.  The other levels are   *)
(* reserved: the builder supports them, no caller sets them.  The code's   *)
(* instance is MCRNScheduler.cfg; MCRNSchedulerReservedLevels.cfg checks   *)
(* the builder's contract at the reserved levels.                          *)
(*                                                                         *)
(* Channel FIFO: within each reliable class, only a channel's head is      *)
(* offered, at the best priority among that class's channel items. The     *)
(* "rel" head is the channel's earliest numbered item.  The "rtx" head is  *)
(* the lost item earliest in the resend FIFO, resendQ: the caller's order  *)
(* there is rn_ordered_walk's walk of that FIFO, and every send moves its  *)
(* number to the FIFO's tail (rn_resend_fifo_push_tail), so the head is    *)
(* the least recently sent lost item, not the lowest id.                   *)
(* The implementation refines the head and boost into one flat sequence    *)
(* per call: contiguous channel/class ranges, with the suffix priority     *)
(* minimum at each range position. The plan is zeroed on the call's stack *)
(* each call, so no earlier call's entry adds a candidate to Cands, and    *)
(* the plan does not change this module's traces.                          *)
(*                                                                         *)
(* The window stall: a new item takes a message number when it is          *)
(* numbered, and at most W numbers are outstanding.  A full window stops   *)
(* numbering; it never holds back "rtx" (already numbered) or "unrel"      *)
(* (never numbered, I13).  In the endpoint a stalled window with nothing   *)
(* due ends the reliable phase (rn_endpoint_ordered_due false), and        *)
(* rn_endpoint_end_reliable starts the unreliable phase unless the budget  *)
(* refused a reliable packet: the unreliable phase runs while budget       *)
(* remains (UnreliableNotBlockedByWindow).                                 *)
(*                                                                         *)
(* The budget: Cap items per update.  An update ends when nothing more can *)
(* be numbered or written; the unreliable items it did not write are       *)
(* dropped and counted, the reliable ones wait.  UPerTick unreliable items *)
(* arrive with every update: a steady unreliable load.                     *)
(*                                                                         *)
(* Acks are the transport's packet acks (owner rule R1): Ack(i) is the     *)
(* transport acking the packet that carried reliable item i; Lose(i) is    *)
(* that packet lost, which queues i again as a retransmit.  Losses are     *)
(* bounded.                                                                *)
(*                                                                         *)
(* The reliable items are a constant scenario, Msgs, queued by the         *)
(* application in that order; each names its channel and priority level.  *)
(* An item's ordering on its channel is its position among the items of    *)
(* that channel.  Refill is a channel that keeps sending: an item of       *)
(* Refill is queued again at its channel's tail once acked (Requeue), an   *)
(* endless stream on a bounded state.                                      *)
(*                                                                         *)
(* Constants flip the guarding rules for the reversed witnesses:           *)
(*   ChannelFIFO = FALSE     per-message priority: every numbered or lost  *)
(*                           item is offered at its own level, not only    *)
(*                           the channel's head.  Fails                    *)
(*                           FirstWriteInOrderPerChannel (BrokenPer-       *)
(*                           MessagePriority, reserved levels) and         *)
(*                           RtxHeadBoost (BrokenRtxPerMessage).           *)
(*   RtxSendOrder = FALSE    the "rtx" head is the lost item with the      *)
(*                           lowest id, not the least recently sent.       *)
(*                           Fails RtxHeadBoost (BrokenRtxById).           *)
(*   UnreliableLast = FALSE  the unreliable class is offered first.        *)
(*                           Fails NoStarvation.                           *)
(*   StallEndsPass = TRUE    a stalled window ends the update's pass, as   *)
(*                           a budget refusal does: the unreliable items   *)
(*                           are dropped with budget left.  Fails          *)
(*                           UnreliableNotBlockedByWindow.                 *)
(*   ReadyRotate = FALSE     the numbered channel keeps the ready list's   *)
(*                           head while it has more, instead of rejoining  *)
(*                           the back.  Under Refill, fails NoStarvation   *)
(*                           for the other channel's item.                 *)
(***************************************************************************)
EXTENDS Naturals, Sequences, FiniteSets

CONSTANTS
    NC,             \* channels, numbered 0..NC-1
    Msgs,           \* the reliable items, in queue order: [ch, lv]
    Refill,         \* items queued again once acked: a channel that keeps sending
    ULevel,         \* the unreliable items' priority level
    W,              \* the window: numbers outstanding, at most
    Cap,            \* items one update may write (the budget)
    UPerTick,       \* unreliable items arriving with every update
    MaxLoss,        \* transport packet losses, at most
    ChannelFIFO,    \* TRUE: only a channel's head is offered (FALSE: BrokenPerMessagePriority, BrokenRtxPerMessage)
    RtxSendOrder,   \* TRUE: the rtx head is the least recently sent lost item (FALSE: BrokenRtxById)
    UnreliableLast, \* TRUE: the class order (FALSE: BrokenUnreliableStarvesOrdered)
    StallEndsPass,  \* FALSE: a stalled window leaves the unreliable phase to run (TRUE: BrokenStallEndsPass)
    ReadyRotate     \* TRUE: a numbered channel rejoins the ready list's back (FALSE: BrokenReadyKeepsTurn)

Levels == 0..3
N == Len(Msgs)
Ids == 1..N
Chans == 0..(NC - 1)

ASSUME /\ NC \in Nat \ {0}
       /\ \A i \in Ids : Msgs[i].ch \in Chans /\ Msgs[i].lv \in Levels
       /\ Refill \subseteq Ids
       /\ ULevel \in Levels
       /\ W \in Nat \ {0} /\ Cap \in Nat \ {0}
       /\ UPerTick \in Nat /\ MaxLoss \in Nat
       /\ ChannelFIFO \in BOOLEAN /\ RtxSendOrder \in BOOLEAN /\ UnreliableLast \in BOOLEAN
       /\ StallEndsPass \in BOOLEAN /\ ReadyRotate \in BOOLEAN

\* The stride and seed per level
Pow2(p) == CASE p = 0 -> 1 [] p = 1 -> 2 [] p = 2 -> 4 [] p = 3 -> 8
Stride(p) == Pow2(p) * (p + 1) + p          \* 1, 5, 14, 35
Seed(p) == Pow2(p) * p + p                  \* 0, 3, 10, 27
Seeds == [p \in Levels |-> Seed(p)]
MaxClock == 35

Min(a, b) == IF a < b THEN a ELSE b
Without(q, i) == SelectSeq(q, LAMBDA j : j # i)
Max(a, b) == IF a > b THEN a ELSE b
Range(s) == { s[k] : k \in 1..Len(s) }

VARIABLES
    sent,       \* items the application has queued: Msgs[1..sent]
    st,         \* per item: "none", "queued", "numbered", "inflight", "lost", "acked"
    q,          \* per channel: its FIFO of queued, not yet numbered items
    ready,      \* the ready list: channels with queued items, in turn order
    numQ,       \* numbered items not yet first-written, in numbering order
    clock,      \* per level: the virtual clock, relative to the last take
    rr,         \* the channel the last item was taken from
    unrelQ,     \* unreliable items queued for this update
    budget,     \* items this update may still write
    losses,     \* transport packet losses so far
    drops,      \* unreliable drops counted (capped)
    written,    \* ghost: per channel, the orderings first-written, in order
    classBad,   \* ghost: a write took a class while a higher class had an item
    unrelHeld,  \* ghost: an update ended with unreliable items and budget left
    resendQ     \* the resend FIFO: written, unacked reliable items, least recently sent first

vars == << sent, st, q, ready, numQ, clock, rr, unrelQ, budget, losses, drops,
           written, classBad, unrelHeld, resendQ >>

DropCap == 2

DesignOrder == << "rtx", "rel", "unrel" >>
Order == IF UnreliableLast THEN DesignOrder ELSE << "unrel", "rtx", "rel" >>

\* the ordering of item i on its channel: its position among that channel's items
Ordering(i) == Cardinality({ j \in Ids : j <= i /\ Msgs[j].ch = Msgs[i].ch }) - 1

Outstanding == Cardinality({ i \in Ids : st[i] \in {"numbered", "inflight", "lost"} })
WindowOpen == Outstanding < W

\* a numbering turn is due: the ready head has a FIFO head and the window is
\* closed to it (rn_window_send_first_write refuses: send->stalled)
Stalled == ready # << >> /\ ~WindowOpen

\* the call's room: the retransmits and the numbered items already planned
\* take it first (rn_ordered_walk, then the numbering loop while planned < room)
Planned == Cardinality({ i \in Ids : st[i] \in {"numbered", "lost"} })
CanNumber == ready # << >> /\ WindowOpen /\ Planned < budget

\* an item's place in numQ (defined for every numbered item)
NumPos(i) == CHOOSE k \in 1..Len(numQ) : numQ[k] = i

\* a channel's "rel" head: its earliest numbered item
IsHead(i) == st[i] = "numbered" /\
             \A j \in Ids : (j # i /\ st[j] = "numbered" /\ Msgs[j].ch = Msgs[i].ch) => NumPos(i) < NumPos(j)

\* the boost: the best (lowest) level among a channel's numbered items
Boost(c) == CHOOSE l \in Levels :
                /\ \E j \in Ids : st[j] = "numbered" /\ Msgs[j].ch = c /\ Msgs[j].lv = l
                /\ \A j \in Ids : (st[j] = "numbered" /\ Msgs[j].ch = c) => l <= Msgs[j].lv

\* an item's place in the resend FIFO (defined for every lost item: it was
\* written and is not acked, so it is in resendQ)
FifoPos(i) == CHOOSE k \in 1..Len(resendQ) : resendQ[k] = i

\* the C++ rtx group head: the channel's lost item earliest in the resend FIFO
FifoHead(i) == st[i] = "lost" /\
               \A j \in Ids : (j # i /\ st[j] = "lost" /\ Msgs[j].ch = Msgs[i].ch) => FifoPos(i) < FifoPos(j)

\* retransmits have their own per-channel head and boost within the rtx class;
\* RtxSendOrder = FALSE is the reversed witness that orders by id instead
RtxHead(i) == IF RtxSendOrder
              THEN FifoHead(i)
              ELSE st[i] = "lost" /\
                   \A j \in Ids : (j < i /\ Msgs[j].ch = Msgs[i].ch) => st[j] # "lost"
RtxBoost(c) == CHOOSE l \in Levels :
                   /\ \E j \in Ids : st[j] = "lost" /\ Msgs[j].ch = c /\ Msgs[j].lv = l
                   /\ \A j \in Ids : (st[j] = "lost" /\ Msgs[j].ch = c) => l <= Msgs[j].lv

\* the items class c may write now, as [id, ch, lv]; id 0 is an unreliable item.
\* The window is not a gate here: it gates numbering, and a numbered item is
\* admitted.  StallEndsPass = TRUE is the reversed witness that lets a stall
\* end the pass before the unreliable phase.
Cands(c) ==
    CASE c = "rtx" ->
            IF ChannelFIFO
            THEN { [id |-> i, ch |-> Msgs[i].ch, lv |-> RtxBoost(Msgs[i].ch)] :
                   i \in { j \in Ids : RtxHead(j) } }
            ELSE { [id |-> i, ch |-> Msgs[i].ch, lv |-> Msgs[i].lv] :
                   i \in { j \in Ids : st[j] = "lost" } }
      [] c = "rel" ->
            IF ChannelFIFO
            THEN { [id |-> i, ch |-> Msgs[i].ch, lv |-> Boost(Msgs[i].ch)] : i \in { j \in Ids : IsHead(j) } }
            ELSE { [id |-> i, ch |-> Msgs[i].ch, lv |-> Msgs[i].lv] : i \in { j \in Ids : st[j] = "numbered" } }
      [] c = "unrel" ->
            IF unrelQ > 0 /\ ~(StallEndsPass /\ Stalled)
            THEN { [id |-> 0, ch |-> 0, lv |-> ULevel] } ELSE {}

\* source contract: a retransmit group offers only its resend-FIFO head at its
\* best level.  Stated from resendQ (FifoHead), not from RtxHead, so it checks
\* what Cands("rtx") offers against the C++ walk order.  Under the design
\* constants (ChannelFIFO, RtxSendOrder) Cands is built from the same
\* definitions and it holds by construction: there it guards Cands against
\* drift, and it bites only in MCRNSchedulerBrokenRtxPerMessage.cfg (each lost
\* item at its own level) and MCRNSchedulerBrokenRtxById.cfg (head by id).
RtxHeadBoost ==
    Cands("rtx") = { [id |-> i, ch |-> Msgs[i].ch, lv |-> RtxBoost(Msgs[i].ch)] :
                     i \in { j \in Ids : FifoHead(j) } }

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
    /\ st \in [Ids -> {"none", "queued", "numbered", "inflight", "lost", "acked"}]
    /\ q \in [Chans -> Seq(Ids)]
    /\ ready \in Seq(Chans)
    /\ numQ \in Seq(Ids)
    /\ clock \in [Levels -> 0..MaxClock]
    /\ rr \in Chans
    /\ unrelQ \in 0..UPerTick
    /\ budget \in 0..Cap
    /\ losses \in 0..MaxLoss
    /\ drops \in 0..DropCap
    /\ written \in [Chans -> Seq(Nat)]
    /\ \A c \in Chans : Len(written[c]) <= N
    /\ classBad \in BOOLEAN
    /\ unrelHeld \in BOOLEAN
    /\ resendQ \in Seq(Ids)

\* the ready list holds each channel with queued items once, and no other
\* (rn_ready_push on empty -> non-empty, rejoin or leave at each turn)
ReadyIsQueued ==
    /\ \A a, b \in 1..Len(ready) : a # b => ready[a] # ready[b]
    /\ Range(ready) = { c \in Chans : q[c] # << >> }

Init ==
    /\ sent = 0
    /\ st = [i \in Ids |-> "none"]
    /\ q = [c \in Chans |-> << >>]
    /\ ready = << >>
    /\ numQ = << >>
    /\ clock = Seeds
    /\ rr = NC - 1
    /\ unrelQ = UPerTick
    /\ budget = Cap
    /\ losses = 0
    /\ drops = 0
    /\ written = [c \in Chans |-> << >>]
    /\ classBad = FALSE
    /\ unrelHeld = FALSE
    /\ resendQ = << >>

\* item i joins its channel's FIFO tail; the channel joins the ready list's
\* back when its FIFO was empty (rn_channels_send: was_empty -> rn_ready_push)
Enqueue(i) ==
    LET c == Msgs[i].ch
    IN /\ st' = [st EXCEPT ![i] = "queued"]
       /\ q' = [q EXCEPT ![c] = Append(@, i)]
       /\ ready' = IF q[c] = << >> THEN Append(ready, c) ELSE ready

(***************************************************************************)
(* Queue: the application queues the next item of the scenario.  CONFIG    *)
(* (rn_ordered_announce_config) is the scenario's first item when it is    *)
(* in it: queued at the connect, before any message, so its push to the    *)
(* front of channel 0 and of the turns is this append.                     *)
(***************************************************************************)
Queue ==
    /\ sent < N
    /\ sent' = sent + 1
    /\ Enqueue(sent + 1)
    /\ UNCHANGED << numQ, clock, rr, unrelQ, budget, losses, drops, written, classBad,
                    unrelHeld, resendQ >>

(***************************************************************************)
(* Requeue(i): the channel that keeps sending queues item i again once it  *)
(* was acked.                                                              *)
(***************************************************************************)
Requeue(i) ==
    /\ i \in Refill
    /\ st[i] = "acked"
    /\ Enqueue(i)
    /\ UNCHANGED << sent, numQ, clock, rr, unrelQ, budget, losses, drops, written,
                    classBad, unrelHeld, resendQ >>

(***************************************************************************)
(* Number: rn_ordered_write's numbering turn.  The ready head channel's    *)
(* FIFO head takes a number (rn_window_send_first_write), is popped        *)
(* (rn_channel_queue_pop), and the channel leaves the turns and rejoins    *)
(* the back if it has more (rn_ready_remove_named, rn_ready_push).  Every  *)
(* number is planned now: the room is the budget left, after the           *)
(* retransmits.                                                            *)
(***************************************************************************)
Number ==
    /\ CanNumber
    /\ LET c == Head(ready)
           i == Head(q[c])
           rest == Tail(q[c])
       IN /\ st' = [st EXCEPT ![i] = "numbered"]
          /\ q' = [q EXCEPT ![c] = rest]
          /\ numQ' = Append(numQ, i)
          /\ ready' = IF rest = << >> THEN Tail(ready)
                      ELSE IF ReadyRotate THEN Append(Tail(ready), c)
                      ELSE ready
    /\ UNCHANGED << sent, clock, rr, unrelQ, budget, losses, drops, written, classBad,
                    unrelHeld, resendQ >>

(***************************************************************************)
(* Write: the builder takes the next item, once the call's numbering is    *)
(* done.  The level taken becomes the clocks' origin: its clock advances   *)
(* by its stride, every other clock is measured from the taken one and a   *)
(* clock behind it is pulled forward to zero (the pull-forward).  A        *)
(* reliable item written, first or resent, moves to the resend FIFO's tail *)
(* (rn_window_ack_commit -> rn_resend_fifo_push_tail); an ack unlinks it   *)
(* (rn_resend_fifo_unlink).  A reliable write that leaves no item numbered *)
(* or lost spends the call's plan, and the clocks go back to the seeds     *)
(* (rn_packet_builder_write's reset at the end of a call whose every       *)
(* group range was taken), whatever is still queued behind a stalled       *)
(* window or the room.                                                     *)
(***************************************************************************)
Write ==
    /\ ~CanNumber
    /\ CanWrite
    /\ LET x == Pick
           c == Order[ClassIdx]
           e == clock[x.lv]
           \* the call's plan (rn_ordered_write: the lost and the numbered
           \* items) is spent by this write: rn_packet_builder_write's reset
           drained == c # "unrel" /\
                      \A j \in Ids : j # x.id => st[j] \notin {"numbered", "lost"}
       IN
       /\ classBad' = (classBad \/ \E k \in 1..3 :
                            DesignOrder[k] = c /\ \E j \in 1..(k - 1) : HasItem(DesignOrder[j]))
       /\ clock' = IF drained
                   THEN Seeds
                   ELSE [p \in Levels |-> IF p = x.lv THEN Stride(p) ELSE Max(clock[p] - e, 0)]
       /\ rr' = x.ch
       /\ budget' = budget - 1
       /\ IF c = "unrel"
          THEN /\ unrelQ' = unrelQ - 1
               /\ UNCHANGED << st, numQ, written, resendQ >>
          ELSE /\ st' = [st EXCEPT ![x.id] = "inflight"]
               /\ resendQ' = Append(Without(resendQ, x.id), x.id)
               /\ unrelQ' = unrelQ
               /\ IF c = "rel"
                  THEN /\ numQ' = Without(numQ, x.id)
                       \* the ghost is kept for a finite scenario only: under
                       \* Refill an item's ordering repeats
                       /\ written' = IF Refill = {}
                                     THEN [written EXCEPT ![x.ch] = Append(@, Ordering(x.id))]
                                     ELSE written
                  ELSE /\ numQ' = numQ
                       /\ written' = written
    /\ UNCHANGED << sent, q, ready, losses, drops, unrelHeld >>

(***************************************************************************)
(* Tick: the update ends when nothing more can be numbered or written.     *)
(* The unreliable items left are dropped and counted, the budget refills,  *)
(* the next update's unreliable items arrive.  The clocks are left as they *)
(* are: their reset is per builder call, in Write.                         *)
(***************************************************************************)
Tick ==
    /\ ~CanNumber
    /\ ~CanWrite
    /\ unrelHeld' = (unrelHeld \/ (unrelQ > 0 /\ budget > 0))
    /\ drops' = Min(drops + unrelQ, DropCap)
    /\ unrelQ' = UPerTick
    /\ budget' = Cap
    /\ UNCHANGED << sent, st, q, ready, numQ, clock, rr, losses, written, classBad, resendQ >>

Ack(i) ==
    /\ st[i] = "inflight"
    /\ st' = [st EXCEPT ![i] = "acked"]
    /\ resendQ' = Without(resendQ, i)
    /\ UNCHANGED << sent, q, ready, numQ, clock, rr, unrelQ, budget, losses, drops, written,
                    classBad, unrelHeld >>

Lose(i) ==
    /\ st[i] = "inflight"
    /\ losses < MaxLoss
    /\ st' = [st EXCEPT ![i] = "lost"]
    /\ losses' = losses + 1
    /\ UNCHANGED << sent, q, ready, numQ, clock, rr, unrelQ, budget, drops, written, classBad,
                    unrelHeld, resendQ >>

Next ==
    \/ Queue
    \/ Number
    \/ Write
    \/ Tick
    \/ \E i \in Ids : Ack(i) \/ Lose(i) \/ Requeue(i)

\* The application queues its items, the code numbers and writes what it can,
\* updates keep coming, and the transport eventually acks every packet in
\* flight.  Requeue is the application's choice and has no fairness.
Fairness ==
    /\ WF_vars(Queue)
    /\ WF_vars(Number)
    /\ WF_vars(Write)
    /\ WF_vars(Tick)
    /\ \A i \in Ids : WF_vars(Ack(i))

Spec == Init /\ [][Next]_vars /\ Fairness

(***************************************************************************)
(* Properties.                                                             *)
(***************************************************************************)

\* within each channel, first writes follow the ordering
FirstWriteInOrderPerChannel ==
    \A c \in Chans : \A k \in 1..Len(written[c]) : written[c][k] = k - 1

\* the code's shape: a closed window ends the reliable phase, and the
\* unreliable phase still runs while budget remains; an update ends with
\* unreliable items dropped only when its budget is spent
UnreliableNotBlockedByWindow == ~unrelHeld

\* the class order is the outer rule: no write ever took a class while a
\* higher class had an item it could write, whatever the priorities
ClassOrderBeforePriority == ~classBad

\* LIVE: every queued reliable item is eventually first-written.  Per item,
\* so per channel: under Refill one channel always has an item queued, and
\* the other channel's items still get their turn.
NoStarvation == \A i \in Ids : (st[i] = "queued") ~> (st[i] \in {"inflight", "lost", "acked"})

=============================================================================
