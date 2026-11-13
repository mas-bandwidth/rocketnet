--------------------------- MODULE RNMsgxSocketTable ---------------------------
(***************************************************************************)
(* The macOS batched server transport's per-client socket table           *)
(* (sdk/rocketnet_msgx_socket_table.h) at the code's layout: the slot     *)
(* word (generation, descriptor + 1), the holders' pass counters (even    *)
(* quiescent, odd inside a pass), the snapshot taken at a retire, the     *)
(* retired list compacted in place by the sweep, the free stack, the      *)
(* open-addressing address index with its backward-shift delete, the      *)
(* direct-mapped cooldown table, the update's open budget and the eight   *)
(* counters.  The kernel is a pool of NFd descriptors handed out lowest   *)
(* free first (POSIX), each mapped to the peer its socket is connected    *)
(* to, refusing a second socket connected to the same peer (EADDRINUSE,   *)
(* measured).  That reuse is what makes a close too early a cross-client  *)
(* send: the descriptor a holder resolved is reissued to another client.  *)
(*                                                                         *)
(* Threads.  Main runs Mark, Forget (its retire in two steps, word then   *)
(* snapshot, then the caller's poller deregistration), BeginUpdate and    *)
(* the sweep (SweepBegin, then one step per slot of the dead-report loop  *)
(* and one per entry of the close loop).  Fill threads Stamp a packet     *)
(* (slot, generation) while main is between calls.  Holders: transmit     *)
(* workers resolve a stamped packet inside a pass and send on the         *)
(* descriptor; the receive worker takes a poller event (fd, slot,         *)
(* generation, address), resolves it and reads.  A send or read on a     *)
(* disconnected socket reports it dead.  The environment opens and closes *)
(* other descriptors of the process and disconnects sockets (ICMP).       *)
(*                                                                         *)
(* The code has Quarantine, GenCheck and RefusedOpenFree TRUE and          *)
(* SnapshotFirst FALSE; each broken config flips one and must fail its     *)
(* named invariant (README-RNMsgxSocketTable.md).                          *)
(***************************************************************************)
EXTENDS Integers, Sequences, FiniteSets

CONSTANTS
    Cap,            \* slots: rocketnet_msgx_socket_table_t.capacity
    Addrs,          \* client addresses, ids >= 1 (0 is the zero address)
    NFd,            \* kernel descriptors 0..NFd-1
    TxHolders,      \* transmit worker holder ids
    RxHolder,       \* the receive worker's holder id
    IS,             \* index entries: ROCKETNET_MSGX_TABLE_INDEX_SIZE
    Home(_),        \* an address's index home: hash & (IS - 1)
    CoolEntries,    \* ROCKETNET_MSGX_TABLE_COOLDOWN_ENTRIES
    CoolOf(_),      \* an address's cooldown entry: hash % CoolEntries
    Cooldown,       \* ROCKETNET_MSGX_TABLE_DEAD_COOLDOWN_UPDATES
    Budget,         \* opens_per_update
    GenWrap,        \* the generation counter wraps modulo this (2^32 in the code)
    PassLimit,      \* a holder begins a pass only while its counter is below this
    MaxUpdates,     \* bound: updates
    MaxPackets,     \* bound: stamped packets
    MaxEnv,         \* bound: other descriptors of the process open at once
    Icmp,           \* TRUE: the network may disconnect a socket
    Quarantine,         \* TRUE: a retired socket closes only after the holders' passes
    GenCheck,           \* TRUE: resolve compares the stamp's generation
    SnapshotFirst,      \* FALSE: the retire stores the word before it snapshots the passes
    RefusedOpenFree     \* TRUE: a refused open takes no slot and is counted

Holders == TxHolders \cup {RxHolder}
Slots == 0..(Cap - 1)
Fds == 0..(NFd - 1)
Entries == 0..(IS - 1)

FREE == 0
OPEN == 1
RETIRED == 2

\* the counters, by the code's index
C_OPENED == 0
C_CLOSED == 1
C_OPEN_FAILED == 2
C_OPEN_DEFERRED == 3
C_TABLE_FULL == 4
C_RETIRED_FORGET == 5
C_RETIRED_DEAD == 6
C_COOLDOWN == 7
Counters == 0..7

\* a kernel descriptor: -1 closed, 0 another descriptor of the process, else
\* a socket connected to that address
KClosed == -1
KEnv == 0

VARIABLES
    \* the slots (rocketnet_msgx_socket_slot_t)
    word,           \* <<generation, fd + 1>>; read by holders and fill threads
    deadRep,        \* dead_report: 0 none, else reported generation + 1
    state, fd, gen, addr,
    snap,           \* snapshot[holder]: each holder's pass at the retire
    \* the table
    pass,           \* pass[holder]: even quiescent, odd inside a pass
    deadReports,    \* dead_reports
    freeSlots, numFree,     \* free_slots[], num_free: a stack
    retired, numRetired,    \* retired[], num_retired: oldest first, compacted by the sweep
    numOpen,
    index,          \* index[entry]: slot, or -1
    coolAddr, coolUntil,    \* cooldown_address[], cooldown_until[]
    counters,
    update, opensLeft,
    \* main's place inside a call (not code state: "idle" between calls)
    mpc, ms, mi, mk,
    \* the kernel and the caller's receive poller
    kfd,            \* kfd[f]: KClosed, KEnv or the connected peer
    kdead,          \* kdead[f]: the socket was disconnected
    poller,         \* registered events <<fd, slot, generation, address>>
    \* the holders and the packets
    pkts,           \* stamped packets [a, s, g, h, st, fd]
    cur,            \* cur[tx holder]: the packet resolved and not yet sent, or 0
    rep,            \* rep[holder]: <<slot, generation>> to report dead, or <<>>
    rxEv, rxFd,     \* the receive worker's event and resolved descriptor
    \* history
    crossed,        \* a holder sent or read on a socket connected to another peer
    onClosed,       \* a holder sent or read on a closed descriptor
    attempts        \* open calls made

slotv == <<word, deadRep, state, fd, gen, addr, snap>>
tabv == <<deadReports, freeSlots, numFree, retired, numRetired, numOpen, index,
          coolAddr, coolUntil, counters, update, opensLeft>>
mainv == <<mpc, ms, mi, mk>>
kernv == <<kfd, kdead, poller>>
holdv == <<pass, pkts, cur, rep, rxEv, rxFd>>
ghostv == <<crossed, onClosed, attempts>>
vars == <<slotv, tabv, mainv, kernv, holdv, ghostv>>

-----------------------------------------------------------------------------
(* The index: open addressing, linear probing, backward-shift delete.      *)

RECURSIVE LookupFrom(_, _, _, _, _)
LookupFrom(ix, ad, a, e, n) ==
    IF n = IS \/ ix[e] < 0 THEN -1
    ELSE IF ad[ix[e]] = a THEN ix[e]
    ELSE LookupFrom(ix, ad, a, (e + 1) % IS, n + 1)

\* rocketnet_msgx_socket_table_lookup
Lookup(a) == LookupFrom(index, addr, a, Home(a), 0)

RECURSIVE EmptyFrom(_, _)
EmptyFrom(ix, e) == IF ix[e] < 0 THEN e ELSE EmptyFrom(ix, (e + 1) % IS)

\* rocketnet_msgx_socket_table_index_insert, the slot's address being a
IxInsert(ix, slot, a) == [ix EXCEPT ![EmptyFrom(ix, Home(a))] = slot]

RECURSIVE SlotFrom(_, _, _)
SlotFrom(ix, slot, e) == IF ix[e] = slot THEN e ELSE SlotFrom(ix, slot, (e + 1) % IS)

\* the shift loop: hole is the entry to fill, entry the last one examined
RECURSIVE Shift(_, _, _, _)
Shift(ix, ad, hole, entry) ==
    LET next == (entry + 1) % IS
        moving == ix[next]
    IN IF moving < 0 THEN [ix EXCEPT ![hole] = -1]
       ELSE LET home == Home(ad[moving])
                inRange == IF hole <= next THEN home > hole /\ home <= next
                                           ELSE home > hole \/ home <= next
            IN IF inRange THEN Shift(ix, ad, hole, next)
               ELSE Shift([ix EXCEPT ![hole] = moving], ad, next, next)

\* rocketnet_msgx_socket_table_index_remove
IxRemove(ix, ad, slot) ==
    LET hole == SlotFrom(ix, slot, Home(ad[slot])) IN Shift(ix, ad, hole, hole)

-----------------------------------------------------------------------------
(* Holders read the word.                                                   *)

\* rocketnet_msgx_socket_table_resolve
Resolve(s, g) ==
    IF s < 0 \/ s >= Cap THEN -1
    ELSE IF GenCheck /\ word[s][1] # g THEN -1
    ELSE word[s][2] - 1

\* rocketnet_msgx_socket_table_quiescent
Quiescent(s) == \A h \in Holders : ~(snap[s][h] % 2 = 1 /\ pass[h] = snap[s][h])

Odd(n) == n % 2 = 1

\* the kernel: the lowest free descriptor, refused for a peer that already
\* has a connected socket (EADDRINUSE) or when none is free
FreeFds == {f \in Fds : kfd[f] = KClosed}
KernelOpen(a) ==
    IF \E f \in Fds : kfd[f] = a THEN -1
    ELSE IF FreeFds = {} THEN -1
    ELSE CHOOSE f \in FreeFds : \A g \in FreeFds : f <= g

KClose(k, f) == IF f \in Fds THEN [k EXCEPT ![f] = KClosed] ELSE k
KUndead(k, f) == IF f \in Fds THEN [k EXCEPT ![f] = FALSE] ELSE k

Idle == mpc = "idle"

\* a refused mark (cooldown, table full, open deferred) changes only its
\* counter, which no guard reads; the liveness configuration leaves refused
\* marks out (MCNoRefusedMarks) so that its state space is finite without
\* the VIEW, and every other configuration keeps them
RefusedMarks == TRUE

-----------------------------------------------------------------------------
Init ==
    /\ word = [s \in Slots |-> <<0, 0>>]
    /\ deadRep = [s \in Slots |-> 0]
    /\ state = [s \in Slots |-> FREE]
    /\ fd = [s \in Slots |-> -1]
    /\ gen = [s \in Slots |-> 0]
    /\ addr = [s \in Slots |-> 0]
    /\ snap = [s \in Slots |-> [h \in Holders |-> 0]]
    /\ pass = [h \in Holders |-> 0]
    /\ deadReports = 0
    /\ freeSlots = [i \in Slots |-> Cap - 1 - i]
    /\ numFree = Cap
    /\ retired = [i \in Slots |-> 0]
    /\ numRetired = 0
    /\ numOpen = 0
    /\ index = [e \in Entries |-> -1]
    /\ coolAddr = [e \in 0..(CoolEntries - 1) |-> 0]
    /\ coolUntil = [e \in 0..(CoolEntries - 1) |-> 0]
    /\ counters = [c \in Counters |-> 0]
    /\ update = 0
    /\ opensLeft = Budget
    /\ mpc = "idle" /\ ms = 0 /\ mi = 0 /\ mk = 0
    /\ kfd = [f \in Fds |-> KClosed]
    /\ kdead = [f \in Fds |-> FALSE]
    /\ poller = {}
    /\ pkts = << >>
    /\ cur = [h \in TxHolders |-> 0]
    /\ rep = [h \in Holders |-> << >>]
    /\ rxEv = << >>
    /\ rxFd = -1
    /\ crossed = FALSE /\ onClosed = FALSE /\ attempts = 0

-----------------------------------------------------------------------------
(* Main.                                                                    *)

\* rocketnet_msgx_socket_table_begin_update
BeginUpdate ==
    /\ Idle
    /\ update < MaxUpdates
    /\ update' = update + 1
    /\ opensLeft' = Budget
    /\ UNCHANGED <<slotv, deadReports, freeSlots, numFree, retired, numRetired, numOpen, index,
                   coolAddr, coolUntil, counters, mainv, kernv, holdv, ghostv>>

\* rocketnet_msgx_socket_table_mark, then the caller registers the new
\* socket with its receive poller
Mark(a) ==
    /\ Idle
    /\ IF Lookup(a) >= 0 THEN UNCHANGED vars
       ELSE LET e == CoolOf(a) IN
       IF coolUntil[e] > update /\ coolAddr[e] = a THEN
            /\ RefusedMarks
            /\ counters' = [counters EXCEPT ![C_COOLDOWN] = @ + 1]
            /\ UNCHANGED <<slotv, deadReports, freeSlots, numFree, retired, numRetired, numOpen, index,
                           coolAddr, coolUntil, update, opensLeft, mainv, kernv, holdv, ghostv>>
       ELSE IF numFree = 0 THEN
            /\ RefusedMarks
            /\ counters' = [counters EXCEPT ![C_TABLE_FULL] = @ + 1]
            /\ UNCHANGED <<slotv, deadReports, freeSlots, numFree, retired, numRetired, numOpen, index,
                           coolAddr, coolUntil, update, opensLeft, mainv, kernv, holdv, ghostv>>
       ELSE IF opensLeft <= 0 THEN
            /\ RefusedMarks
            /\ counters' = [counters EXCEPT ![C_OPEN_DEFERRED] = @ + 1]
            /\ UNCHANGED <<slotv, deadReports, freeSlots, numFree, retired, numRetired, numOpen, index,
                           coolAddr, coolUntil, update, opensLeft, mainv, kernv, holdv, ghostv>>
       ELSE LET f == KernelOpen(a)
                s == freeSlots[numFree - 1]
            IN
            /\ opensLeft' = opensLeft - 1
            /\ attempts' = attempts + 1
            /\ IF f < 0 /\ RefusedOpenFree THEN
                    /\ counters' = [counters EXCEPT ![C_OPEN_FAILED] = @ + 1]
                    /\ UNCHANGED <<slotv, deadReports, freeSlots, numFree, retired, numRetired, numOpen,
                                   index, coolAddr, coolUntil, update, mainv, kernv, holdv, crossed, onClosed>>
               ELSE IF f < 0 THEN
                    \* broken: the refused open takes a slot, uncounted
                    /\ numFree' = numFree - 1
                    /\ state' = [state EXCEPT ![s] = OPEN]
                    /\ fd' = [fd EXCEPT ![s] = f]
                    /\ addr' = [addr EXCEPT ![s] = a]
                    /\ word' = [word EXCEPT ![s] = <<gen[s], f + 1>>]
                    /\ index' = IxInsert(index, s, a)
                    /\ numOpen' = numOpen + 1
                    /\ UNCHANGED <<deadRep, gen, snap, deadReports, freeSlots, retired, numRetired,
                                   coolAddr, coolUntil, counters, update, mainv, kernv, holdv, crossed, onClosed>>
               ELSE
                    /\ numFree' = numFree - 1
                    /\ state' = [state EXCEPT ![s] = OPEN]
                    /\ fd' = [fd EXCEPT ![s] = f]
                    /\ addr' = [addr EXCEPT ![s] = a]
                    /\ word' = [word EXCEPT ![s] = <<gen[s], f + 1>>]
                    /\ index' = IxInsert(index, s, a)
                    /\ numOpen' = numOpen + 1
                    /\ counters' = [counters EXCEPT ![C_OPENED] = @ + 1]
                    /\ kfd' = [kfd EXCEPT ![f] = a]
                    /\ kdead' = [kdead EXCEPT ![f] = FALSE]
                    /\ poller' = poller \cup {<<f, s, gen[s], a>>}
                    /\ UNCHANGED <<deadRep, gen, snap, deadReports, freeSlots, retired, numRetired,
                                   coolAddr, coolUntil, update, mainv, holdv, crossed, onClosed>>

\* the first step of a retire: the address leaves the index, the generation
\* advances, and (in the code's order) the word loses its descriptor; or,
\* broken, the passes are snapshotted first
RetireFirst(s, next) ==
    LET ng == (gen[s] + 1) % GenWrap IN
    /\ index' = IxRemove(index, addr, s)
    /\ gen' = [gen EXCEPT ![s] = ng]
    /\ IF ~SnapshotFirst
          THEN /\ word' = [word EXCEPT ![s] = <<ng, 0>>]
               /\ mpc' = next[1]
               /\ UNCHANGED snap
          ELSE /\ snap' = [snap EXCEPT ![s] = pass]
               /\ mpc' = next[2]
               /\ UNCHANGED word
    /\ ms' = s

\* the end of a retire: the slot joins the quarantine (or, broken, closes now)
Finish(s, c) ==
    /\ numOpen' = numOpen - 1
    /\ IF Quarantine THEN
            /\ state' = [state EXCEPT ![s] = RETIRED]
            /\ retired' = [retired EXCEPT ![numRetired] = s]
            /\ numRetired' = numRetired + 1
            /\ counters' = [counters EXCEPT ![c] = @ + 1]
            /\ UNCHANGED <<fd, freeSlots, numFree, kfd, kdead>>
       ELSE
            /\ state' = [state EXCEPT ![s] = FREE]
            /\ fd' = [fd EXCEPT ![s] = -1]
            /\ freeSlots' = [freeSlots EXCEPT ![numFree] = s]
            /\ numFree' = numFree + 1
            /\ kfd' = KClose(kfd, fd[s])
            /\ kdead' = KUndead(kdead, fd[s])
            /\ counters' = [counters EXCEPT ![c] = @ + 1, ![C_CLOSED] = @ + 1]
            /\ UNCHANGED <<retired, numRetired>>

\* where the dead-report loop goes after slot s
NextDead(s) ==
    IF s + 1 < Cap THEN mpc' = "sdead" /\ ms' = s + 1 /\ UNCHANGED <<mi, mk>>
    ELSE IF numRetired' > 0 THEN mpc' = "sclose" /\ ms' = 0 /\ mi' = 0 /\ mk' = 0
    ELSE mpc' = "idle" /\ ms' = 0 /\ mi' = 0 /\ mk' = 0


\* rocketnet_msgx_socket_table_forget: the lookup and the retire's first step
Forget(a) ==
    /\ Idle
    /\ IF Lookup(a) < 0 THEN UNCHANGED vars
       ELSE /\ RetireFirst(Lookup(a), <<"fsnap", "fword">>)
            /\ UNCHANGED <<deadRep, state, fd, addr, deadReports, freeSlots, numFree, retired, numRetired,
                           numOpen, coolAddr, coolUntil, counters, update, opensLeft, mi, mk,
                           kernv, holdv, ghostv>>

\* RNMsgxSocketTable Retire: the retire's second step, the passes
\* snapshotted after the word store, then the slot in quarantine; on the
\* forget path the caller deregisters next, on the sweep's path its
\* dead-report loop goes on
Retire ==
    /\ mpc \in {"fsnap", "ssnap"}
    /\ snap' = [snap EXCEPT ![ms] = pass]
    /\ Finish(ms, IF mpc = "fsnap" THEN C_RETIRED_FORGET ELSE C_RETIRED_DEAD)
    /\ UNCHANGED <<word, deadRep, gen, addr, deadReports, index, coolAddr, coolUntil, update, opensLeft,
                   poller, holdv, ghostv>>
    /\ IF mpc = "fsnap" THEN mpc' = "fdereg" /\ UNCHANGED <<ms, mi, mk>> ELSE NextDead(ms)

\* broken (SnapshotFirst): the word store after the snapshot
RetireWordLate ==
    /\ SnapshotFirst
    /\ mpc \in {"fword", "sword"}
    /\ word' = [word EXCEPT ![ms] = <<gen[ms], 0>>]
    /\ Finish(ms, IF mpc = "fword" THEN C_RETIRED_FORGET ELSE C_RETIRED_DEAD)
    /\ UNCHANGED <<snap, deadRep, gen, addr, deadReports, index, coolAddr, coolUntil, update, opensLeft,
                   poller, holdv, ghostv>>
    /\ IF mpc = "fword" THEN mpc' = "fdereg" /\ UNCHANGED <<ms, mi, mk>> ELSE NextDead(ms)

\* the caller deregisters the retired descriptor (forget's retired_fd) from
\* its receive poller
ForgetDereg ==
    /\ mpc = "fdereg"
    /\ poller' = {e \in poller : e[2] # ms}
    /\ mpc' = "idle"
    /\ ms' = 0
    /\ UNCHANGED <<slotv, tabv, mi, mk, kfd, kdead, holdv, ghostv>>

\* rocketnet_msgx_socket_table_sweep: the dead-report count is taken; with
\* none, the close loop; with nothing retired either, nothing changes
SweepBegin ==
    /\ Idle
    /\ IF deadReports > 0 THEN
            /\ deadReports' = 0
            /\ mpc' = "sdead"
            /\ ms' = 0
            /\ UNCHANGED <<slotv, freeSlots, numFree, retired, numRetired, numOpen, index, coolAddr,
                           coolUntil, counters, update, opensLeft, mi, mk, kernv, holdv, ghostv>>
       ELSE IF numRetired > 0 THEN
            /\ mpc' = "sclose"
            /\ UNCHANGED <<slotv, tabv, ms, mi, mk, kernv, holdv, ghostv>>
       ELSE UNCHANGED vars

\* the dead-report loop at slot ms: the report is taken; one about the
\* slot's open generation cools the address down, the caller's retire hook
\* deregisters the descriptor, and the retire's first step runs
RetireDead ==
    /\ mpc = "sdead"
    /\ LET s == ms
           r == deadRep[ms]
       IN
       /\ deadRep' = [deadRep EXCEPT ![s] = 0]
       /\ IF r = 0 \/ state[s] # OPEN \/ r - 1 # gen[s] THEN
               /\ UNCHANGED <<word, state, fd, gen, addr, snap, tabv, kernv, holdv, ghostv>>
               /\ NextDead(s)
          ELSE
               /\ coolAddr' = [coolAddr EXCEPT ![CoolOf(addr[s])] = addr[s]]
               /\ coolUntil' = [coolUntil EXCEPT ![CoolOf(addr[s])] = update + Cooldown]
               /\ poller' = {e \in poller : e[2] # s}
               /\ RetireFirst(s, <<"ssnap", "sword">>)
               /\ UNCHANGED <<state, fd, addr, deadReports, freeSlots, numFree, retired, numRetired,
                              numOpen, counters, update, opensLeft, mi, mk, kfd, kdead, holdv, ghostv>>

\* the close loop at retired[mi]: closed and freed once quiescent, else kept
\* at retired[mk]; after the last entry num_retired is the kept count
Close ==
    /\ mpc = "sclose"
    /\ mi < numRetired
    /\ LET s == retired[mi]
           q == Quiescent(s)
           nk == IF q THEN mk ELSE mk + 1
       IN
       /\ IF q THEN
               /\ kfd' = KClose(kfd, fd[s])
               /\ kdead' = KUndead(kdead, fd[s])
               /\ fd' = [fd EXCEPT ![s] = -1]
               /\ state' = [state EXCEPT ![s] = FREE]
               /\ freeSlots' = [freeSlots EXCEPT ![numFree] = s]
               /\ numFree' = numFree + 1
               /\ counters' = [counters EXCEPT ![C_CLOSED] = @ + 1]
               /\ UNCHANGED retired
          ELSE
               /\ retired' = [retired EXCEPT ![mk] = s]
               /\ UNCHANGED <<kfd, kdead, fd, state, freeSlots, numFree, counters>>
       /\ IF mi + 1 = numRetired
             THEN mpc' = "idle" /\ numRetired' = nk /\ mi' = 0 /\ mk' = 0
             ELSE mpc' = "sclose" /\ mi' = mi + 1 /\ mk' = nk /\ UNCHANGED numRetired
    /\ UNCHANGED <<word, deadRep, gen, addr, snap, deadReports, numOpen, index, coolAddr, coolUntil,
                   update, opensLeft, ms, poller, holdv, ghostv>>

-----------------------------------------------------------------------------
(* Fill threads, between main's calls (the fill phase).                     *)

\* rocketnet_msgx_socket_table_stamp: the slot and the generation in its word
Stamp(a, h) ==
    /\ Idle
    /\ Len(pkts) < MaxPackets
    /\ LET s == Lookup(a) IN
       pkts' = Append(pkts, [a |-> a, s |-> s, g |-> IF s < 0 THEN 0 ELSE word[s][1],
                             h |-> h, st |-> "queued", fd |-> -1])
    /\ UNCHANGED <<slotv, tabv, mainv, kernv, pass, cur, rep, rxEv, rxFd, ghostv>>

-----------------------------------------------------------------------------
(* Holders.                                                                 *)

\* rocketnet_msgx_socket_table_holder_begin
HolderBegin(h) ==
    /\ ~Odd(pass[h])
    /\ pass[h] < PassLimit
    /\ pass' = [pass EXCEPT ![h] = @ + 1]
    /\ UNCHANGED <<slotv, tabv, mainv, kernv, pkts, cur, rep, rxEv, rxFd, ghostv>>

\* rocketnet_msgx_socket_table_holder_end: the pass's sends, reads and
\* reports are done
HolderEnd(h) ==
    /\ Odd(pass[h])
    /\ rep[h] = << >>
    /\ h \in TxHolders => cur[h] = 0
    /\ h = RxHolder => rxEv = << >>
    /\ pass' = [pass EXCEPT ![h] = @ + 1]
    /\ UNCHANGED <<slotv, tabv, mainv, kernv, pkts, cur, rep, rxEv, rxFd, ghostv>>

\* rocketnet_msgx_socket_table_report_dead: inside the pass, the slot and
\* generation the holder resolved
ReportDead(h) ==
    /\ Odd(pass[h])
    /\ rep[h] # << >>
    /\ LET s == rep[h][1]
           g == rep[h][2]
       IN IF s >= 0 /\ s < Cap /\ deadRep[s] = 0
             THEN /\ deadRep' = [deadRep EXCEPT ![s] = g + 1]
                  /\ deadReports' = deadReports + 1
             ELSE UNCHANGED <<deadRep, deadReports>>
    /\ rep' = [rep EXCEPT ![h] = << >>]
    /\ UNCHANGED <<word, state, fd, gen, addr, snap, freeSlots, numFree, retired, numRetired, numOpen,
                   index, coolAddr, coolUntil, counters, update, opensLeft, mainv, kernv,
                   pass, pkts, cur, rxEv, rxFd, ghostv>>

\* a transmit worker resolves one of its stamped packets: the descriptor,
\* or the shared socket
TxResolve(h, i) ==
    /\ Odd(pass[h])
    /\ cur[h] = 0
    /\ rep[h] = << >>
    /\ pkts[i].h = h
    /\ pkts[i].st = "queued"
    /\ LET f == Resolve(pkts[i].s, pkts[i].g) IN
       IF f < 0 THEN /\ pkts' = [pkts EXCEPT ![i].st = "shared"]
                     /\ UNCHANGED cur
                ELSE /\ pkts' = [pkts EXCEPT ![i].st = "resolved", ![i].fd = f]
                     /\ cur' = [cur EXCEPT ![h] = i]
    /\ UNCHANGED <<slotv, tabv, mainv, kernv, pass, rep, rxEv, rxFd, ghostv>>

\* the send, in the same pass: the kernel sends to the descriptor's peer; a
\* disconnected socket answers ENOTCONN and the holder will report it
TxSend(h) ==
    /\ cur[h] # 0
    /\ LET i == cur[h]
           p == pkts[i]
           k == kfd[p.fd]
           ok == k = p.a
       IN
       /\ crossed' = (crossed \/ (k # KClosed /\ ~ok))
       /\ onClosed' = (onClosed \/ k = KClosed)
       /\ IF ok /\ kdead[p.fd]
             THEN /\ pkts' = [pkts EXCEPT ![i].st = "counted"]
                  /\ rep' = [rep EXCEPT ![h] = <<p.s, p.g>>]
             ELSE /\ pkts' = [pkts EXCEPT ![i].st = IF k = KClosed THEN "counted" ELSE "conn"]
                  /\ UNCHANGED rep
       /\ cur' = [cur EXCEPT ![h] = 0]
    /\ UNCHANGED <<slotv, tabv, mainv, kernv, pass, rxEv, rxFd, attempts>>

\* the receive worker takes a poller event inside its pass
RxPoll(e) ==
    /\ Odd(pass[RxHolder])
    /\ rxEv = << >>
    /\ rep[RxHolder] = << >>
    /\ e \in poller
    /\ rxEv' = e
    /\ UNCHANGED <<slotv, tabv, mainv, kernv, pass, pkts, cur, rep, rxFd, ghostv>>

\* and resolves its slot and generation: a retired socket's event is dropped
RxResolve ==
    /\ rxEv # << >>
    /\ rxFd = -1
    /\ LET f == Resolve(rxEv[2], rxEv[3]) IN
       IF f < 0 THEN rxEv' = << >> /\ UNCHANGED rxFd
                ELSE rxFd' = f /\ UNCHANGED rxEv
    /\ UNCHANGED <<slotv, tabv, mainv, kernv, pass, pkts, cur, rep, ghostv>>

\* the read on the resolved descriptor
RxRead ==
    /\ rxFd >= 0
    /\ LET k == kfd[rxFd]
           ok == k = rxEv[4]
       IN
       /\ crossed' = (crossed \/ (k # KClosed /\ ~ok))
       /\ onClosed' = (onClosed \/ k = KClosed)
       /\ IF ok /\ kdead[rxFd]
             THEN rep' = [rep EXCEPT ![RxHolder] = <<rxEv[2], rxEv[3]>>]
             ELSE UNCHANGED rep
    /\ rxEv' = << >>
    /\ rxFd' = -1
    /\ UNCHANGED <<slotv, tabv, mainv, kernv, pass, pkts, cur, attempts>>

-----------------------------------------------------------------------------
(* The environment: other descriptors of the process, and ICMP.             *)

EnvOpen ==
    /\ Cardinality({f \in Fds : kfd[f] = KEnv}) < MaxEnv
    /\ FreeFds # {}
    /\ kfd' = [kfd EXCEPT ![CHOOSE f \in FreeFds : \A g \in FreeFds : f <= g] = KEnv]
    /\ UNCHANGED <<slotv, tabv, mainv, kdead, poller, holdv, ghostv>>

EnvClose(f) ==
    /\ kfd[f] = KEnv
    /\ kfd' = [kfd EXCEPT ![f] = KClosed]
    /\ UNCHANGED <<slotv, tabv, mainv, kdead, poller, holdv, ghostv>>

\* an ICMP error disconnects a connected socket (ENOTCONN on its next use)
Disconnect(f) ==
    /\ Icmp
    /\ kfd[f] \in Addrs
    /\ ~kdead[f]
    /\ kdead' = [kdead EXCEPT ![f] = TRUE]
    /\ UNCHANGED <<slotv, tabv, mainv, kfd, poller, holdv, ghostv>>

-----------------------------------------------------------------------------
\* main's next step inside a call
MainCont == Retire \/ RetireWordLate \/ ForgetDereg \/ RetireDead \/ Close

Next ==
    \/ \E a \in Addrs : Mark(a) \/ Forget(a)
    \/ BeginUpdate
    \/ SweepBegin
    \/ MainCont
    \/ \E a \in Addrs, h \in TxHolders : Stamp(a, h)
    \/ \E h \in Holders : HolderBegin(h) \/ HolderEnd(h) \/ ReportDead(h)
    \/ \E h \in TxHolders : TxSend(h) \/ \E i \in 1..Len(pkts) : TxResolve(h, i)
    \/ \E e \in poller : RxPoll(e)
    \/ RxResolve
    \/ RxRead
    \/ EnvOpen
    \/ \E f \in Fds : EnvClose(f) \/ Disconnect(f)

Spec == Init /\ [][Next]_vars

\* main keeps sweeping, and every holder finishes and ends its pass (strong
\* fairness: the receive worker may keep taking events)
SpecFair ==
    /\ Spec
    /\ WF_vars(SweepBegin \/ MainCont)
    /\ \A h \in Holders : SF_vars(HolderEnd(h)) /\ WF_vars(ReportDead(h))
    /\ \A h \in TxHolders : WF_vars(TxSend(h))
    /\ WF_vars(RxResolve)
    /\ WF_vars(RxRead)

-----------------------------------------------------------------------------
Statuses == {"queued", "resolved", "conn", "shared", "counted"}

TypeOK ==
    /\ word \in [Slots -> (0..(GenWrap - 1)) \X (0..NFd)]
    /\ deadRep \in [Slots -> 0..GenWrap]
    /\ state \in [Slots -> {FREE, OPEN, RETIRED}]
    /\ fd \in [Slots -> -1..(NFd - 1)]
    /\ gen \in [Slots -> 0..(GenWrap - 1)]
    /\ addr \in [Slots -> {0} \cup Addrs]
    /\ snap \in [Slots -> [Holders -> 0..PassLimit]]
    /\ pass \in [Holders -> 0..PassLimit]
    /\ freeSlots \in [Slots -> Slots] /\ numFree \in 0..Cap
    /\ retired \in [Slots -> Slots] /\ numRetired \in 0..Cap
    /\ numOpen \in 0..Cap
    /\ index \in [Entries -> -1..(Cap - 1)]
    /\ coolAddr \in [0..(CoolEntries - 1) -> {0} \cup Addrs]
    /\ counters \in [Counters -> Nat]
    /\ mpc \in {"idle", "fsnap", "fword", "fdereg", "sdead", "ssnap", "sword", "sclose"}
    /\ kfd \in [Fds -> {KClosed, KEnv} \cup Addrs]
    /\ kdead \in [Fds -> BOOLEAN]
    /\ \A i \in 1..Len(pkts) :
          pkts[i] \in [a : Addrs, s : -1..(Cap - 1), g : 0..(GenWrap - 1), h : TxHolders,
                       st : Statuses, fd : -1..(NFd - 1)]
    /\ cur \in [TxHolders -> 0..MaxPackets]
    /\ rxFd \in -1..(NFd - 1)

\* every send and read goes to the client the packet or event names
NoCrossClient == ~crossed

\* no holder sends or reads on a closed descriptor
NoSendOnClosed == ~onClosed

\* every open call is opened or counted refused; every packet is on its way
\* or ended sent connected, sent shared or counted; between main's calls,
\* every opened socket is open, in quarantine or counted closed, and every
\* retire is in quarantine or counted closed
Accounting ==
    /\ attempts = counters[C_OPENED] + counters[C_OPEN_FAILED]
    /\ \A i \in 1..Len(pkts) : (pkts[i].st = "resolved") <=> (cur[pkts[i].h] = i)
    /\ Idle => counters[C_OPENED] = counters[C_CLOSED] + numOpen + numRetired
    /\ Idle => counters[C_RETIRED_FORGET] + counters[C_RETIRED_DEAD] = counters[C_CLOSED] + numRetired

\* between main's calls every slot is in exactly one of the free stack, open,
\* or the retired list, every slot that is not free holds the open socket
\* connected to its address, and every socket the table opened is in exactly
\* one slot
NoLeak ==
    Idle =>
    /\ numFree + numOpen + numRetired = Cap
    /\ {freeSlots[i] : i \in 0..(numFree - 1)} = {s \in Slots : state[s] = FREE}
    /\ {retired[i] : i \in 0..(numRetired - 1)} = {s \in Slots : state[s] = RETIRED}
    /\ numOpen = Cardinality({s \in Slots : state[s] = OPEN})
    /\ \A s \in Slots : state[s] # FREE => fd[s] \in Fds /\ kfd[fd[s]] = addr[s]
    /\ \A s \in Slots : state[s] = FREE => fd[s] = -1
    /\ \A f \in Fds : kfd[f] \in Addrs => Cardinality({s \in Slots : state[s] # FREE /\ fd[s] = f}) = 1

\* between main's calls the index finds exactly the open slots, one per
\* address, and holds nothing else
IndexMatchesOpen ==
    Idle =>
    /\ \A a \in Addrs :
          LET s == Lookup(a) IN
          /\ s >= 0 => state[s] = OPEN /\ addr[s] = a
          /\ \A t \in Slots : state[t] = OPEN /\ addr[t] = a => s = t
    /\ {index[e] : e \in {x \in Entries : index[x] >= 0}} = {s \in Slots : state[s] = OPEN}
    /\ Cardinality({x \in Entries : index[x] >= 0}) = numOpen

\* between main's calls the word of an open slot is its generation and
\* descriptor, and of any other slot its generation and no descriptor
WordMatchesSlot ==
    Idle =>
    \A s \in Slots :
        word[s] = <<gen[s], IF state[s] = OPEN THEN fd[s] + 1 ELSE 0>>

\* a socket in quarantine is closed and its slot freed
RetiredClosed == \A s \in Slots : state[s] = RETIRED ~> state[s] = FREE

-----------------------------------------------------------------------------
(* The checked configurations use this VIEW: the state with every value    *)
(* that no later step reads replaced by a constant.  A FREE slot's address *)
(* and an OPEN slot's snapshot are written before they are read again; the *)
(* free stack above numFree and the retired list above numRetired are      *)
(* written before they are read; a cooldown entry whose update has passed  *)
(* never matches again (update only grows); a finished packet keeps only   *)
(* its status and holder.  So two states equal under the view have the     *)
(* same futures and the same invariant values, and the view only merges    *)
(* states TLC would otherwise explore twice.  Traces print full states.    *)

\* the counters and attempts only as the differences Accounting reads: every
\* action adds the same to a counter whatever its value, so states that
\* differ only in history agree on every future difference
CounterView ==
    <<attempts - counters[C_OPENED] - counters[C_OPEN_FAILED],
      counters[C_OPENED] - counters[C_CLOSED],
      counters[C_RETIRED_FORGET] + counters[C_RETIRED_DEAD] - counters[C_CLOSED]>>

LiveSnap(s) == state[s] = RETIRED \/ (mpc \in {"fword", "sword"} /\ ms = s)

\* a live snapshot only as what Quiescent reads of it: whether the holder is
\* still inside the pass snapshotted.  A pass only grows, so once it moves on
\* from the snapshot it never equals it again
Blocks(s) == [h \in Holders |-> LiveSnap(s) /\ Odd(snap[s][h]) /\ pass[h] = snap[s][h]]

SafetyView ==
    <<word, deadRep, state, fd, gen,
      [s \in Slots |-> IF state[s] = FREE THEN 0 ELSE addr[s]],
      [s \in Slots |-> Blocks(s)],
      pass, deadReports,
      [i \in Slots |-> IF i < numFree THEN freeSlots[i] ELSE -1], numFree,
      [i \in Slots |-> IF i < numRetired THEN retired[i] ELSE -1], numRetired,
      numOpen, index,
      [e \in 0..(CoolEntries - 1) |-> IF coolUntil[e] > update THEN <<coolAddr[e], coolUntil[e] - update>> ELSE 0],
      CounterView, update, opensLeft, mainv, kernv,
      [i \in 1..Len(pkts) |-> IF pkts[i].st \in {"queued", "resolved"} THEN pkts[i]
                                                                       ELSE [st |-> pkts[i].st, h |-> pkts[i].h]],
      cur, rep, rxEv, rxFd, crossed, onClosed>>

=============================================================================
