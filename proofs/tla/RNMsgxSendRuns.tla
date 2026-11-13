---------------------------- MODULE RNMsgxSendRuns ----------------------------
(***************************************************************************)
(* The macOS batched server transport's send path over one taken bank     *)
(* (sdk/rocketnet_msgx_send_runs.h, called by rocketnet_server_msgx_sort,  *)
(* rocketnet_server_msgx_send_bank and rocketnet_server_msgx_worker in     *)
(* sdk/rocketnet_server_msgx_apple.cpp) at the code's layout: the         *)
(* counting sort per fill phase, its bucket array and placement loop, the *)
(* sorted order array, the cursor, the run builder's scan, the partial    *)
(* send's cursor walk, the dead run's resend loop and the worker's        *)
(* bounded retries after quit.  Each action is one call of                *)
(* rocketnet_msgx_send_runs_step (or the sort, or the worker's give-up    *)
(* check), so tests/tla_trace_msgx_send_runs.cpp binds the code to it by  *)
(* a trace check (TraceRNMsgxSendRuns.tla).                               *)
(*                                                                         *)
(* The bank is the input: count packets (num_packets clamped at Size),     *)
(* each with its client (the destination), the stamp the fill thread      *)
(* gave it (table slot, -1 the shared socket, and the slot's generation),  *)
(* the fill phase it was staged in (main's update count, advanced only in *)
(* commit) and whether it is live (packet_bytes > 0; aborted or never     *)
(* finished otherwise).  The table at send time is which slots are open   *)
(* at which generation; a slot's socket is connected to the client        *)
(* stamped at that slot and generation (0: a client outside this bank).  *)
(* Main may retire a slot while the worker sends (the resolve then        *)
(* answers no socket; the quarantine of RNMsgxSocketTable keeps a         *)
(* resolved socket open and connected to its peer until the pass ends, so *)
(* a send already resolved still reaches that peer).  The connected send  *)
(* answers any of all taken, a prefix taken, nothing taken or full,       *)
(* refused, dead or another error; the shared send sent, full or dropped. *)
(* The bank hand-off is RNBankFifoLayout's and is not modelled here.      *)
(*                                                                         *)
(* Fill phases rise in staged order (a fill of the next phase runs after  *)
(* main's commit, after every fill of this one: the fill-phase contract   *)
(* X0), and within one phase a client has one stamp (the table changes   *)
(* only in commit).  Across phases a client's stamp may change: a bank    *)
(* whose publish was deferred across a commit that retired or opened its  *)
(* socket.  MultiStamp FALSE keeps one stamp per client for the whole     *)
(* bank (one phase).  The input domain is otherwise every stamp           *)
(* assignment the header accepts, wider than the server reaches (a slot's *)
(* generations need not rise).                                             *)
(*                                                                         *)
(* History is kept per packet and per client, not as the wire itself:    *)
(* left[i] counts packet i's departures (sent on either socket, or        *)
(* dropped counted), lastC[c] and lastS[c, stamp] the staged index of the *)
(* last packet of client c (of that stamp) sent, and a flag records each  *)
(* send that broke an order or reached another client.                    *)
(*                                                                         *)
(* The code has PhaseSort, StableSort, GenBreak, PartialSkipsAborted and  *)
(* DeadResends TRUE; each broken config flips one and must fail its named *)
(* invariant (README-RNMsgxSendRuns.md).                                   *)
(***************************************************************************)
EXTENDS Integers, Sequences, FiniteSets

CONSTANTS
    Size,               \* ROCKETNET_MSGX_SEND_QUEUE_SIZE
    Batch,              \* ROCKETNET_MSGX_SEND_BATCH
    Cap,                \* the table's capacity: slots 0..Cap-1
    Gens,               \* the generations a stamp or the table holds
    Clients,            \* destinations, ids >= 1 (0: a peer outside the bank)
    Phases,             \* the fill phases a bank's packets carry (naturals)
    Claims,             \* num_packets at the take (above Size: refused claims)
    MultiStamp,         \* TRUE: a client's stamp may change across fill phases
    MaxStalls,          \* bound: answers that keep the bank (the call returns false)
    MaxRetires,         \* bound: slots main retires during the send
    RetriesAfterQuit,   \* ROCKETNET_MSGX_SEND_RETRIES_AFTER_QUIT
    PhaseSort,          \* TRUE: the sort keeps fill phases in staged order, by slot inside each
    StableSort,         \* TRUE: placement walks the bank forward from each bucket's start
    GenBreak,           \* TRUE: a run ends at a change of generation
    PartialSkipsAborted,\* TRUE: a partial send's cursor counts only live packets as taken
    DeadResends,        \* TRUE: a dead run leaves through the shared socket
    OpenGenAt,          \* the generation each slot is open at when the send starts:
                        \* s * GenSpan + g for slot s open at generation g
    Bank                \* {}: every bank Init admits; else the one bank, as the set
                        \* of BankCode(i, packet) over its packets (a trace's input)

Shared == -1
Slots == 0..(Cap - 1)
Stamps == {<< Shared, 0 >>} \cup (Slots \X Gens)
Index == 0..(Size - 1)

ASSUME Gens \subseteq 0..9 /\ Gens # {}
ASSUME Clients \subseteq 1..9 /\ Phases \subseteq 0..9 /\ Cap <= 8

\* the base of OpenGenAt's code: one more than the highest generation
GenSpan == 1 + (CHOOSE m \in Gens : \A x \in Gens : x <= m)

\* every slot is open at exactly one generation
ASSUME \A s \in Slots : Cardinality({g \in Gens : s * GenSpan + g \in OpenGenAt}) = 1

OpenGen(s) == CHOOSE g \in Gens : s * GenSpan + g \in OpenGenAt

VARIABLES
    count,      \* num_packets clamped: rocketnet_server_msgx_worker's count
    client,     \* info[i].to, as a client id
    slot,       \* info[i].table_slot
    gen,        \* info[i].generation
    phase,      \* info[i].phase
    live,       \* info[i].packet_bytes > 0
    tOpen,      \* the table: slot s resolves at generation tGen[s] while open
    tGen,
    order,      \* worker->order
    pc,         \* the send's place (state.pc), "sort" before the sort
    cursor,     \* state.cursor
    hd,         \* state.head: the run's head packet
    scan,       \* state.scan: the run builder's scan
    run,        \* state.run_packets[0..state.run)
    attempts,   \* state.attempts: the refusal retry
    left,       \* left[i]: departures of packet i, sent or dropped
    lastC,      \* lastC[c]: staged index of client c's last packet sent, -1 none
    lastS,      \* lastS[c, st]: the same per stamp
    bad,        \* [cross, client, stamp, dead]: a send to another client, out of
                \* client order, out of stamp order, a dead report on the shared socket
    quit,       \* worker->shutdown
    retries,    \* retries_after_quit
    stalls,     \* bound counter
    retires     \* bound counter

hist == << left, lastC, lastS, bad >>
vars == << count, client, slot, gen, phase, live, tOpen, tGen, order, pc, cursor, hd, scan, run, attempts,
           left, lastC, lastS, bad, quit, retries, stalls, retires >>
bank == << count, client, slot, gen, phase, live, tGen >>

Min(a, b) == IF a < b THEN a ELSE b

\* the slot's socket's peer: the client stamped at its open generation
Peer(s) == IF \E i \in 0..(count - 1) : slot[i] = s /\ gen[i] = tGen[s]
           THEN client[CHOOSE i \in 0..(count - 1) : slot[i] = s /\ gen[i] = tGen[s]]
           ELSE 0

\* rocketnet_msgx_socket_table_resolve answers a descriptor
Resolves(i) == slot[i] >= 0 /\ tOpen[slot[i]] /\ tGen[slot[i]] = gen[i]

StampOf(i) == << slot[i], gen[i] >>

-----------------------------------------------------------------------------
(* rocketnet_msgx_send_runs_sort: the bank cut into runs of one fill      *)
(* phase in staged order (PhaseSort FALSE: one run, the whole bank), and  *)
(* rocketnet_msgx_send_runs_sort_range's three loops over bucket[] on     *)
(* each (capacity + 2 entries; key 0 the shared socket and any slot       *)
(* outside the table, key s + 1 slot s).                                  *)

Key(i) == IF slot[i] >= 0 /\ slot[i] < Cap THEN slot[i] + 1 ELSE 0

\* the end of the phase run that starts at lo
RECURSIVE RangeEnd(_, _)
RangeEnd(lo, hi) == IF hi >= count \/ (PhaseSort /\ phase[hi] # phase[lo]) THEN hi ELSE RangeEnd(lo, hi + 1)

RECURSIVE CountKeys(_, _, _)
CountKeys(i, hi, b) == IF i = hi THEN b ELSE CountKeys(i + 1, hi, [b EXCEPT ![Key(i) + 1] = @ + 1])

RECURSIVE Prefix(_, _)
Prefix(b, k) == IF k = Cap + 1 THEN b ELSE Prefix([b EXCEPT ![k + 1] = @ + b[k]], k + 1)

\* order[lo + bucket[key]++] = i; the broken form order[lo + --bucket[key + 1]] = i
RECURSIVE Place(_, _, _, _, _)
Place(i, lo, hi, b, o) ==
    IF i = hi THEN o
    ELSE IF StableSort
         THEN Place(i + 1, lo, hi, [b EXCEPT ![Key(i)] = @ + 1], [o EXCEPT ![lo + b[Key(i)]] = i])
         ELSE Place(i + 1, lo, hi, [b EXCEPT ![Key(i) + 1] = @ - 1], [o EXCEPT ![lo + b[Key(i) + 1] - 1] = i])

RECURSIVE SortFrom(_, _)
SortFrom(lo, o) ==
    IF lo >= count THEN o
    ELSE LET hi == RangeEnd(lo, lo + 1)
             b == Prefix(CountKeys(lo, hi, [k \in 0..(Cap + 1) |-> 0]), 0)
         IN SortFrom(hi, Place(lo, lo, hi, b, o))

SortedOrder == SortFrom(0, [q \in Index |-> 0])

-----------------------------------------------------------------------------
(* the run builder: from the head, packets of the head's slot and         *)
(* generation, aborted ones skipped, at most Batch live ones              *)

RECURSIVE Build(_, _, _)
Build(sc, r, h) ==
    IF sc >= count \/ Len(r) >= Batch THEN << sc, r >>
    ELSE LET i == order[sc] IN
         IF slot[i] # slot[h] \/ (GenBreak /\ gen[i] # gen[h]) THEN << sc, r >>
         ELSE IF ~live[i] THEN Build(sc + 1, r, h)
         ELSE Build(sc + 1, Append(r, i), h)

\* a partial send took k: the cursor walks past k taken packets
RECURSIVE Advance(_, _, _)
Advance(c, taken, k) ==
    IF taken >= k THEN c
    ELSE Advance(c + 1, IF PartialSkipsAborted /\ ~live[order[c]] THEN taken ELSE taken + 1, k)

-----------------------------------------------------------------------------
(* the history: one packet onto the wire (via a peer, or Shared), or      *)
(* dropped counted                                                         *)

SendOne(h, i, via) ==
    LET c == client[i]
        st == StampOf(i)
    IN [left |-> [h.left EXCEPT ![i] = @ + 1],
        lastC |-> [h.lastC EXCEPT ![c] = i],
        lastS |-> [h.lastS EXCEPT ![c, st] = i],
        bad |-> [h.bad EXCEPT !.cross = @ \/ (via # Shared /\ via # c),
                              !.client = @ \/ h.lastC[c] > i,
                              !.stamp = @ \/ h.lastS[c, st] > i]]

DropOne(h, i) == [h EXCEPT !.left = [@ EXCEPT ![i] = @ + 1]]

RECURSIVE SendSeq(_, _, _, _)
SendSeq(h, r, n, via) == IF n = 0 THEN h ELSE SendOne(SendSeq(h, r, n - 1, via), r[n], via)

RECURSIVE DropSeq(_, _, _)
DropSeq(h, r, n) == IF n = 0 THEN h ELSE DropOne(DropSeq(h, r, n - 1), r[n])

H == [left |-> left, lastC |-> lastC, lastS |-> lastS, bad |-> bad]

SetH(h) == /\ left' = h.left /\ lastC' = h.lastC /\ lastS' = h.lastS /\ bad' = h.bad

-----------------------------------------------------------------------------

\* one staged packet: client, stamp, fill phase, live
Packets == [c : Clients, st : Stamps, ph : Phases, l : BOOLEAN]

\* Bank's code of packet i: one decimal digit per field below the index
BankCode(i, p) ==
    i * 100000 + p.c * 10000 + (p.st[1] + 1) * 1000 + p.st[2] * 100 + p.ph * 10 + (IF p.l THEN 1 ELSE 0)

\* the packets Bank gives, by index
BankPacket(i) ==
    LET x == CHOOSE y \in Bank : y \div 100000 = i
    IN [c |-> (x \div 10000) % 10, st |-> << ((x \div 1000) % 10) - 1, (x \div 100) % 10 >>,
        ph |-> (x \div 10) % 10, l |-> x % 10 = 1]

\* Bank is count packets, one per index, each a packet of the domain
BankGiven ==
    /\ Cardinality(Bank) = count
    /\ \A i \in 0..(count - 1) : \E y \in Bank : y \div 100000 = i
    /\ \A i \in 0..(count - 1) : BankPacket(i) \in Packets /\ BankCode(i, BankPacket(i)) \in Bank

\* the bank variables from the packets pk
LoadBank(pk) ==
    /\ client = [i \in Index |-> IF i < count THEN pk[i].c ELSE 0]
    /\ slot = [i \in Index |-> IF i < count THEN pk[i].st[1] ELSE Shared]
    /\ gen = [i \in Index |-> IF i < count THEN pk[i].st[2] ELSE 0]
    /\ phase = [i \in Index |-> IF i < count THEN pk[i].ph ELSE 0]
    /\ live = [i \in Index |-> IF i < count THEN pk[i].l ELSE FALSE]

\* the bank's rules, over bank variables already given: what the fill
\* threads can stage (TraceRNMsgxSendRuns.tla checks a record against it)
BankOK ==
    \* fill phases rise in staged order
    /\ \A i \in 0..(count - 2) : phase[i] <= phase[i + 1]
    \* a slot at a generation belongs to one client
    /\ \A i, j \in 0..(count - 1) : (slot[i] >= 0 /\ slot[i] = slot[j] /\ gen[i] = gen[j]) => client[i] = client[j]
    \* one stamp per client within a fill phase, and for the whole bank
    \* unless MultiStamp
    /\ \A i, j \in 0..(count - 1) :
        (client[i] = client[j] /\ (phase[i] = phase[j] \/ ~MultiStamp)) => (slot[i] = slot[j] /\ gen[i] = gen[j])

\* the state before the sort, the bank aside: every slot open at
\* OpenGen(s); stamps at the other generation are stale, and Retire closes
\* a slot: generations are only compared, so this covers each resolve
\* answer up to renaming
InitRest ==
    /\ tOpen = [s \in Slots |-> TRUE]
    /\ tGen = [s \in Slots |-> OpenGen(s)]
    /\ order = [q \in Index |-> 0]
    /\ pc = "sort"
    /\ cursor = 0
    /\ hd = 0
    /\ scan = 0
    /\ run = << >>
    /\ attempts = 0
    /\ left = [i \in Index |-> 0]
    /\ lastC = [c \in Clients |-> -1]
    /\ lastS = [x \in Clients \X Stamps |-> -1]
    /\ bad = [cross |-> FALSE, client |-> FALSE, stamp |-> FALSE, dead |-> FALSE]
    /\ quit = FALSE
    /\ retries = 0
    /\ stalls = 0
    /\ retires = 0

\* every bank the fill threads can stage, or (a trace) the one Bank gives
Init ==
    /\ \E claimed \in Claims : count = Min(claimed, Size)
    \* packets past the count are never read: one value each
    /\ IF Bank = {}
       THEN \E pk \in [0..(count - 1) -> Packets] : LoadBank(pk)
       ELSE /\ BankGiven
            /\ LoadBank([i \in 0..(count - 1) |-> BankPacket(i)])
    /\ BankOK
    /\ InitRest

\* rocketnet_server_msgx_sort
Sort ==
    /\ pc = "sort"
    /\ order' = SortedOrder
    /\ pc' = "head"
    /\ UNCHANGED << bank, tOpen, cursor, hd, scan, run, attempts, hist, quit, retries, stalls, retires >>

\* The step at HEAD, one action per outcome: the bank done, an aborted head
\* skipped, a head that resolves to no socket sent alone on the shared
\* socket (sent, dropped, or full and kept), or the run built.

\* the head packet, at a cursor inside the bank
HeadPacket == order[cursor]

\* a live head that resolves to no socket: the shared socket's packet
SharedHead == pc = "head" /\ cursor < count /\ live[HeadPacket] /\ ~Resolves(HeadPacket)

HeadDone ==
    /\ pc = "head"
    /\ cursor >= count
    /\ pc' = "done"
    /\ UNCHANGED << bank, tOpen, order, cursor, hd, scan, run, attempts, hist, quit, retries, stalls, retires >>

HeadSkip ==
    /\ pc = "head"
    /\ cursor < count
    /\ ~live[HeadPacket]
    /\ cursor' = cursor + 1
    /\ UNCHANGED << bank, tOpen, order, pc, hd, scan, run, attempts, hist, quit, retries, stalls, retires >>

HeadShared ==
    /\ SharedHead
    /\ SetH(SendOne(H, HeadPacket, Shared))
    /\ cursor' = cursor + 1
    /\ UNCHANGED << bank, tOpen, order, pc, hd, scan, run, attempts, quit, retries, stalls, retires >>

HeadDropped ==
    /\ SharedHead
    /\ SetH(DropOne(H, HeadPacket))
    /\ cursor' = cursor + 1
    /\ UNCHANGED << bank, tOpen, order, pc, hd, scan, run, attempts, quit, retries, stalls, retires >>

HeadKept ==
    /\ SharedHead
    /\ stalls < MaxStalls
    /\ stalls' = stalls + 1
    /\ pc' = "kept"
    /\ UNCHANGED << bank, tOpen, order, cursor, hd, scan, run, attempts, hist, quit, retries, retires >>

HeadRun ==
    /\ pc = "head"
    /\ cursor < count
    /\ live[HeadPacket]
    /\ Resolves(HeadPacket)
    /\ LET b == Build(cursor, << >>, HeadPacket) IN
       /\ hd' = HeadPacket
       /\ scan' = b[1]
       /\ run' = b[2]
    /\ attempts' = 0
    /\ pc' = "send"
    /\ UNCHANGED << bank, tOpen, order, cursor, hist, quit, retries, stalls, retires >>

\* the step at SEND: one connected send of the run and what its answer does
SendAll ==
    /\ pc = "send"
    /\ SetH(SendSeq(H, run, Len(run), Peer(slot[hd])))
    /\ cursor' = scan
    /\ pc' = "head"
    /\ UNCHANGED << bank, tOpen, order, hd, scan, run, attempts, quit, retries, stalls, retires >>

\* a prefix of k taken: k is the send's answer
SendPartial(k) ==
    /\ pc = "send"
    /\ stalls < MaxStalls
    /\ k \in 1..(Len(run) - 1)
    /\ SetH(SendSeq(H, run, k, Peer(slot[hd])))
    /\ cursor' = Advance(cursor, 0, k)
    /\ stalls' = stalls + 1
    /\ pc' = "kept"
    /\ UNCHANGED << bank, tOpen, order, hd, scan, run, attempts, quit, retries, retires >>

\* nothing taken (0) or full: the run kept
SendFull ==
    /\ pc = "send"
    /\ stalls < MaxStalls
    /\ stalls' = stalls + 1
    /\ pc' = "kept"
    /\ UNCHANGED << bank, tOpen, order, cursor, hd, scan, run, attempts, hist, quit, retries, retires >>

SendRefused ==
    /\ pc = "send"
    /\ IF attempts = 0
       THEN /\ attempts' = 1
            /\ UNCHANGED << cursor, pc, hist >>
       ELSE /\ SetH(DropSeq(H, run, Len(run)))
            /\ cursor' = scan
            /\ pc' = "head"
            /\ UNCHANGED attempts
    /\ UNCHANGED << bank, tOpen, order, hd, scan, run, quit, retries, stalls, retires >>

SendDead ==
    /\ pc = "send"
    /\ bad' = [bad EXCEPT !.dead = @ \/ slot[hd] < 0]
    /\ IF DeadResends
       THEN /\ pc' = "dead"
            /\ UNCHANGED cursor
       ELSE /\ cursor' = scan
            /\ pc' = "head"
    /\ UNCHANGED << bank, tOpen, order, hd, scan, run, attempts, left, lastC, lastS, quit, retries, stalls, retires >>

SendError ==
    /\ pc = "send"
    /\ SetH(DropSeq(H, run, Len(run)))
    /\ cursor' = scan
    /\ pc' = "head"
    /\ UNCHANGED << bank, tOpen, order, hd, scan, run, attempts, quit, retries, stalls, retires >>

\* The step at DEAD, the dead run's resend loop, one packet per step, one
\* action per outcome: the run done, an aborted packet skipped, a packet
\* sent on the shared socket, dropped, or full and kept.

\* a live packet of the dead run at the cursor
DeadLive == pc = "dead" /\ cursor < scan /\ live[order[cursor]]

DeadDone ==
    /\ pc = "dead"
    /\ cursor >= scan
    /\ pc' = "head"
    /\ UNCHANGED << bank, tOpen, order, cursor, hd, scan, run, attempts, hist, quit, retries, stalls, retires >>

DeadSkip ==
    /\ pc = "dead"
    /\ cursor < scan
    /\ ~live[order[cursor]]
    /\ cursor' = cursor + 1
    /\ UNCHANGED << bank, tOpen, order, pc, hd, scan, run, attempts, hist, quit, retries, stalls, retires >>

DeadShared ==
    /\ DeadLive
    /\ SetH(SendOne(H, order[cursor], Shared))
    /\ cursor' = cursor + 1
    /\ UNCHANGED << bank, tOpen, order, pc, hd, scan, run, attempts, quit, retries, stalls, retires >>

DeadDropped ==
    /\ DeadLive
    /\ SetH(DropOne(H, order[cursor]))
    /\ cursor' = cursor + 1
    /\ UNCHANGED << bank, tOpen, order, pc, hd, scan, run, attempts, quit, retries, stalls, retires >>

DeadKept ==
    /\ DeadLive
    /\ stalls < MaxStalls
    /\ stalls' = stalls + 1
    /\ pc' = "kept"
    /\ UNCHANGED << bank, tOpen, order, cursor, hd, scan, run, attempts, hist, quit, retries, retires >>

\* rocketnet_server_msgx_worker after a false return
\* (rocketnet_msgx_send_runs_give_up: shutdown && ++retries > limit): past
\* the retries after quit it gives up (GiveUp), every live packet from the
\* cursor dropped, counted; otherwise the next call resumes at the same
\* cursor (Resume, rocketnet_msgx_send_runs_resume).  The count rises on
\* every check after quit, the last one too.
GiveUp ==
    /\ pc = "kept"
    /\ quit /\ retries + 1 > RetriesAfterQuit
    /\ retries' = retries + 1
    /\ left' = [i \in Index |-> left[i] + (IF live[i] /\ \E q \in cursor..(count - 1) : order[q] = i THEN 1 ELSE 0)]
    /\ pc' = "done"
    /\ UNCHANGED << bank, tOpen, order, cursor, hd, scan, run, attempts, lastC, lastS, bad, quit, stalls, retires >>

Resume ==
    /\ pc = "kept"
    /\ ~(quit /\ retries + 1 > RetriesAfterQuit)
    /\ retries' = IF quit THEN retries + 1 ELSE retries
    /\ pc' = "head"
    /\ UNCHANGED << bank, tOpen, order, cursor, hd, scan, run, attempts, hist, quit, stalls, retires >>

Done ==
    /\ pc = "done"
    /\ UNCHANGED vars

\* the environment: main sets shutdown; main retires a slot (on a forget or
\* a dead report) while the worker sends.  The worker reads shutdown only in
\* GiveUp and Resume and the table only at a resolve at HEAD, so each step
\* of the environment is taken only just before that read: every other
\* placement reaches the same reads.
Quit ==
    /\ pc = "kept"
    /\ ~quit
    /\ quit' = TRUE
    /\ UNCHANGED << bank, tOpen, order, pc, cursor, hd, scan, run, attempts, hist, retries, stalls, retires >>

Retire(s) ==
    /\ pc = "head"
    /\ retires < MaxRetires
    /\ tOpen[s]
    /\ tOpen' = [tOpen EXCEPT ![s] = FALSE]
    /\ retires' = retires + 1
    /\ UNCHANGED << bank, order, pc, cursor, hd, scan, run, attempts, hist, quit, retries, stalls >>

Next ==
    \/ Sort
    \/ HeadDone \/ HeadSkip \/ HeadShared \/ HeadDropped \/ HeadKept \/ HeadRun
    \/ SendAll \/ \E k \in 1..(Batch - 1) : SendPartial(k)
    \/ SendFull \/ SendRefused \/ SendDead \/ SendError
    \/ DeadDone \/ DeadSkip \/ DeadShared \/ DeadDropped \/ DeadKept
    \/ GiveUp \/ Resume \/ Done \/ Quit
    \/ \E s \in Slots : Retire(s)

Spec == Init /\ [][Next]_vars

-----------------------------------------------------------------------------
(* Properties *)

TypeOK ==
    /\ count \in 0..Size
    /\ phase \in [Index -> Phases \cup {0}]
    /\ order \in [Index -> Index]
    /\ pc \in {"sort", "head", "send", "dead", "kept", "done"}
    /\ cursor \in 0..count
    /\ scan \in 0..count
    /\ Len(run) <= Batch
    /\ left \in [Index -> Nat]

\* the sort is a permutation of the bank, in fill phase order, the shared
\* socket's packets first inside each phase, then by slot
SortPermutation ==
    pc # "sort" =>
        /\ {order[q] : q \in 0..(count - 1)} = 0..(count - 1)
        /\ \A q \in 0..(count - 2) :
            \/ phase[order[q]] < phase[order[q + 1]]
            \/ phase[order[q]] = phase[order[q + 1]] /\ Key(order[q]) <= Key(order[q + 1])

\* every finished packet leaves exactly once (sent connected, sent shared or
\* dropped counted), and an aborted packet never leaves
LeavesOnce ==
    \A i \in 0..(count - 1) :
        /\ live[i] => left[i] <= 1
        /\ ~live[i] => left[i] = 0
        /\ (pc = "done" /\ live[i]) => left[i] = 1

\* a connected send reaches the packet's own client
NoCrossClient == ~bad.cross

\* per client, the wire carries packets in staged order
ClientOrder == ~bad.client

\* per client and stamp, the wire carries packets in staged order
StampOrder == ~bad.stamp

\* the cursor never passes a finished packet that has not left
CursorSafe ==
    pc # "sort" => \A q \in 0..(cursor - 1) : live[order[q]] => left[order[q]] = 1

\* a kept bank resumes exactly where it stopped: nothing at or past the
\* cursor has left
ResumeExact ==
    pc = "kept" => \A q \in cursor..(count - 1) : left[order[q]] = 0

\* a dead report names a connected stamp, never the shared socket
DeadReportsConnected == ~bad.dead


=============================================================================
