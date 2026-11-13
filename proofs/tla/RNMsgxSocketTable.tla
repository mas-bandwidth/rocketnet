--------------------------- MODULE RNMsgxSocketTable ---------------------------
(***************************************************************************)
(* The macOS batched server transport's per-client socket table           *)
(* (sdk/rocketnet_msgx_socket_table.h) at the code's layout: the slot     *)
(* word (generation, descriptor + 1), the holders' pass counters (even    *)
(* quiescent, odd inside a pass), the passcopy taken at a retire, the     *)
(* retired list compacted in place by the sweep, the free stack, the      *)
(* open-addressing address index with its backward-shift delete, the      *)
(* direct-mapped cooldown table, the update's open budget and the eight   *)
(* counters.  The kernel is a pool of NFd descriptors handed out lowest   *)
(* free first (POSIX), each mapped to the remote its socket is connected    *)
(* to, refusing a second socket connected to the same remote (EADDRINUSE,   *)
(* measured).  That reuse is what makes a close too early a cross-client  *)
(* send: the descriptor a holder resolved is reissued to another client.  *)
(*                                                                         *)
(* Threads.  Main runs Mark, Forget (its retire in two steps, word then   *)
(* passcopy, then the caller's poller deregistration), BeginUpdate and    *)
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
(* PassCopyFirst FALSE; each broken config flips one and must fail its     *)
(* named invariant (README-RNMsgxSocketTable.md).  ClearReportAtFree and   *)
(* RegisterMayFail are TRUE in the code; a cfg overrides them.             *)
(***************************************************************************)
EXTENDS Integers, Sequences, FiniteSets

CONSTANTS
    Cap,            \* slots: rocketnet_msgx_socket_table_t.capacity
    Addrs,          \* client addresses, ids >= 1 (0 is the zero address)
    NFd,            \* kernel descriptors 0..NFd-1
    TxHolders,      \* transmit worker holder ids
    RxHolder,       \* the receive worker's holder id
    IS,             \* index entries: ROCKETNET_MSGX_TABLE_INDEX_SIZE
    HomeAt,         \* the index homes: a * IS + h for address a homed on entry h = hash & (IS - 1)
    CoolEntries,    \* ROCKETNET_MSGX_TABLE_COOLDOWN_ENTRIES
    CoolAt,         \* the cooldown entries: a * CoolEntries + e for address a on entry e = hash % CoolEntries
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
    PassCopyFirst,      \* FALSE: the retire stores the word before it passcopys the passes
    RefusedOpenFree     \* TRUE: a refused open takes no slot and is counted

\* every address has exactly one home and one cooldown entry (a cfg carries a
\* set of integers, not a function)
ASSUME /\ \A a \in Addrs : Cardinality({x \in HomeAt : x \div IS = a}) = 1
       /\ \A a \in Addrs : Cardinality({x \in CoolAt : x \div CoolEntries = a}) = 1

Holders == TxHolders \cup {RxHolder}
Slots == 0..(Cap - 1)
Fds == 0..(NFd - 1)
Entries == 0..(IS - 1)

\* an address's index home and cooldown entry
Home(a) == (CHOOSE x \in HomeAt : x \div IS = a) % IS
CoolOf(a) == (CHOOSE x \in CoolAt : x \div CoolEntries = a) % CoolEntries

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
    snap,           \* passcopy[holder]: each holder's pass at the retire
    \* the table
    pass,           \* pass[holder]: even quiescent, odd inside a pass
    deadReports,    \* dead_reports
    freeSlots, numFree,     \* free_slots[], num_free: a stack
    retired, numRetired,    \* retired[], num_retired: oldest first, compacted by the sweep
    numOpen,
    index,          \* index[entry]: the slot, for each entry in use (an empty entry, -1 in the code, is outside the domain)
    coolAddr, coolUntil,    \* cooldown_address[], cooldown_until[], for the entries in use (not zero) only
    counters,
    update, opensLeft,
    \* main's place inside a call (not code state: "idle" between calls)
    mpc, ms, mi, mk,
    \* the kernel and the caller's receive poller
    kfd,            \* kfd[f]: KClosed, KEnv or the connected remote
    kdead,          \* kdead[f]: the socket was disconnected
    poller,         \* registered events <<fd, slot, generation, address>>
    \* the holders and the packets
    pkts,           \* stamped packets [a, s, g, h, st, fd]
    cur,            \* cur[tx holder]: the packet resolved and not yet sent, or 0
    rep,            \* rep[holder]: <<slot, generation>> to report dead, or <<>>
    rxEv, rxFd,     \* the receive worker's event and resolved descriptor
    \* history
    crossed,        \* a holder sent or read on a socket connected to another remote
    onClosed,       \* a holder sent or read on a closed descriptor
    attempts        \* open calls made

slotv == <<word, deadRep, state, fd, gen, addr, snap>>
tabv == <<deadReports, freeSlots, numFree, retired, numRetired, numOpen, index,
          coolAddr, coolUntil, counters, update, opensLeft>>
mainv == <<mpc, ms, mi, mk>>
kernv == <<kfd, kdead, poller>>
holdv == <<pass, pkts, cur, rep, rxEv, rxFd>>
ghostv == <<crossed, onClosed, attempts>>
\* every variable, flat (the trace check reads it); the groups above are for UNCHANGED
vars == <<word, deadRep, state, fd, gen, addr, snap,
          deadReports, freeSlots, numFree, retired, numRetired, numOpen, index, coolAddr, coolUntil,
          counters, update, opensLeft, mpc, ms, mi, mk, kfd, kdead, poller,
          pass, pkts, cur, rep, rxEv, rxFd, crossed, onClosed, attempts>>

-----------------------------------------------------------------------------
(* The index: open addressing, linear probing, backward-shift delete.      *)

\* The code's index holds -1 in an empty entry; here an entry in use is in the
\* domain and an empty one is not, so a record of the table names the entries
\* in use and no more.  Every step below is the code's arithmetic on the entry
\* numbers.

RECURSIVE LookupFrom(_, _, _, _, _)
LookupFrom(ix, ad, a, e, n) ==
    IF n = IS \/ e \notin DOMAIN ix THEN -1
    ELSE IF ad[ix[e]] = a THEN ix[e]
    ELSE LookupFrom(ix, ad, a, (e + 1) % IS, n + 1)

\* rocketnet_msgx_socket_table_lookup
Lookup(a) == LookupFrom(index, addr, a, Home(a), 0)

\* a function with the value at e set, added to the domain if need be
FnSet(f, e, v) == [x \in DOMAIN f \cup {e} |-> IF x = e THEN v ELSE f[x]]

IxDel(ix, e) == [x \in DOMAIN ix \ {e} |-> ix[x]]

RECURSIVE EmptyFrom(_, _)
EmptyFrom(ix, e) == IF e \notin DOMAIN ix THEN e ELSE EmptyFrom(ix, (e + 1) % IS)

\* rocketnet_msgx_socket_table_index_insert, the slot's address being a
IxInsert(ix, slot, a) == FnSet(ix, EmptyFrom(ix, Home(a)), slot)

RECURSIVE SlotFrom(_, _, _)
SlotFrom(ix, slot, e) == IF ix[e] = slot THEN e ELSE SlotFrom(ix, slot, (e + 1) % IS)

\* the shift loop: hole is the entry to fill, entry the last one examined
RECURSIVE Shift(_, _, _, _)
Shift(ix, ad, hole, entry) ==
    LET next == (entry + 1) % IS IN
    IF next \notin DOMAIN ix THEN IxDel(ix, hole)
    ELSE LET moving == ix[next]
             home == Home(ad[moving])
             inRange == IF hole <= next THEN home > hole /\ home <= next
                                        ELSE home > hole \/ home <= next
         IN IF inRange THEN Shift(ix, ad, hole, next)
            ELSE Shift(FnSet(ix, hole, moving), ad, next, next)

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

\* rocketnet_msgx_socket_table_quiescent, over a record of main's state
Quiescent(R, s) == \A h \in Holders : ~(R.snap[s][h] % 2 = 1 /\ pass[h] = R.snap[s][h])

Odd(n) == n % 2 = 1

\* the kernel: the lowest free descriptor, refused for a remote that already
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

\* the receive poller may refuse to register a new socket (kevent fails), and
\* main then reports it dead (rocketnet_server_msgx_mark)
RegisterMayFail == TRUE

\* the sweep's close loop clears a dead report left in the slot it frees (a
\* holder's report about the retired generation that landed after the dead
\* scan); the broken configuration MCRNMsgxSocketTableBrokenStaleReport.cfg
\* keeps it, and the slot's next socket's report is then lost
ClearReportAtFree == TRUE

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
    /\ index = [e \in {} |-> 0]
    /\ coolAddr = [e \in {} |-> 0]
    /\ coolUntil = [e \in {} |-> 0]
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
       IF e \in DOMAIN coolUntil /\ coolUntil[e] > update /\ coolAddr[e] = a THEN
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
                    \* the caller registers the socket with its receive poller;
                    \* when the poller refuses it, main reports it dead at its
                    \* generation (report_dead: taken only by an empty slot)
                    /\ \E registered \in IF RegisterMayFail THEN BOOLEAN ELSE {TRUE} :
                          IF registered
                             THEN /\ poller' = poller \cup {<<f, s, gen[s], a>>}
                                  /\ UNCHANGED <<deadRep, deadReports>>
                             ELSE /\ UNCHANGED poller
                                  /\ IF deadRep[s] = 0
                                        THEN /\ deadRep' = [deadRep EXCEPT ![s] = gen[s] + 1]
                                             /\ deadReports' = deadReports + 1
                                        ELSE UNCHANGED <<deadRep, deadReports>>
                    /\ UNCHANGED <<gen, snap, freeSlots, retired, numRetired,
                                   coolAddr, coolUntil, update, mainv, holdv, crossed, onClosed>>

\* Main's steps inside a call, as functions of a record of the state main's
\* steps read and write (the holders' passes and the update number, which no
\* main step of a call changes, are read from the state).  Each fine-grained
\* action below applies one of these once; ForgetCall and SweepCall apply
\* them in the code's order with no holder step between, which is what a
\* trace sees: the walk looks at the table between calls.

MainV == [word |-> word, deadRep |-> deadRep, state |-> state, fd |-> fd, gen |-> gen, addr |-> addr,
          snap |-> snap, deadReports |-> deadReports, freeSlots |-> freeSlots, numFree |-> numFree,
          retired |-> retired, numRetired |-> numRetired, numOpen |-> numOpen, index |-> index,
          coolAddr |-> coolAddr, coolUntil |-> coolUntil, counters |-> counters, kfd |-> kfd,
          kdead |-> kdead, poller |-> poller, mpc |-> mpc, ms |-> ms, mi |-> mi, mk |-> mk]

\* the primed state is the record N, every variable of MainV.  TLC passes an
\* operator's argument as an expression it evaluates at each use, so N is
\* bound to its value once, by the singleton quantifier (M), or each of the 24
\* fields would evaluate the whole step again
Apply(N) ==
    \E M \in {N} :
    /\ word' = M.word /\ deadRep' = M.deadRep /\ state' = M.state /\ fd' = M.fd /\ gen' = M.gen
    /\ addr' = M.addr /\ snap' = M.snap /\ deadReports' = M.deadReports /\ freeSlots' = M.freeSlots
    /\ numFree' = M.numFree /\ retired' = M.retired /\ numRetired' = M.numRetired /\ numOpen' = M.numOpen
    /\ index' = M.index /\ coolAddr' = M.coolAddr /\ coolUntil' = M.coolUntil /\ counters' = M.counters
    /\ kfd' = M.kfd /\ kdead' = M.kdead /\ poller' = M.poller
    /\ mpc' = M.mpc /\ ms' = M.ms /\ mi' = M.mi /\ mk' = M.mk

\* the first step of a retire: the address leaves the index, the generation
\* advances, and (in the code's order) the word loses its descriptor; or,
\* broken, the passes are copied first
RetireFirstF(R, s, next) ==
    LET ng == (R.gen[s] + 1) % GenWrap
        r1 == [R EXCEPT !.index = IxRemove(R.index, R.addr, s),
                        !.gen = [R.gen EXCEPT ![s] = ng],
                        !.ms = s]
    IN IF ~PassCopyFirst
          THEN [r1 EXCEPT !.word = [R.word EXCEPT ![s] = <<ng, 0>>], !.mpc = next[1]]
          ELSE [r1 EXCEPT !.snap = [R.snap EXCEPT ![s] = pass], !.mpc = next[2]]

\* the end of a retire: the slot joins the quarantine (or, broken, closes now)
FinishF(R, s, c) ==
    LET r1 == [R EXCEPT !.numOpen = R.numOpen - 1] IN
    IF Quarantine
       THEN [r1 EXCEPT !.state = [R.state EXCEPT ![s] = RETIRED],
                       !.retired = [R.retired EXCEPT ![R.numRetired] = s],
                       !.numRetired = R.numRetired + 1,
                       !.counters = [R.counters EXCEPT ![c] = @ + 1]]
       ELSE [r1 EXCEPT !.state = [R.state EXCEPT ![s] = FREE],
                       !.fd = [R.fd EXCEPT ![s] = -1],
                       !.freeSlots = [R.freeSlots EXCEPT ![R.numFree] = s],
                       !.numFree = R.numFree + 1,
                       !.kfd = KClose(R.kfd, R.fd[s]),
                       !.kdead = KUndead(R.kdead, R.fd[s]),
                       !.counters = [R.counters EXCEPT ![c] = @ + 1, ![C_CLOSED] = @ + 1]]

\* where the dead-report loop goes after slot s
NextDeadF(R, s) ==
    IF s + 1 < Cap THEN [R EXCEPT !.mpc = "sdead", !.ms = s + 1]
    ELSE IF R.numRetired > 0 THEN [R EXCEPT !.mpc = "sclose", !.ms = 0, !.mi = 0, !.mk = 0]
    ELSE [R EXCEPT !.mpc = "idle", !.ms = 0, !.mi = 0, !.mk = 0]

\* rocketnet_msgx_socket_table_forget: the lookup and the retire's first step
ForgetF(R, a) ==
    LET s == LookupFrom(R.index, R.addr, a, Home(a), 0) IN
    IF s < 0 THEN R ELSE RetireFirstF(R, s, <<"fsnap", "fword">>)

\* the retire's second step: the passes copied after the word store,
\* then the slot in quarantine; on the forget path the caller deregisters
\* next, on the sweep's path its dead-report loop goes on
RetireF(R) ==
    LET s == R.ms
        f == FinishF([R EXCEPT !.snap = [R.snap EXCEPT ![s] = pass]], s,
                     IF R.mpc = "fsnap" THEN C_RETIRED_FORGET ELSE C_RETIRED_DEAD)
    IN IF R.mpc = "fsnap" THEN [f EXCEPT !.mpc = "fdereg"] ELSE NextDeadF(f, s)

\* broken (PassCopyFirst): the word store after the passcopy
RetireWordLateF(R) ==
    LET s == R.ms
        f == FinishF([R EXCEPT !.word = [R.word EXCEPT ![s] = <<R.gen[s], 0>>]], s,
                     IF R.mpc = "fword" THEN C_RETIRED_FORGET ELSE C_RETIRED_DEAD)
    IN IF R.mpc = "fword" THEN [f EXCEPT !.mpc = "fdereg"] ELSE NextDeadF(f, s)

\* the caller deregisters the retired descriptor (forget's retired_fd) from
\* its receive poller
DeregF(R) == [R EXCEPT !.poller = {e \in R.poller : e[2] # R.ms}, !.mpc = "idle", !.ms = 0]

\* rocketnet_msgx_socket_table_sweep: the dead-report count is taken; with
\* none, the close loop; with nothing retired either, nothing changes
SweepBeginF(R) ==
    IF R.deadReports > 0 THEN [R EXCEPT !.deadReports = 0, !.mpc = "sdead", !.ms = 0]
    ELSE IF R.numRetired > 0 THEN [R EXCEPT !.mpc = "sclose"]
    ELSE R

\* the dead-report loop at slot ms: the report is taken; one about the
\* slot's open generation cools the address down, the caller's retire hook
\* deregisters the descriptor, and the retire's first step runs
RetireDeadF(R) ==
    LET s == R.ms
        r == R.deadRep[s]
        taken == [R EXCEPT !.deadRep = [R.deadRep EXCEPT ![s] = 0]]
    IN IF r = 0 \/ R.state[s] # OPEN \/ r - 1 # R.gen[s]
          THEN NextDeadF(taken, s)
          ELSE RetireFirstF([taken EXCEPT
                   !.coolAddr = FnSet(R.coolAddr, CoolOf(R.addr[s]), R.addr[s]),
                   !.coolUntil = FnSet(R.coolUntil, CoolOf(R.addr[s]), update + Cooldown),
                   !.poller = {e \in R.poller : e[2] # s}], s, <<"ssnap", "sword">>)

\* the close loop at retired[mi]: closed and freed once quiescent, else kept
\* at retired[mk]; after the last entry num_retired is the kept count
CloseF(R) ==
    LET s == R.retired[R.mi]
        q == Quiescent(R, s)
        nk == IF q THEN R.mk ELSE R.mk + 1
        c == IF q
                THEN [R EXCEPT !.kfd = KClose(R.kfd, R.fd[s]),
                               !.kdead = KUndead(R.kdead, R.fd[s]),
                               !.fd = [R.fd EXCEPT ![s] = -1],
                               !.state = [R.state EXCEPT ![s] = FREE],
                               !.deadRep = IF ClearReportAtFree THEN [R.deadRep EXCEPT ![s] = 0] ELSE R.deadRep,
                               !.freeSlots = [R.freeSlots EXCEPT ![R.numFree] = s],
                               !.numFree = R.numFree + 1,
                               !.counters = [R.counters EXCEPT ![C_CLOSED] = @ + 1]]
                ELSE [R EXCEPT !.retired = [R.retired EXCEPT ![R.mk] = s]]
    IN IF R.mi + 1 = R.numRetired
          THEN [c EXCEPT !.mpc = "idle", !.numRetired = nk, !.mi = 0, !.mk = 0]
          ELSE [c EXCEPT !.mi = R.mi + 1, !.mk = nk]

\* main's next step inside a call
MainStepF(R) ==
    CASE R.mpc \in {"fsnap", "ssnap"} -> RetireF(R)
      [] R.mpc \in {"fword", "sword"} -> RetireWordLateF(R)
      [] R.mpc = "fdereg" -> DeregF(R)
      [] R.mpc = "sdead" -> RetireDeadF(R)
      [] R.mpc = "sclose" -> CloseF(R)

\* a call run to its end: main's steps in order until it is idle
RECURSIVE RunF(_)
RunF(R) ==
    IF R.mpc = "idle" THEN R
    ELSE CHOOSE M \in {RunF(N) : N \in {MainStepF(R)}} : TRUE   \* N is a value, not an expression evaluated at each use

\* the fine-grained actions: each one step of a call, holders free to run
\* between them

Forget(a) ==
    /\ Idle
    /\ Apply(ForgetF(MainV, a))
    /\ UNCHANGED <<update, opensLeft, holdv, ghostv>>

\* RNMsgxSocketTable Retire
Retire ==
    /\ mpc \in {"fsnap", "ssnap"}
    /\ Apply(RetireF(MainV))
    /\ UNCHANGED <<update, opensLeft, holdv, ghostv>>

\* broken (PassCopyFirst): the word store after the passcopy
RetireWordLate ==
    /\ PassCopyFirst
    /\ mpc \in {"fword", "sword"}
    /\ Apply(RetireWordLateF(MainV))
    /\ UNCHANGED <<update, opensLeft, holdv, ghostv>>

ForgetDereg ==
    /\ mpc = "fdereg"
    /\ Apply(DeregF(MainV))
    /\ UNCHANGED <<update, opensLeft, holdv, ghostv>>

SweepBegin ==
    /\ Idle
    /\ Apply(SweepBeginF(MainV))
    /\ UNCHANGED <<update, opensLeft, holdv, ghostv>>

RetireDead ==
    /\ mpc = "sdead"
    /\ Apply(RetireDeadF(MainV))
    /\ UNCHANGED <<update, opensLeft, holdv, ghostv>>

Close ==
    /\ mpc = "sclose"
    /\ mi < numRetired
    /\ Apply(CloseF(MainV))
    /\ UNCHANGED <<update, opensLeft, holdv, ghostv>>

\* the call-level actions: a whole call by main with no holder step inside
\* it, the same steps as above in the code's order.  A trace sees the table
\* between calls, so a call is one record.  Every behaviour with one is a
\* behaviour of the fine-grained actions in which no holder runs in the call.

ForgetCall(a) ==
    /\ Idle
    /\ Apply(RunF(ForgetF(MainV, a)))
    /\ UNCHANGED <<update, opensLeft, holdv, ghostv>>

SweepCall ==
    /\ Idle
    /\ Apply(RunF(SweepBeginF(MainV)))
    /\ UNCHANGED <<update, opensLeft, holdv, ghostv>>

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

\* the send, in the same pass: the kernel sends to the descriptor's remote; a
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
    \/ \E a \in Addrs : Mark(a) \/ Forget(a) \/ ForgetCall(a)
    \/ BeginUpdate
    \/ SweepBegin
    \/ SweepCall
    \/ Retire
    \/ RetireWordLate
    \/ ForgetDereg
    \/ RetireDead
    \/ Close
    \/ \E a \in Addrs, h \in TxHolders : Stamp(a, h)
    \/ \E h \in Holders : HolderBegin(h) \/ HolderEnd(h) \/ ReportDead(h)
    \/ \E h \in TxHolders : TxSend(h) \/ \E i \in 1..Len(pkts) : TxResolve(h, i)
    \/ \E e \in poller : RxPoll(e)
    \/ RxResolve
    \/ RxRead
    \/ EnvOpen
    \/ \E f \in Fds : EnvClose(f) \/ Disconnect(f)

\* Next without the call-level actions: the fine-grained steps already take
\* every call, with or without a holder step inside it, so the liveness
\* configurations check the fine-grained model alone (a call-level step adds
\* edges between states the fine-grained steps already reach)
NextFine ==
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
    /\ Init /\ [][NextFine]_vars
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
    /\ DOMAIN index \subseteq Entries /\ \A e \in DOMAIN index : index[e] \in Slots
    /\ DOMAIN coolAddr \subseteq 0..(CoolEntries - 1) /\ DOMAIN coolUntil = DOMAIN coolAddr
    /\ \A e \in DOMAIN coolAddr : coolAddr[e] \in {0} \cup Addrs
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
    /\ {index[e] : e \in DOMAIN index} = {s \in Slots : state[s] = OPEN}
    /\ Cardinality(DOMAIN index) = numOpen

\* between main's calls the word of an open slot is its generation and
\* descriptor, and of any other slot its generation and no descriptor
WordMatchesSlot ==
    Idle =>
    \A s \in Slots :
        word[s] = <<gen[s], IF state[s] = OPEN THEN fd[s] + 1 ELSE 0>>

\* between main's calls every open socket is watched by the receive poller,
\* or reported dead at its own generation (the next sweep retires it): a
\* socket is never left open and unread
OpenWatched ==
    Idle =>
    \A s \in Slots :
        state[s] = OPEN =>
            \/ <<fd[s], s, gen[s], addr[s]>> \in poller
            \/ deadRep[s] = gen[s] + 1

\* between main's calls a free slot holds no dead report
FreeHasNoReport == Idle => \A s \in Slots : state[s] = FREE => deadRep[s] = 0

\* a socket in quarantine is closed and its slot freed
RetiredClosed == \A s \in Slots : state[s] = RETIRED ~> state[s] = FREE

-----------------------------------------------------------------------------
(* The checked configurations use this VIEW: the state with every value    *)
(* that no later step reads replaced by a constant.  A FREE slot's address *)
(* and an OPEN slot's passcopy are written before they are read again; the *)
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

\* a live passcopy only as what Quiescent reads of it: whether the holder is
\* still inside the pass copied.  A pass only grows, so once it moves on
\* from the passcopy it never equals it again
Blocks(s) == [h \in Holders |-> LiveSnap(s) /\ Odd(snap[s][h]) /\ pass[h] = snap[s][h]]

View ==
    <<word, deadRep, state, fd, gen,
      [s \in Slots |-> IF state[s] = FREE THEN 0 ELSE addr[s]],
      [s \in Slots |-> Blocks(s)],
      pass, deadReports,
      [i \in Slots |-> IF i < numFree THEN freeSlots[i] ELSE -1], numFree,
      [i \in Slots |-> IF i < numRetired THEN retired[i] ELSE -1], numRetired,
      numOpen, index,
      [e \in 0..(CoolEntries - 1) |-> IF e \in DOMAIN coolUntil /\ coolUntil[e] > update THEN <<coolAddr[e], coolUntil[e] - update>> ELSE 0],
      CounterView, update, opensLeft, mainv, kernv,
      [i \in 1..Len(pkts) |-> IF pkts[i].st \in {"queued", "resolved"} THEN pkts[i]
                                                                       ELSE [st |-> pkts[i].st, h |-> pkts[i].h]],
      cur, rep, rxEv, rxFd, crossed, onClosed>>

=============================================================================
