--------------------------- MODULE RNAckApplyPhase ---------------------------
(***************************************************************************)
(* The deferred apply of the server's received acks                        *)
(* (sdk/rocketnet_ack.h and sdk/rocketnet_ack.cpp, rocketnet_server_acks_t *)
(* with the deferral on), the state behind the update in three calls:      *)
(* rocketnet_server_update_receive on the main thread,                     *)
(* rocketnet_server_update_client for each client on the game threads,     *)
(* rocketnet_server_update_finish on the main thread.                      *)
(*                                                                         *)
(* The receive loop queues each received packet's ack block on its         *)
(* client's chain in place of applying it (Receive). The queue is a fixed  *)
(* array filled in arrival order; a client's chain runs through it by      *)
(* index (head, tail, each entry's next), and each entry's ranges lie in a *)
(* shared range store at the entry's offset. When the array or the range   *)
(* store cannot take the next block, everything queued is applied and      *)
(* gathered first, on the main thread, inside the same call. Anything the  *)
(* receive loop does to a client's sent side first applies that client's   *)
(* chain where it stands (Settle). At the receive's end every client with  *)
(* a chain gets its claim set (Open); a thread takes a client by one       *)
(* exchange of that claim and applies its chain (Take), and a call that    *)
(* finds the claim clear does nothing (TakeRefused while open: nothing     *)
(* queued, or taken already; TakeOutside in any other phase). The finish   *)
(* (Close) applies every client still claimed, then gathers: each entry's  *)
(* ids, read from its client's segment at the offset its apply recorded,   *)
(* go into the batch in arrival order, and the queue empties. The one-call *)
(* update is Open then Close with no Take between.                         *)
(*                                                                         *)
(* A block is one id here: applying entry e of client c appends its block  *)
(* to c's segment and records the offset, unless c's endpoint has          *)
(* acknowledged that block already (acked): a duplicate block, queued like *)
(* any other, gives no id (ids_count 0), as rocketnet_ack_endpoint_process_*)
(* block marks only records not yet acknowledged.  The rest of the         *)
(* endpoint's state is a function of the blocks its client applied, in     *)
(* order, and SegmentInOrder says that order is arrival order.  An entry's *)
(* done says it was applied: the code keeps no such word, an entry is      *)
(* applied when it is no longer on its client's chain.                     *)
(*                                                                         *)
(* Receive is the whole of rocketnet_server_acks_receive_block: an empty   *)
(* block (largest 0, here block 0) acks nothing and changes nothing; while *)
(* receiving a block is queued; while closed (the deferral on, no update   *)
(* open) it is applied at once and its id goes straight into the batch     *)
(* (rocketnet_server_acks_apply_inline).  The code applies inline in the   *)
(* open phase too; the model assumes no block arrives then: both callers   *)
(* of rocketnet_server_receive_one are inside rocketnet_server_transport_  *)
(* update_receive, before rocketnet_server_acks_open.  Settle of a client  *)
(* with no chain returns at once, in any phase.                            *)
(*                                                                         *)
(* The layout is the code's: indexes from 0, -1 for none, queue[e + 1] the *)
(* entry at index e, the count the queue's length.                         *)
(*                                                                         *)
(* Reversed witnesses, each a cfg that must fail the invariant named:      *)
(*   DupGivesNothing = FALSE       Published      (BrokenDuplicateSurfaces)*)
(*   ExchangeTake = FALSE          OneTaker       (BrokenLoadTake)         *)
(*   SettleOnlyReceiving = FALSE   OneTaker       (BrokenSettleWhileOpen)  *)
(*   CloseAppliesUntaken = FALSE   Whole          (BrokenCloseSkipsUntaken)*)
(*   CloseClearsClaim = FALSE      ClaimOnlyOpen  (BrokenCloseKeepsClaim)  *)
(*   GatherWhenFull = FALSE        InBounds       (BrokenNoGatherWhenFull) *)
(*   AdvanceTail = FALSE           ChainWhole     (BrokenTailNotAdvanced)  *)
(***************************************************************************)
EXTENDS Integers, Sequences, FiniteSets

CONSTANTS Clients,              \* client indexes
          Threads,              \* the threads that may call the take
          Main,                 \* the main thread, one of Threads
          MinBlock,             \* blocks are MinBlock..MaxBlock (0 is the empty block); any may arrive again
          MaxBlock,
          MaxRanges,            \* the most ranges a block carries
          RangeCounts,          \* the range counts the blocks of this instance carry
          PendingCap,           \* entries the queue holds
          RangesCap,            \* ranges the range store holds
          ExchangeTake,         \* the take clears the claim in the step that reads it
          CloseAppliesUntaken,  \* the gather applies every client that still has a chain
          CloseClearsClaim,     \* the gather clears the claim of each client it applies
          SettleOnlyReceiving,  \* settle runs only while the main thread owns every client
          GatherWhenFull,       \* a block that does not fit gathers first
          AdvanceTail,          \* a queued block becomes its client's tail
          DupGivesNothing       \* a block the endpoint has acknowledged gives no id again

ASSUME /\ Main \in Threads
       /\ MaxRanges <= RangesCap
       /\ RangeCounts \subseteq 0..MaxRanges
       /\ PendingCap >= 1
       /\ MinBlock >= 1

VARIABLES phase,        \* "closed", "receiving" or "open" (acks->phase)
          queue,        \* the queued entries in arrival order (acks->pending[0 .. num_pending))
          numRanges,    \* ranges used in the range store (acks->num_ranges)
          head,         \* client -> index of its first entry not yet applied, -1 for none
          tail,         \* client -> index of its last entry while it has a chain, -1 for none
          claim,        \* client -> 1 while the client waits for a taker
          segment,      \* client -> the ids its applied blocks gave since the last gather, in order
          batch,        \* the published batch: <<client, id>> in order (acks->acked_*[0 .. num_acked))
          took,         \* client -> the threads that applied it since the last Open
          acked,        \* client -> the blocks its endpoint has acknowledged (the sent records marked acked)
          arrived,      \* history: the <<client, block>> that arrived new (not acknowledged, not
                        \* arrived already) since the receive began, in arrival order
          gathered      \* history: what forced the receive's latest early gather, "queue" (the
                        \* queue was full) or "ranges" (only the range store was), "none" before one

vars == << phase, queue, numRanges, head, tail, claim, segment, batch, took, acked, arrived, gathered >>

Blocks == MinBlock..MaxBlock

Entry == [client : Clients, block : Blocks, ranges : RangeCounts, roff : Nat, next : Int, ioff : Nat, icnt : 0..1,
          done : BOOLEAN]

TypeOK ==
    /\ phase \in {"closed", "receiving", "open"}
    /\ queue \in Seq(Entry)
    /\ numRanges \in Nat
    /\ head \in [Clients -> Int]
    /\ tail \in [Clients -> Int]
    /\ claim \in [Clients -> 0..1]
    /\ segment \in [Clients -> Seq(Blocks)]
    /\ batch \in Seq(Clients \X Blocks)
    /\ took \in [Clients -> SUBSET Threads]
    /\ acked \in [Clients -> SUBSET Blocks]
    /\ arrived \in Seq(Clients \X Blocks)
    /\ gathered \in {"none", "queue", "ranges"}

Init ==
    /\ phase = "closed"
    /\ queue = <<>>
    /\ numRanges = 0
    /\ head = [c \in Clients |-> -1]
    /\ tail = [c \in Clients |-> -1]
    /\ claim = [c \in Clients |-> 0]
    /\ segment = [c \in Clients |-> <<>>]
    /\ batch = <<>>
    /\ took = [c \in Clients |-> {}]
    /\ acked = [c \in Clients |-> {}]
    /\ arrived = <<>>
    /\ gathered = "none"

\* the indexes a chain visits from h, following next
Chain(q, h) ==
    LET F[e \in -1..Len(q), n \in 0..Len(q)] ==
            IF e < 0 \/ e >= Len(q) \/ n = 0 THEN <<>> ELSE <<e>> \o F[q[e + 1].next, n - 1]
    IN F[h, Len(q)]

InChain(ch, e) == \E k \in DOMAIN ch : ch[k] = e
Pos(ch, e) == CHOOSE k \in DOMAIN ch : ch[k] = e

\* the entry at chain position k of client c gives an id: its block is one
\* the endpoint has not acknowledged, before or earlier on the chain
\* (DupGivesNothing = FALSE: every applied block gives its id)
Fresh(c, ch, k) ==
    LET b == queue[ch[k] + 1].block
    IN ~DupGivesNothing \/ (b \notin acked[c] /\ \A j \in 1..(k - 1) : queue[ch[j] + 1].block # b)

FreshBefore(c, ch, k) == Cardinality({j \in 1..(k - 1) : Fresh(c, ch, j)})

FreshBlocks(c, ch) ==
    LET F[k \in 0..Len(ch)] ==
            IF k = 0 THEN <<>> ELSE F[k - 1] \o (IF Fresh(c, ch, k) THEN <<queue[ch[k] + 1].block>> ELSE <<>>)
    IN F[Len(ch)]

\* rocketnet_server_acks_apply_queued for each client of S: every entry on the
\* client's chain, in chain order, is applied; one that gives an id puts it
\* at the end of the client's segment, and each records the segment's length
\* before it (ids_offset) and its ids (ids_count, 0 for a duplicate)
QueueApplied(S) ==
    [i \in DOMAIN queue |->
        LET c == queue[i].client
            ch == Chain(queue, head[c])
        IN IF c \in S /\ InChain(ch, i - 1)
              THEN LET k == Pos(ch, i - 1)
                   IN [queue[i] EXCEPT !.ioff = Len(segment[c]) + FreshBefore(c, ch, k),
                                       !.icnt = IF Fresh(c, ch, k) THEN 1 ELSE 0,
                                       !.done = TRUE]
              ELSE queue[i]]

SegmentApplied(S) ==
    [c \in Clients |->
        IF c \in S THEN segment[c] \o FreshBlocks(c, Chain(queue, head[c])) ELSE segment[c]]

\* the endpoint acknowledges every block its chain applied
AckedApplied(S) ==
    [c \in Clients |->
        IF c \in S
           THEN LET ch == Chain(queue, head[c]) IN acked[c] \cup {queue[ch[k] + 1].block : k \in DOMAIN ch}
           ELSE acked[c]]

\* the gather (rocketnet_server_acks_gather): every client that still has a
\* chain is applied, its claim cleared; then each entry's ids go from its
\* client's segment into the batch, in arrival order
Queued == {c \in Clients : head[c] >= 0}
InQueue == {queue[i].client : i \in DOMAIN queue}
GatherSet == IF CloseAppliesUntaken THEN Queued ELSE {}

Ids(q, seg) ==
    LET F[i \in 0..Len(q)] ==
            IF i = 0 THEN <<>>
            ELSE F[i - 1] \o (IF q[i].icnt = 1 THEN << <<q[i].client, seg[q[i].client][q[i].ioff + 1]>> >> ELSE <<>>)
    IN F[Len(q)]

GatherBatch == batch \o Ids(QueueApplied(GatherSet), SegmentApplied(GatherSet))
GatherSegment == [c \in Clients |-> IF c \in InQueue THEN <<>> ELSE segment[c]]
GatherHead == [c \in Clients |-> IF c \in GatherSet THEN -1 ELSE head[c]]
GatherTail == [c \in Clients |-> IF c \in GatherSet THEN -1 ELSE tail[c]]
GatherClaim == [c \in Clients |-> IF c \in Queued /\ CloseClearsClaim THEN 0 ELSE claim[c]]
GatherAcked == AckedApplied(GatherSet)

\* rocketnet_server_acks_begin_receive: the batch empties and blocks queue
BeginReceive ==
    /\ phase = "closed"
    /\ phase' = "receiving"
    /\ batch' = <<>>
    /\ arrived' = <<>>
    /\ gathered' = "none"
    /\ UNCHANGED << queue, numRanges, head, tail, claim, segment, took, acked >>

\* block b of client c arrives new: its endpoint has not acknowledged it and
\* it has not arrived already in this receive
New(c, b) == b \notin acked[c] /\ \A i \in DOMAIN arrived : arrived[i] # <<c, b>>

\* rocketnet_server_acks_receive_block: block b of client c with r ranges.
\* The empty block (b = 0) changes nothing. While receiving, the block is
\* queued at the queue's end and linked at its client's tail; when the queue
\* is full or the range store cannot take r more, everything queued is
\* applied and gathered first. While closed it is applied at once
\* (rocketnet_server_acks_apply_inline) and its id goes into the batch. A
\* duplicate is queued or applied like any other block and gives no id
Receive(c, b, r) ==
    /\ phase \in {"receiving", "closed"}
    /\ IF b = 0
       THEN UNCHANGED vars
       ELSE IF phase = "closed"
       THEN /\ batch' = IF b \notin acked[c] \/ ~DupGivesNothing THEN Append(batch, <<c, b>>) ELSE batch
            /\ acked' = [acked EXCEPT ![c] = @ \cup {b}]
            /\ arrived' = IF New(c, b) THEN Append(arrived, <<c, b>>) ELSE arrived
            /\ UNCHANGED << phase, queue, numRanges, head, tail, claim, segment, took, gathered >>
       ELSE
       LET full == GatherWhenFull /\ (Len(queue) = PendingCap \/ r > RangesCap - numRanges)
           q == IF full THEN <<>> ELSE queue
           hd == IF full THEN GatherHead ELSE head
           tl == IF full THEN GatherTail ELSE tail
           nr == IF full THEN 0 ELSE numRanges
           e == Len(q)
           entry == [client |-> c, block |-> b, ranges |-> r, roff |-> nr, next |-> -1, ioff |-> 0, icnt |-> 0,
                     done |-> FALSE]
       IN /\ queue' = Append(IF hd[c] >= 0 THEN [q EXCEPT ![tl[c] + 1].next = e] ELSE q, entry)
          /\ head' = [hd EXCEPT ![c] = IF hd[c] < 0 THEN e ELSE @]
          /\ tail' = [tl EXCEPT ![c] = IF hd[c] < 0 \/ AdvanceTail THEN e ELSE @]
          /\ numRanges' = nr + r
          /\ batch' = IF full THEN GatherBatch ELSE batch
          /\ segment' = IF full THEN GatherSegment ELSE segment
          /\ claim' = IF full THEN GatherClaim ELSE claim
          /\ acked' = IF full THEN GatherAcked ELSE acked
          /\ arrived' = IF New(c, b) THEN Append(arrived, <<c, b>>) ELSE arrived
          /\ gathered' = IF ~full THEN gathered ELSE IF Len(queue) = PendingCap THEN "queue" ELSE "ranges"
          /\ UNCHANGED << phase, took >>

\* rocketnet_server_acks_settle: the main thread applies client c's chain
\* where it stands, before the receive loop touches c's sent side. Never
\* while the clients are open to the takers. A client with no chain returns
\* at once, in any phase
Settle(c) ==
    IF head[c] < 0
    THEN UNCHANGED vars
    ELSE /\ IF SettleOnlyReceiving THEN phase = "receiving" ELSE phase \in {"receiving", "open"}
         /\ queue' = QueueApplied({c})
         /\ segment' = SegmentApplied({c})
         /\ acked' = AckedApplied({c})
         /\ head' = [head EXCEPT ![c] = -1]
         /\ tail' = [tail EXCEPT ![c] = -1]
         /\ took' = IF phase = "open" THEN [took EXCEPT ![c] = @ \cup {Main}] ELSE took
         /\ UNCHANGED << phase, numRanges, claim, batch, arrived, gathered >>

\* rocketnet_server_acks_open: every client with a chain may be taken
Open ==
    /\ phase = "receiving"
    /\ phase' = "open"
    /\ claim' = [c \in Clients |-> IF head[c] >= 0 THEN 1 ELSE claim[c]]
    /\ took' = [c \in Clients |-> {}]
    /\ UNCHANGED << queue, numRanges, head, tail, segment, batch, acked, arrived, gathered >>

\* rocketnet_server_acks_apply_client that found the claim set: thread t
\* takes client c and applies the n blocks of its chain. The code tests no
\* phase here: the claim is the whole guard
Take(t, c, n) ==
    /\ claim[c] = 1
    /\ n = Len(Chain(queue, head[c]))
    /\ claim' = [claim EXCEPT ![c] = IF ExchangeTake THEN 0 ELSE @]
    /\ queue' = QueueApplied({c})
    /\ segment' = SegmentApplied({c})
    /\ acked' = AckedApplied({c})
    /\ head' = [head EXCEPT ![c] = -1]
    /\ tail' = [tail EXCEPT ![c] = -1]
    /\ took' = [took EXCEPT ![c] = @ \cup {t}]
    /\ UNCHANGED << phase, numRanges, batch, arrived, gathered >>

\* the same call finding the claim clear while open: nothing was queued for
\* the client, or it is taken already
TakeRefused(t, c) ==
    /\ phase = "open"
    /\ claim[c] = 0
    /\ UNCHANGED vars

\* the same call outside the open phase: no claim is set there
TakeOutside(t, c) ==
    /\ phase # "open"
    /\ claim[c] = 0
    /\ UNCHANGED vars

\* rocketnet_server_acks_close: the main thread, after the takers have
\* joined, applies every client still claimed and gathers. The code closes
\* from the receive phase too (no Open: nothing was claimed)
Close ==
    /\ phase \in {"receiving", "open"}
    /\ phase' = "closed"
    /\ batch' = GatherBatch
    /\ segment' = GatherSegment
    /\ head' = GatherHead
    /\ tail' = GatherTail
    /\ claim' = GatherClaim
    /\ acked' = GatherAcked
    /\ queue' = <<>>
    /\ numRanges' = 0
    /\ UNCHANGED << took, arrived, gathered >>

Next ==
    \/ BeginReceive
    \/ Open
    \/ Close
    \/ \E c \in Clients : Settle(c)
    \/ \E t \in Threads, c \in Clients : TakeRefused(t, c) \/ TakeOutside(t, c)
    \/ \E t \in Threads, c \in Clients, n \in 0..Len(queue) : Take(t, c, n)
    \/ \E c \in Clients, b \in {0} \cup Blocks, r \in RangeCounts : Receive(c, b, r)

Spec == Init /\ [][Next]_vars

\* one taker a client in one open phase: the client's sent side, chain,
\* segment and counters are one thread's
OneTaker == \A c \in Clients : Cardinality(took[c]) <= 1

\* a claim stands only while open, and only on a client with a chain: a take
\* outside update_receive .. update_finish finds nothing
ClaimOnlyOpen == \A c \in Clients : claim[c] = 1 => phase = "open" /\ head[c] >= 0

\* outside an update nothing is queued and nothing waits
ClosedEmpty ==
    phase = "closed" =>
        /\ queue = <<>>
        /\ numRanges = 0
        /\ \A c \in Clients : head[c] = -1 /\ tail[c] = -1 /\ claim[c] = 0 /\ segment[c] = <<>>

\* the queued entries that give or gave an id, as <<client, block>> in queue
\* order: an applied one whose apply gave its id, or one not applied yet whose
\* block the endpoint has not acknowledged and no earlier entry carries
Counted(i) ==
    LET x == queue[i]
    IN IF x.done THEN x.icnt = 1
       ELSE x.block \notin acked[x.client]
            /\ \A j \in 1..(i - 1) : <<queue[j].client, queue[j].block>> # <<x.client, x.block>>

CountedPairs ==
    LET F[i \in 0..Len(queue)] ==
            IF i = 0 THEN <<>>
            ELSE F[i - 1] \o (IF Counted(i) THEN << <<queue[i].client, queue[i].block>> >> ELSE <<>>)
    IN F[Len(queue)]

\* nothing lost, nothing twice, nothing out of order: what was published and
\* what is still queued to give an id are, together, what arrived new, in
\* arrival order; a duplicate gives nothing
Whole == arrived = batch \o CountedPairs

\* the result does not depend on who took what: after the finish the batch
\* is the arrival order, which is what applying each block where it arrives
\* publishes
Published == phase = "closed" => batch = arrived

\* the indexes of client c's entries with icnt = k, ascending
Idx(c, k) ==
    LET F[i \in 0..Len(queue)] ==
            IF i = 0 THEN <<>>
            ELSE F[i - 1] \o (IF queue[i].client = c /\ queue[i].icnt = k THEN <<i - 1>> ELSE <<>>)
    IN F[Len(queue)]

\* the indexes of client c's entries not applied yet, ascending
Undone(c) ==
    LET F[i \in 0..Len(queue)] ==
            IF i = 0 THEN <<>>
            ELSE F[i - 1] \o (IF queue[i].client = c /\ ~queue[i].done THEN <<i - 1>> ELSE <<>>)
    IN F[Len(queue)]

\* a client's chain is its unapplied entries in arrival order, and its tail
\* is the last of them
ChainWhole ==
    \A c \in Clients :
        LET ch == Chain(queue, head[c])
        IN /\ ch = Undone(c)
           /\ IF ch = <<>> THEN head[c] = -1 /\ tail[c] = -1 ELSE tail[c] = ch[Len(ch)]

\* a client's segment is its applied entries' ids in arrival order, each at
\* the offset its entry recorded: its blocks were applied once each, in the
\* order they arrived, whoever applied them
SegmentInOrder ==
    \A c \in Clients :
        LET gave == Idx(c, 1)
        IN /\ Len(segment[c]) = Len(gave)
           /\ \A k \in DOMAIN gave : queue[gave[k] + 1].ioff = k - 1 /\ segment[c][k] = queue[gave[k] + 1].block

\* the queue and the range store stay inside their arrays, and each entry's
\* ranges start where the entries before it end
RangesBefore(n) ==
    LET F[i \in 0..Len(queue)] == IF i = 0 THEN 0 ELSE F[i - 1] + queue[i].ranges
    IN F[n]

InBounds ==
    /\ Len(queue) <= PendingCap
    /\ numRanges <= RangesCap
    /\ numRanges = RangesBefore(Len(queue))
    /\ \A i \in DOMAIN queue : queue[i].roff = RangesBefore(i - 1)
=============================================================================
