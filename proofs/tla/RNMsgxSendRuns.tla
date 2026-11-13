---------------------------- MODULE RNMsgxSendRuns ----------------------------
(***************************************************************************)
(* The macOS batched server transport's send path over one taken bank     *)
(* (sdk/rocketnet_msgx_send_runs.h, called by rocketnet_server_msgx_sort,  *)
(* rocketnet_server_msgx_send_bank and rocketnet_server_msgx_worker in     *)
(* sdk/rocketnet_server_msgx_apple.cpp) at the code's layout: the        *)
(* counting sort's bucket array and its placement loop, the sorted order  *)
(* array, the cursor, the run builder's scan, the partial send's cursor   *)
(* walk, the dead run's resend loop and the worker's bounded retries      *)
(* after quit.                                                             *)
(*                                                                         *)
(* The bank is the input: count packets (num_packets clamped at Size),     *)
(* each with its client (the destination), the stamp the fill thread      *)
(* gave it (table slot, -1 the shared socket, and the slot's generation)   *)
(* and whether it is live (packet_bytes > 0; aborted or never finished     *)
(* otherwise).  The table at send time is which slots are open at which   *)
(* generation; a slot's socket is connected to the client stamped at      *)
(* that slot and generation (0: a client outside this bank).  Main may    *)
(* retire a slot while the worker sends (the resolve then answers no      *)
(* socket; the quarantine of RNMsgxSocketTable keeps a resolved socket    *)
(* open and connected to its peer until the pass ends, so a send already  *)
(* resolved still reaches that peer).  The connected send answers any of *)
(* all taken, a prefix taken, nothing taken or full, refused, dead or     *)
(* another error; the shared send sent, full or dropped.  The bank        *)
(* hand-off is RNBankFifoLayout's and is not modelled here.               *)
(*                                                                         *)
(* The input domain is every stamp assignment the header accepts, wider   *)
(* than the server reaches: within one fill phase a client has one stamp  *)
(* and a slot's generations rise in staged order.  MultiStamp FALSE keeps *)
(* one stamp per client (the fill-phase contract); TRUE lets a client     *)
(* hold two (a bank whose publish was deferred across a table change).    *)
(*                                                                         *)
(* History is kept per packet and per client, not as the wire itself:    *)
(* left[i] counts packet i's departures (sent on either socket, or        *)
(* dropped counted), lastC[c] and lastS[c, stamp] the staged index of the *)
(* last packet of client c (of that stamp) sent, and a flag records each  *)
(* send that broke an order or reached another client.                    *)
(*                                                                         *)
(* The code has StableSort, GenBreak, PartialSkipsAborted and DeadResends *)
(* TRUE; each broken config flips one and must fail its named invariant   *)
(* (README-RNMsgxSendRuns.md).                                             *)
(***************************************************************************)
EXTENDS Integers, Sequences, FiniteSets

CONSTANTS
    Size,               \* ROCKETNET_MSGX_SEND_QUEUE_SIZE
    Batch,              \* ROCKETNET_MSGX_SEND_BATCH
    Cap,                \* the table's capacity: slots 0..Cap-1
    Gens,               \* the generations a stamp or the table holds
    Clients,            \* destinations, ids >= 1 (0: a peer outside the bank)
    Claims,             \* num_packets at the take (above Size: refused claims)
    MultiStamp,         \* TRUE: a client may hold two stamps in one bank
    MaxStalls,          \* bound: answers that keep the bank (the call returns false)
    MaxRetires,         \* bound: slots main retires during the send
    RetriesAfterQuit,   \* ROCKETNET_MSGX_SEND_RETRIES_AFTER_QUIT
    StableSort,         \* TRUE: placement walks the bank forward from each bucket's start
    GenBreak,           \* TRUE: a run ends at a change of generation
    PartialSkipsAborted,\* TRUE: a partial send's cursor counts only live packets as taken
    DeadResends,        \* TRUE: a dead run leaves through the shared socket
    OpenGen(_)          \* the generation each slot is open at when the send starts

Shared == -1
Slots == 0..(Cap - 1)
Stamps == {<< Shared, 0 >>} \cup (Slots \X Gens)
Index == 0..(Size - 1)

VARIABLES
    count,      \* num_packets clamped: rocketnet_server_msgx_worker's count
    client,     \* info[i].to, as a client id
    slot,       \* info[i].table_slot
    gen,        \* info[i].generation
    live,       \* info[i].packet_bytes > 0
    tOpen,      \* the table: slot s resolves at generation tGen[s] while open
    tGen,
    order,      \* worker->order
    pc,         \* the worker's place
    cursor,     \* the cursor of rocketnet_server_msgx_worker
    hd,         \* the run's head packet (head in the header)
    scan,       \* the run builder's scan
    run,        \* run_packets[0..run)
    attempts,   \* the refusal retry
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
vars == << count, client, slot, gen, live, tOpen, tGen, order, pc, cursor, hd, scan, run, attempts,
           hist, quit, retries, stalls, retires >>
bank == << count, client, slot, gen, live, tGen >>

Min(a, b) == IF a < b THEN a ELSE b

\* the slot's socket's peer: the client stamped at its open generation
Peer(s) == IF \E i \in 0..(count - 1) : slot[i] = s /\ gen[i] = tGen[s]
           THEN client[CHOOSE i \in 0..(count - 1) : slot[i] = s /\ gen[i] = tGen[s]]
           ELSE 0

\* rocketnet_msgx_socket_table_resolve answers a descriptor
Resolves(i) == slot[i] >= 0 /\ tOpen[slot[i]] /\ tGen[slot[i]] = gen[i]

StampOf(i) == << slot[i], gen[i] >>

-----------------------------------------------------------------------------
(* rocketnet_msgx_send_runs_sort: the code's three loops over bucket[]    *)
(* (capacity + 2 entries; key 0 the shared socket and any slot outside   *)
(* the table, key s + 1 slot s).                                          *)

Key(i) == IF slot[i] >= 0 /\ slot[i] < Cap THEN slot[i] + 1 ELSE 0

RECURSIVE CountKeys(_, _)
CountKeys(i, b) == IF i = count THEN b ELSE CountKeys(i + 1, [b EXCEPT ![Key(i) + 1] = @ + 1])

RECURSIVE Prefix(_, _)
Prefix(b, k) == IF k = Cap + 1 THEN b ELSE Prefix([b EXCEPT ![k + 1] = @ + b[k]], k + 1)

\* order[bucket[key]++] = i; the broken form order[--bucket[key + 1]] = i
RECURSIVE Place(_, _, _)
Place(i, b, o) ==
    IF i = count THEN o
    ELSE IF StableSort
         THEN Place(i + 1, [b EXCEPT ![Key(i)] = @ + 1], [o EXCEPT ![b[Key(i)]] = i])
         ELSE Place(i + 1, [b EXCEPT ![Key(i) + 1] = @ - 1], [o EXCEPT ![b[Key(i) + 1] - 1] = i])

SortedOrder ==
    LET zero == [k \in 0..(Cap + 1) |-> 0]
        b == Prefix(CountKeys(0, zero), 0)
    IN Place(0, b, [q \in Index |-> 0])

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

\* one staged packet: client, stamp, live
Packets == [c : Clients, st : Stamps, l : BOOLEAN]

Init ==
    /\ \E claimed \in Claims : count = Min(claimed, Size)
    \* packets past the count are never read: one value each
    /\ \E pk \in [0..(count - 1) -> Packets] :
        /\ client = [i \in Index |-> IF i < count THEN pk[i].c ELSE 0]
        /\ slot = [i \in Index |-> IF i < count THEN pk[i].st[1] ELSE Shared]
        /\ gen = [i \in Index |-> IF i < count THEN pk[i].st[2] ELSE 0]
        /\ live = [i \in Index |-> IF i < count THEN pk[i].l ELSE FALSE]
    \* a slot at a generation belongs to one client
    /\ \A i, j \in 0..(count - 1) : (slot[i] >= 0 /\ slot[i] = slot[j] /\ gen[i] = gen[j]) => client[i] = client[j]
    \* one stamp per client, unless MultiStamp
    /\ ~MultiStamp => \A i, j \in 0..(count - 1) : client[i] = client[j] => (slot[i] = slot[j] /\ gen[i] = gen[j])
    \* every slot open at OpenGen(s); stamps at the other generation are
    \* stale, and Retire closes a slot: generations are only compared, so
    \* this covers each resolve answer up to renaming
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

\* rocketnet_server_msgx_sort
Sort ==
    /\ pc = "sort"
    /\ order' = SortedOrder
    /\ pc' = "head"
    /\ UNCHANGED << bank, tOpen, cursor, hd, scan, run, attempts, hist, quit, retries, stalls, retires >>

\* the top of rocketnet_msgx_send_runs_send's loop: the bank done, an
\* aborted head skipped, a head that resolves to no socket sent alone on the
\* shared socket, or the run built
LoopTop ==
    /\ pc = "head"
    /\ IF cursor >= count
       THEN /\ pc' = "done"
            /\ UNCHANGED << cursor, hd, scan, run, attempts, hist, stalls >>
       ELSE LET h == order[cursor] IN
            IF ~live[h]
            THEN /\ cursor' = cursor + 1
                 /\ UNCHANGED << pc, hd, scan, run, attempts, hist, stalls >>
            ELSE IF ~Resolves(h)
            THEN \/ /\ SetH(SendOne(H, h, Shared))
                    /\ cursor' = cursor + 1
                    /\ UNCHANGED << pc, hd, scan, run, attempts, stalls >>
                 \/ /\ SetH(DropOne(H, h))
                    /\ cursor' = cursor + 1
                    /\ UNCHANGED << pc, hd, scan, run, attempts, stalls >>
                 \/ /\ stalls < MaxStalls
                    /\ stalls' = stalls + 1
                    /\ pc' = "kept"
                    /\ UNCHANGED << cursor, hd, scan, run, attempts, hist >>
            ELSE LET b == Build(cursor, << >>, h) IN
                 /\ hd' = h
                 /\ scan' = b[1]
                 /\ run' = b[2]
                 /\ attempts' = 0
                 /\ pc' = "send"
                 /\ UNCHANGED << cursor, hist, stalls >>
    /\ UNCHANGED << bank, tOpen, order, quit, retries, retires >>

\* one connected send of the run and what its answer does
SendAll ==
    /\ pc = "send"
    /\ SetH(SendSeq(H, run, Len(run), Peer(slot[hd])))
    /\ cursor' = scan
    /\ pc' = "head"
    /\ UNCHANGED << bank, tOpen, order, hd, scan, run, attempts, quit, retries, stalls, retires >>

SendPartial ==
    /\ pc = "send"
    /\ stalls < MaxStalls
    /\ \E k \in 1..(Len(run) - 1) :
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

\* the dead run's resend loop, one packet per step
Dead ==
    /\ pc = "dead"
    /\ IF cursor >= scan
       THEN /\ pc' = "head"
            /\ UNCHANGED << cursor, hist, stalls >>
       ELSE LET i == order[cursor] IN
            IF ~live[i]
            THEN /\ cursor' = cursor + 1
                 /\ UNCHANGED << pc, hist, stalls >>
            ELSE \/ /\ SetH(SendOne(H, i, Shared))
                    /\ cursor' = cursor + 1
                    /\ UNCHANGED << pc, stalls >>
                 \/ /\ SetH(DropOne(H, i))
                    /\ cursor' = cursor + 1
                    /\ UNCHANGED << pc, stalls >>
                 \/ /\ stalls < MaxStalls
                    /\ stalls' = stalls + 1
                    /\ pc' = "kept"
                    /\ UNCHANGED << cursor, hist >>
    /\ UNCHANGED << bank, tOpen, order, hd, scan, run, attempts, quit, retries, retires >>

\* rocketnet_server_msgx_worker after a false return: past the retries
\* after quit, every live packet from the cursor is dropped, counted;
\* otherwise the next call starts at the same cursor
Kept ==
    /\ pc = "kept"
    /\ IF quit /\ retries + 1 > RetriesAfterQuit
       THEN /\ left' = [i \in Index |-> left[i] + (IF live[i] /\ \E q \in cursor..(count - 1) : order[q] = i THEN 1 ELSE 0)]
            /\ pc' = "done"
            /\ UNCHANGED retries
       ELSE /\ retries' = IF quit THEN retries + 1 ELSE retries
            /\ pc' = "head"
            /\ UNCHANGED left
    /\ UNCHANGED << bank, tOpen, order, cursor, hd, scan, run, attempts, lastC, lastS, bad, quit, stalls, retires >>

Done ==
    /\ pc = "done"
    /\ UNCHANGED vars

\* the environment: main sets shutdown; main retires a slot (on a forget or
\* a dead report) while the worker sends.  The worker reads shutdown only in
\* Kept and the table only at a resolve in LoopTop, so each environment step
\* is taken only just before that read: every other placement reaches the
\* same reads.
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
    \/ Sort \/ LoopTop \/ SendAll \/ SendPartial \/ SendFull \/ SendRefused \/ SendDead \/ SendError
    \/ Dead \/ Kept \/ Done \/ Quit
    \/ \E s \in Slots : Retire(s)

Spec == Init /\ [][Next]_vars

-----------------------------------------------------------------------------
(* Properties *)

TypeOK ==
    /\ count \in 0..Size
    /\ order \in [Index -> Index]
    /\ pc \in {"sort", "head", "send", "dead", "kept", "done"}
    /\ cursor \in 0..count
    /\ scan \in 0..count
    /\ Len(run) <= Batch
    /\ left \in [Index -> Nat]

\* the sort is a permutation of the bank, the shared socket's packets
\* first, then by slot
SortPermutation ==
    pc # "sort" =>
        /\ {order[q] : q \in 0..(count - 1)} = 0..(count - 1)
        /\ \A q \in 0..(count - 2) : Key(order[q]) <= Key(order[q + 1])

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
