---------------------------- MODULE RNPacketLease ----------------------------
(***************************************************************************)
(* The packet lease table of sdk/rocketnet_packet_lease.h:                *)
(* rocketnet_packet_leases_t insert, resolve, take, retired and reset.    *)
(*                                                                         *)
(* The table is an open-addressed array of Capacity entries.  Each entry  *)
(* has a state word, ( epoch << 2 ) | phase, and the fields of the leased *)
(* context.  Phase 0 is empty, 1 writing, 2 active, 3 consumed; an entry *)
(* belongs to the current epoch when state >> 2 equals the epoch.  Game   *)
(* threads call insert (start_packet), take and resolve (finish, abort    *)
(* and the capacity query) and retired (a refused finish) concurrently    *)
(* during the fill phase; only the joined main thread calls reset, which  *)
(* bumps the epoch, zeroes the used count and leaves every entry in       *)
(* place.                                                                  *)
(*                                                                         *)
(* insert counts an attempt in used (refusing at Capacity / 2), takes a   *)
(* ticket, probes from the frame's hash for the first entry not of the    *)
(* current epoch, claims it by compare-and-swap to writing, writes the    *)
(* backend id and the context fields in any order, and stores active.     *)
(* resolve probes from the same hash: an entry of another epoch ends the  *)
(* chain; an entry in phase 1 is waited on (the spin of the code) before  *)
(* its fields are read; a frame and ticket mismatch continues the probe,  *)
(* any other field mismatch refuses; take compare-and-swaps active to     *)
(* consumed, resolve only tests that the state is active.  retired scans  *)
(* the whole table for an entry of an earlier epoch still in phase 2 that *)
(* equals the supplied context.                                            *)
(*                                                                         *)
(* The model carries the code's arithmetic: the state word is the epoch   *)
(* shifted by two plus the phase, in a word of WordMod values (2^64 in    *)
(* the code), the probe is ( slot + 1 ) mod Capacity from the hash, and   *)
(* the epoch stops advancing at EpochLimit (UINT64_MAX >> 2) and then is  *)
(* 0, which refuses every call.                                            *)
(*                                                                         *)
(* Ghost state is the contract.  issued records each lease when insert    *)
(* publishes it; takes counts successful consumes; gen counts resets.     *)
(*                                                                         *)
(* Checked claims:                                                         *)
(*   NoWrongSuccess  take, resolve and retired succeed only for a context *)
(*                   exactly equal to a lease of the current epoch (of an *)
(*                   earlier epoch, never consumed, for retired).          *)
(*   TakeOnce        a lease is consumed at most once.                     *)
(*   PublishedIntact a published lease's entry is not overwritten while    *)
(*                   its epoch is current.                                 *)
(*   ChainIntact     every entry between a lease's hash and its slot is    *)
(*                   of the current epoch, so resolve's probe reaches it.  *)
(*   ProbeBound      the entries of the current epoch never exceed         *)
(*                   Capacity / 2.                                         *)
(*   InsertFindsSlot insert never runs out of probes.                      *)
(*   NoRacyRead      no thread reads an entry's fields while another      *)
(*                   thread may be writing them (a data race).             *)
(*   SpinBounded, ResolveCompletes, InsertCompletes  (liveness) see the    *)
(*                   README: the spin ends when the writer takes its       *)
(*                   steps; it is unbounded if the writer pauses.          *)
(*                                                                         *)
(* Rule switches, each changed in a reversed witness or a finding:        *)
(*   UsedLimit       Capacity / 2 in the code.                             *)
(*   UseCas          insert claims its entry by compare-and-swap.          *)
(*   ConsumeCas      take consumes by compare-and-swap.                    *)
(*   OnWriting       "spin" is the code; "skip" continues the probe past  *)
(*                   an entry in phase 1; "read" reads its fields.         *)
(*   BumpEpoch       reset advances the epoch.                             *)
(*   EpochLimited    reset stops at EpochLimit and then refuses.           *)
(*   ValidCallers    resolve and take are given only leases of the       *)
(*                   current fill; FALSE also gives them stale copies,    *)
(*                   the caller misuse the code refuses.                   *)
(*   RetiredOn       the harness calls retired (the cases without it     *)
(*                   check resolve and take alone).                        *)
(*   RetiredValidates retired rereads the state after reading the fields  *)
(*                   and discards the entry if it was rewritten.           *)
(***************************************************************************)
EXTENDS Integers, Sequences, FiniteSets

CONSTANTS Capacity,      \* ROCKETNET_SERVER_PACKET_LEASES-style table size, a power of two
          Threads,       \* game threads
          Frames,        \* frame identities
          Vals,          \* the other context fields, collapsed to one value
          HashSpread,    \* frames hash to ( frame - 1 ) mod HashSpread; 1 collides all
          MaxOps1,       \* operations thread 1 performs
          MaxOps2,       \* operations thread 2 performs
          MaxResets,     \* resets the harness performs
          EpochLimit,    \* the largest epoch before 0 (UINT64_MAX >> 2)
          UsedLimit,
          UseCas, ConsumeCas, BumpEpoch, EpochLimited, RetiredOn, RetiredValidates, OnWriting,
          ValidCallers   \* resolve and take are given only leases of the current fill

ASSUME Threads = {1, 2}
ASSUME Capacity \in {2, 4, 8} /\ HashSpread \in 1..Capacity
ASSUME OnWriting \in {"spin", "skip", "read"}
ASSUME {UseCas, ConsumeCas, BumpEpoch, EpochLimited, RetiredOn, RetiredValidates, ValidCallers} \subseteq BOOLEAN

Slots == 0..Capacity - 1
OpsMax(t) == IF t = 1 THEN MaxOps1 ELSE MaxOps2
MaxTickets == MaxOps1 + MaxOps2
Tickets == 1..MaxTickets
WordMod == 4 * (EpochLimit + 1)
NoLease == <<>>

VARIABLES st, fr, tk, vl, bk,         \* per entry: state word, frame, ticket, value, backend id
          used, epoch, nextTicket,    \* the table's used count and epoch; the ticket counter
          pc, slot, probe, seen, mode, af, av, at, myT, todo, ops,   \* per thread
          issued, takes, gen, wrong, racy, exhausted                 \* ghost

entryVars == <<st, fr, tk, vl, bk>>
tableVars == <<used, epoch, nextTicket>>
tvars == <<pc, slot, probe, seen, mode, af, av, at, myT, todo, ops>>
ghostVars == <<issued, takes, gen, wrong, racy, exhausted>>
vars == <<st, fr, tk, vl, bk, used, epoch, nextTicket, pc, slot, probe, seen, mode, af, av,
          at, myT, todo, ops, issued, takes, gen, wrong, racy, exhausted>>

Word(e, p) == (e * 4 + p) % WordMod
EpochOf(w) == w \div 4
PhaseOf(w) == w % 4
Hash(f) == (f - 1) % HashSpread
Next1(s) == (s + 1) % Capacity
Fields == {"b", "f", "t", "v"}

Init ==
    /\ st = [s \in Slots |-> 0]
    /\ fr = [s \in Slots |-> 0]
    /\ tk = [s \in Slots |-> 0]
    /\ vl = [s \in Slots |-> 0]
    /\ bk = [s \in Slots |-> 0]
    /\ used = 0
    /\ epoch = 1
    /\ nextTicket = 1
    /\ pc = [t \in Threads |-> "idle"]
    /\ slot = [t \in Threads |-> 0]
    /\ probe = [t \in Threads |-> 0]
    /\ seen = [t \in Threads |-> 0]
    /\ mode = [t \in Threads |-> "take"]
    /\ af = [t \in Threads |-> 0]
    /\ av = [t \in Threads |-> 0]
    /\ at = [t \in Threads |-> 0]
    /\ myT = [t \in Threads |-> 0]
    /\ todo = [t \in Threads |-> {}]
    /\ ops = [t \in Threads |-> 0]
    /\ issued = [k \in Tickets |-> NoLease]
    /\ takes = [k \in Tickets |-> 0]
    /\ gen = 0
    /\ wrong = FALSE
    /\ racy = FALSE
    /\ exhausted = FALSE

\* a lease of the current generation holds this frame, or an insert is in flight on it
FrameBusy(f) ==
    \/ \E k \in Tickets : issued[k] # NoLease /\ issued[k].gen = gen /\ issued[k].f = f
    \/ \E u \in Threads : pc[u] \in {"i_used", "i_ticket", "i_load", "i_cas", "i_w"} /\ af[u] = f

\* ------------------------------------------------------------------------
\* insert, rocketnet_packet_leases_t::insert

StartInsert(t) ==
    /\ pc[t] = "idle"
    /\ ops[t] < OpsMax(t)
    /\ \E f \in Frames, v \in Vals :
          /\ ~FrameBusy(f)
          /\ af' = [af EXCEPT ![t] = f]
          /\ av' = [av EXCEPT ![t] = v]
    /\ ops' = [ops EXCEPT ![t] = @ + 1]
    /\ pc' = [pc EXCEPT ![t] = IF epoch = 0 THEN "idle" ELSE "i_used"]
    /\ UNCHANGED <<entryVars, tableVars, ghostVars, slot, probe, seen, mode, at, myT, todo>>

\* used.fetch_add( 1 ) >= Capacity / 2 refuses
IUsed(t) ==
    /\ pc[t] = "i_used"
    /\ used' = used + 1
    /\ pc' = [pc EXCEPT ![t] = IF used >= UsedLimit THEN "idle" ELSE "i_ticket"]
    /\ UNCHANGED <<entryVars, epoch, nextTicket, ghostVars, slot, probe, seen, mode, af, av,
                   at, myT, todo, ops>>

\* rocketnet_next_packet_lease_id, then the probe starts at the frame's hash
ITicket(t) ==
    /\ pc[t] = "i_ticket"
    /\ myT' = [myT EXCEPT ![t] = nextTicket]
    /\ nextTicket' = nextTicket + 1
    /\ slot' = [slot EXCEPT ![t] = Hash(af[t])]
    /\ probe' = [probe EXCEPT ![t] = 0]
    /\ pc' = [pc EXCEPT ![t] = "i_load"]
    /\ UNCHANGED <<entryVars, used, epoch, ghostVars, seen, mode, af, av, at, todo, ops>>

\* load the entry's state; an entry of the current epoch is skipped
ILoad(t) ==
    /\ pc[t] = "i_load"
    /\ seen' = [seen EXCEPT ![t] = st[slot[t]]]
    /\ IF EpochOf(st[slot[t]]) = epoch
         THEN IF probe[t] + 1 >= Capacity
                THEN /\ pc' = [pc EXCEPT ![t] = "idle"]
                     /\ exhausted' = TRUE
                     /\ UNCHANGED <<slot, probe>>
                ELSE /\ slot' = [slot EXCEPT ![t] = Next1(@)]
                     /\ probe' = [probe EXCEPT ![t] = @ + 1]
                     /\ UNCHANGED <<pc, exhausted>>
         ELSE /\ pc' = [pc EXCEPT ![t] = "i_cas"]
              /\ UNCHANGED <<slot, probe, exhausted>>
    /\ UNCHANGED <<entryVars, tableVars, mode, af, av, at, myT, todo, ops, issued, takes, gen,
                   wrong, racy>>

\* compare-and-swap the loaded state to writing; a lost race continues the probe
ICas(t) ==
    /\ pc[t] = "i_cas"
    /\ IF ~UseCas \/ st[slot[t]] = seen[t]
         THEN /\ st' = [st EXCEPT ![slot[t]] = Word(epoch, 1)]
              /\ pc' = [pc EXCEPT ![t] = "i_w"]
              /\ todo' = [todo EXCEPT ![t] = Fields]
              /\ UNCHANGED <<slot, probe, exhausted>>
         ELSE /\ UNCHANGED <<st, todo>>
              /\ IF probe[t] + 1 >= Capacity
                   THEN /\ pc' = [pc EXCEPT ![t] = "idle"]
                        /\ exhausted' = TRUE
                        /\ UNCHANGED <<slot, probe>>
                   ELSE /\ pc' = [pc EXCEPT ![t] = "i_load"]
                        /\ slot' = [slot EXCEPT ![t] = Next1(@)]
                        /\ probe' = [probe EXCEPT ![t] = @ + 1]
                        /\ UNCHANGED exhausted
    /\ UNCHANGED <<fr, tk, vl, bk, tableVars, seen, mode, af, av, at, myT, ops, issued, takes,
                   gen, wrong, racy>>

\* the backend id and the context fields, in any order
IWrite(t) ==
    /\ pc[t] = "i_w"
    /\ todo[t] # {}
    /\ \E fld \in todo[t] :
          /\ todo' = [todo EXCEPT ![t] = @ \ {fld}]
          /\ \/ fld = "b" /\ bk' = [bk EXCEPT ![slot[t]] = myT[t]] /\ UNCHANGED <<fr, tk, vl>>
             \/ fld = "f" /\ fr' = [fr EXCEPT ![slot[t]] = af[t]] /\ UNCHANGED <<tk, vl, bk>>
             \/ fld = "t" /\ tk' = [tk EXCEPT ![slot[t]] = myT[t]] /\ UNCHANGED <<fr, vl, bk>>
             \/ fld = "v" /\ vl' = [vl EXCEPT ![slot[t]] = av[t]] /\ UNCHANGED <<fr, tk, bk>>
    /\ UNCHANGED <<st, tableVars, pc, slot, probe, seen, mode, af, av, at, myT, ops, ghostVars>>

\* the release store of active publishes the lease
IPublish(t) ==
    /\ pc[t] = "i_w"
    /\ todo[t] = {}
    /\ st' = [st EXCEPT ![slot[t]] = Word(epoch, 2)]
    /\ issued' = [issued EXCEPT ![myT[t]] = [f |-> af[t], v |-> av[t], gen |-> gen, slot |-> slot[t]]]
    /\ pc' = [pc EXCEPT ![t] = "idle"]
    /\ UNCHANGED <<fr, tk, vl, bk, tableVars, slot, probe, seen, mode, af, av, at, myT, todo, ops,
                   takes, gen, wrong, racy, exhausted>>

\* ------------------------------------------------------------------------
\* resolve and take, rocketnet_packet_leases_t::resolve and take

\* a thread presents a copy of a published lease, exact or with its value changed
StartResolve(t) ==
    /\ pc[t] = "idle"
    /\ ops[t] < OpsMax(t)
    /\ \E m \in {"take", "check"}, k \in Tickets, v \in Vals :
          /\ issued[k] # NoLease
          /\ (ValidCallers => issued[k].gen = gen)
          /\ mode' = [mode EXCEPT ![t] = m]
          /\ af' = [af EXCEPT ![t] = issued[k].f]
          /\ at' = [at EXCEPT ![t] = k]
          /\ av' = [av EXCEPT ![t] = v]
    /\ ops' = [ops EXCEPT ![t] = @ + 1]
    /\ slot' = [slot EXCEPT ![t] = Hash(af'[t])]
    /\ probe' = [probe EXCEPT ![t] = 0]
    /\ pc' = [pc EXCEPT ![t] = IF epoch = 0 THEN "idle" ELSE "r_load"]
    /\ UNCHANGED <<entryVars, tableVars, ghostVars, seen, myT, todo>>

\* load the state: another epoch ends the chain; phase 1 is the spin
RLoad(t) ==
    /\ pc[t] = "r_load"
    /\ seen' = [seen EXCEPT ![t] = st[slot[t]]]
    /\ IF EpochOf(st[slot[t]]) # epoch
         THEN /\ pc' = [pc EXCEPT ![t] = "idle"]
              /\ UNCHANGED <<slot, probe>>
         ELSE IF PhaseOf(st[slot[t]]) # 1
                THEN /\ pc' = [pc EXCEPT ![t] = "r_cmp"]
                     /\ UNCHANGED <<slot, probe>>
                ELSE IF OnWriting = "spin"
                       THEN /\ pc' = [pc EXCEPT ![t] = "r_spin"]
                            /\ UNCHANGED <<slot, probe>>
                       ELSE IF OnWriting = "read"
                              THEN /\ pc' = [pc EXCEPT ![t] = "r_cmp"]
                                   /\ UNCHANGED <<slot, probe>>
                              ELSE IF probe[t] + 1 >= Capacity
                                     THEN /\ pc' = [pc EXCEPT ![t] = "idle"]
                                          /\ UNCHANGED <<slot, probe>>
                                     ELSE /\ slot' = [slot EXCEPT ![t] = Next1(@)]
                                          /\ probe' = [probe EXCEPT ![t] = @ + 1]
                                          /\ UNCHANGED pc
    /\ UNCHANGED <<entryVars, tableVars, mode, af, av, at, myT, todo, ops, ghostVars>>

\* while ( ( state & 3 ) == 1 ) state = load: the entry is still being written
RSpinWait(t) ==
    /\ pc[t] = "r_spin"
    /\ PhaseOf(st[slot[t]]) = 1
    /\ UNCHANGED vars

\* the writer's release store ended the spin
RSpinDone(t) ==
    /\ pc[t] = "r_spin"
    /\ PhaseOf(st[slot[t]]) # 1
    /\ seen' = [seen EXCEPT ![t] = st[slot[t]]]
    /\ pc' = [pc EXCEPT ![t] = "r_cmp"]
    /\ UNCHANGED <<entryVars, tableVars, slot, probe, mode, af, av, at, myT, todo, ops, ghostVars>>

\* an entry is rewritten when it is claimed again, so a read is a race if the entry is
\* in phase 1 now or was rewritten since the state was loaded
Racy(t) == PhaseOf(st[slot[t]]) = 1 \/ EpochOf(st[slot[t]]) # EpochOf(seen[t])

\* read the entry's fields and compare: frame and ticket continue the probe, the
\* other fields refuse
RCmp(t) ==
    /\ pc[t] = "r_cmp"
    /\ racy' = (racy \/ Racy(t))
    /\ IF fr[slot[t]] # af[t] \/ tk[slot[t]] # at[t]
         THEN IF probe[t] + 1 >= Capacity
                THEN /\ pc' = [pc EXCEPT ![t] = "idle"]
                     /\ UNCHANGED <<slot, probe>>
                ELSE /\ pc' = [pc EXCEPT ![t] = "r_load"]
                     /\ slot' = [slot EXCEPT ![t] = Next1(@)]
                     /\ probe' = [probe EXCEPT ![t] = @ + 1]
         ELSE /\ pc' = [pc EXCEPT ![t] = IF vl[slot[t]] # av[t] THEN "idle" ELSE "r_commit"]
              /\ UNCHANGED <<slot, probe>>
    /\ UNCHANGED <<entryVars, tableVars, seen, mode, af, av, at, myT, todo, ops, issued, takes,
                   gen, wrong, exhausted>>

\* the success check against the contract
Exact(t, s) ==
    /\ issued[at[t]] # NoLease
    /\ issued[at[t]].f = af[t]
    /\ issued[at[t]].v = av[t]
    /\ issued[at[t]].gen = gen
    /\ bk[s] = at[t]

\* take: compare-and-swap active to consumed; resolve: the loaded state was active
RCommit(t) ==
    /\ pc[t] = "r_commit"
    /\ LET s == slot[t]
           active == Word(epoch, 2)
           ok == IF mode[t] = "take"
                   THEN IF ConsumeCas THEN st[s] = active ELSE PhaseOf(seen[t]) = 2
                   ELSE seen[t] = active
       IN /\ IF ok /\ mode[t] = "take"
               THEN /\ st' = [st EXCEPT ![s] = Word(epoch, 3)]
                    /\ takes' = [takes EXCEPT ![at[t]] = @ + 1]
               ELSE UNCHANGED <<st, takes>>
          /\ wrong' = (wrong \/ (ok /\ ~Exact(t, s)))
    /\ pc' = [pc EXCEPT ![t] = "idle"]
    /\ UNCHANGED <<fr, tk, vl, bk, tableVars, slot, probe, seen, mode, af, av, at, myT, todo, ops,
                   issued, gen, racy, exhausted>>

\* ------------------------------------------------------------------------
\* retired, rocketnet_packet_leases_t::retired

StartRetired(t) ==
    /\ RetiredOn
    /\ pc[t] = "idle"
    /\ ops[t] < OpsMax(t)
    /\ \E k \in Tickets, v \in Vals :
          /\ issued[k] # NoLease
          /\ af' = [af EXCEPT ![t] = issued[k].f]
          /\ at' = [at EXCEPT ![t] = k]
          /\ av' = [av EXCEPT ![t] = v]
    /\ ops' = [ops EXCEPT ![t] = @ + 1]
    /\ slot' = [slot EXCEPT ![t] = Hash(af'[t])]
    /\ probe' = [probe EXCEPT ![t] = 0]
    /\ pc' = [pc EXCEPT ![t] = IF epoch = 0 THEN "idle" ELSE "t_load"]
    /\ UNCHANGED <<entryVars, tableVars, ghostVars, seen, mode, myT, todo>>

\* load the state: an empty entry ends the scan; the current epoch or a phase other
\* than active is skipped
TLoad(t) ==
    /\ pc[t] = "t_load"
    /\ seen' = [seen EXCEPT ![t] = st[slot[t]]]
    /\ IF EpochOf(st[slot[t]]) = 0
         THEN /\ pc' = [pc EXCEPT ![t] = "idle"]
              /\ UNCHANGED <<slot, probe>>
         ELSE IF EpochOf(st[slot[t]]) = epoch \/ PhaseOf(st[slot[t]]) # 2
                THEN IF probe[t] + 1 >= Capacity
                       THEN /\ pc' = [pc EXCEPT ![t] = "idle"]
                            /\ UNCHANGED <<slot, probe>>
                       ELSE /\ slot' = [slot EXCEPT ![t] = Next1(@)]
                            /\ probe' = [probe EXCEPT ![t] = @ + 1]
                            /\ UNCHANGED pc
                ELSE /\ pc' = [pc EXCEPT ![t] = "t_cmp"]
                     /\ UNCHANGED <<slot, probe>>
    /\ UNCHANGED <<entryVars, tableVars, mode, af, av, at, myT, todo, ops, ghostVars>>

\* read the fields of the old entry and compare them with the supplied context
TCmp(t) ==
    /\ pc[t] = "t_cmp"
    /\ LET s == slot[t]
           rewritten == st[s] # seen[t]
           match == fr[s] = af[t] /\ tk[s] = at[t] /\ vl[s] = av[t]
           exactOld == /\ issued[at[t]] # NoLease
                       /\ issued[at[t]].f = af[t]
                       /\ issued[at[t]].v = av[t]
                       /\ issued[at[t]].gen < gen
                       /\ takes[at[t]] = 0
       IN IF RetiredValidates /\ rewritten
            THEN \* the state changed under the read: discard the entry
                 /\ IF probe[t] + 1 >= Capacity
                      THEN /\ pc' = [pc EXCEPT ![t] = "idle"]
                           /\ UNCHANGED <<slot, probe>>
                      ELSE /\ pc' = [pc EXCEPT ![t] = "t_load"]
                           /\ slot' = [slot EXCEPT ![t] = Next1(@)]
                           /\ probe' = [probe EXCEPT ![t] = @ + 1]
                 /\ UNCHANGED <<racy, wrong>>
            ELSE /\ racy' = (racy \/ Racy(t))
                 /\ IF match
                      THEN /\ wrong' = (wrong \/ ~exactOld)
                           /\ pc' = [pc EXCEPT ![t] = "idle"]
                           /\ UNCHANGED <<slot, probe>>
                      ELSE /\ UNCHANGED wrong
                           /\ IF probe[t] + 1 >= Capacity
                                THEN /\ pc' = [pc EXCEPT ![t] = "idle"]
                                     /\ UNCHANGED <<slot, probe>>
                                ELSE /\ pc' = [pc EXCEPT ![t] = "t_load"]
                                     /\ slot' = [slot EXCEPT ![t] = Next1(@)]
                                     /\ probe' = [probe EXCEPT ![t] = @ + 1]
    /\ UNCHANGED <<entryVars, tableVars, seen, mode, af, av, at, myT, todo, ops, issued, takes,
                   gen, exhausted>>

\* ------------------------------------------------------------------------
\* reset, called by the joined main thread

Reset ==
    /\ \A t \in Threads : pc[t] = "idle"
    /\ gen < MaxResets
    /\ gen' = gen + 1
    /\ epoch' = IF ~BumpEpoch THEN epoch
                ELSE IF epoch # 0 /\ (epoch < EpochLimit \/ ~EpochLimited) THEN epoch + 1 ELSE 0
    /\ used' = 0
    /\ UNCHANGED <<entryVars, nextTicket, tvars, issued, takes, wrong, racy, exhausted>>

\* the harness is finished: every thread has used its operations and every reset is done
Done ==
    /\ \A t \in Threads : pc[t] = "idle" /\ ops[t] = OpsMax(t)
    /\ gen = MaxResets
    /\ UNCHANGED vars

Next ==
    \/ Done
    \/ \E t \in Threads :
          \/ StartInsert(t) \/ IUsed(t) \/ ITicket(t) \/ ILoad(t) \/ ICas(t) \/ IWrite(t)
          \/ IPublish(t)
          \/ StartResolve(t) \/ RLoad(t) \/ RSpinWait(t) \/ RSpinDone(t) \/ RCmp(t) \/ RCommit(t)
          \/ StartRetired(t) \/ TLoad(t) \/ TCmp(t)
    \/ Reset

Spec == Init /\ [][Next]_vars

ThreadStep(t) ==
    \/ StartInsert(t) \/ IUsed(t) \/ ITicket(t) \/ ILoad(t) \/ ICas(t) \/ IWrite(t) \/ IPublish(t)
    \/ StartResolve(t) \/ RLoad(t) \/ RSpinDone(t) \/ RCmp(t) \/ RCommit(t)
    \/ StartRetired(t) \/ TLoad(t) \/ TCmp(t)

\* Every thread is scheduled: it takes a step whenever one is enabled.
SpecFair == Spec /\ \A t \in Threads : WF_vars(ThreadStep(t))

\* Only thread 1 is scheduled.  Every other thread may stop between any two steps,
\* the way a preempted thread does.
SpecSolo == Spec /\ WF_vars(ThreadStep(1))

-----------------------------------------------------------------------------

TypeOK ==
    /\ st \in [Slots -> 0..WordMod - 1]
    /\ used \in 0..MaxTickets
    /\ epoch \in 0..EpochLimit + 1
    /\ nextTicket \in 1..MaxTickets + 1
    /\ pc \in [Threads -> {"idle", "i_used", "i_ticket", "i_load", "i_cas", "i_w", "r_load",
                           "r_spin", "r_cmp", "r_commit", "t_load", "t_cmp"}]
    /\ {wrong, racy, exhausted} \subseteq BOOLEAN

NoWrongSuccess == ~wrong
TakeOnce == \A k \in Tickets : takes[k] <= 1
NoRacyRead == ~racy
InsertFindsSlot == ~exhausted

Published == {k \in Tickets : issued[k] # NoLease /\ issued[k].gen = gen /\ epoch # 0}

PublishedIntact ==
    \A k \in Published :
        LET s == issued[k].slot
        IN /\ EpochOf(st[s]) = epoch /\ PhaseOf(st[s]) \in {2, 3}
           /\ fr[s] = issued[k].f /\ tk[s] = k /\ vl[s] = issued[k].v /\ bk[s] = k

ChainIntact ==
    \A k \in Published :
        LET s == issued[k].slot
            h == Hash(issued[k].f)
        IN \A i \in 0..((s - h + Capacity) % Capacity) : EpochOf(st[(h + i) % Capacity]) = epoch

ProbeBound ==
    epoch # 0 => Cardinality({s \in Slots : EpochOf(st[s]) = epoch}) <= Capacity \div 2

\* Liveness: a thread that is spinning stops spinning.
SpinBounded ==
    \A t \in Threads : (pc[t] = "r_spin") ~> (pc[t] # "r_spin")

\* Liveness: thread 1's resolve completes, whatever the other threads do.
ResolveCompletes ==
    (pc[1] \in {"r_load", "r_spin", "r_cmp", "r_commit"}) ~> (pc[1] = "idle")

\* Liveness: thread 1's insert completes, whatever the other threads do.
InsertCompletes ==
    (pc[1] \in {"i_used", "i_ticket", "i_load", "i_cas", "i_w"}) ~> (pc[1] = "idle")

=============================================================================
