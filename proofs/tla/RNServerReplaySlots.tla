------------------------- MODULE RNServerReplaySlots -------------------------
(***************************************************************************)
(* The per-slot records of the server's kernel replay walk, as kept in     *)
(* sdk/rocketnet_server_replay_marks.cpp.  RNXdpReplay.tla keys its walk   *)
(* by address; the code keys it by client slot, and a slot can be reused  *)
(* at another address while the forget of its old one is still owed.      *)
(* This model carries the per-slot records and the slot reuse.             *)
(*                                                                         *)
(* A client slot is connected at an address with the session's            *)
(* highest_opened, or free.  Its record (rocketnet_server_replay_slot_     *)
(* mark_t) holds seen and seen_address, has_mark and marked_value, and     *)
(* mark_owed.  rocketnet_server_replay_marks_walk runs once per update:    *)
(*                                                                         *)
(*   the forget pass  every seen slot that is no longer connected, or is   *)
(*                    connected at another address, asks the sink to       *)
(*                    forget seen_address; only an accepted forget clears  *)
(*                    the record (the memset), and it clears every other   *)
(*                    slot's record of that address too (the code looks    *)
(*                    only when supersede_owed says the walk before left   *)
(*                    a refused forget, the one way two records come to    *)
(*                    name one address: DistinctUnlessOwed)                *)
(*   the mark pass    every connected slot: a slot whose record still      *)
(*                    names another address is skipped (its forget is      *)
(*                    owed first), its own address still marked without    *)
(*                    taking the record; otherwise the record is seen at the *)
(*                    address, and a slot first seen, owing a mark or      *)
(*                    whose highest_opened differs from its last mark asks *)
(*                    the sink to mark it; a refused mark is owed, and an  *)
(*                    accepted one clears every other slot's record of the *)
(*                    address (the code looks only when a forget of this   *)
(*                    walk was refused, the one way such a record is left) *)
(*                                                                         *)
(* The sink's answers are chosen freely: a forget or a mark may be refused *)
(* (rocketnet_xdp_replay_marks_forget and _mark refuse when their array    *)
(* is full).  The transport commits after the walk, every forget then      *)
(* every mark; the kernel's own refusals belong to RNXdpReplayMarksLayout  *)
(* and RNXdpReplay, so here the commit applies everything.                 *)
(*                                                                         *)
(* With ForgetRefusable the sink may refuse any forget; without it every   *)
(* forget is queued.                                                       *)
(*                                                                         *)
(* The ghost owed holds each <<slot, address>> the walk saw the slot       *)
(* connected at, until a forget of that address from that slot is          *)
(* accepted, or another slot's queued forget or mark of the address        *)
(* supersedes it.                                                          *)
(*                                                                         *)
(* Reversed witnesses (each flag FALSE is a real bug class):               *)
(*   SkipWhileOwed  the mark pass does not skip a slot whose record names  *)
(*                  another address, and overwrites the identity whose     *)
(*                  forget is owed                                         *)
(*   ClearOnAccept  the forget pass clears the record although the sink    *)
(*                  refused the forget                                     *)
(*   MarkSupersedes a queued mark leaves another slot's record of its      *)
(*                  address: that slot's owed forget, retried, deletes the *)
(*                  marking slot's entry, or that slot, back at the        *)
(*                  address, keeps a mark the kernel no longer holds       *)
(*   ForgetSupersedes  a queued forget leaves another slot's record of its *)
(*                  address, whose mark the commit deletes                 *)
(*   OweOnRefusal   a walk that leaves a refused forget does not raise     *)
(*                  supersede_owed, so the next walk's forget pass skips   *)
(*                  the scan although two records name one address         *)
(*                                                                         *)
(*   MarkWhileSkipped (a definition, overridden by its witness) the skipped *)
(*                  slot's address is not marked: its session is greeted   *)
(*                  with no kernel entry (GreetedSlotHasEntry)              *)
(*   RecordSkip, KeepSkipOwner (definitions, below) the skipped slot's mark *)
(*                  loses its owner (KernelEntryOwned)                      *)
(*   ZeroMarkWaitsForForgets (a definition, below) the XDP sink's rule,    *)
(*                  without which a slot Blocked at a third address is     *)
(*                  greeted with no entry (GreetedSlotHasEntry)             *)
(*   KeptKeyZeroMarkWaits (a definition, below) FALSE is the rule's        *)
(*                  kept-key exception: the same greeting                   *)
(*                  (GreetedSlotHasEntry)                                   *)
(*                                                                         *)
(* A slot's skip mark (skip_marked, skip_address) records the address the *)
(* mark pass marked while the slot was skipped; the forget pass forgets it *)
(* once the slot has left it, kept on a refusal like the record's forget.  *)
(*                                                                         *)
(* ScanWhenOwedOnly FALSE is not a bug: it is the v1.0.0 forget pass, which *)
(* scanned for every queued forget.  Its control reaches the same states,  *)
(* since the scan the code skips clears nothing (DistinctUnlessOwed).      *)
(***************************************************************************)
EXTENDS Integers, FiniteSets

CONSTANTS
    Slots,          \* client slots
    Addrs,          \* client addresses
    MaxHigh,        \* highest_opened runs 0..MaxHigh
    SkipWhileOwed,  \* the mark pass skips a slot seen at another address
    ClearOnAccept,  \* the forget pass clears a record only on an accepted forget
    ForgetRefusable,\* the sink may refuse a forget (its array full)
    MarkSupersedes, \* a queued mark clears every other slot's record of its address
    ForgetSupersedes,\* a queued forget clears every other slot's record of its address
    OweOnRefusal,   \* a walk that leaves a refused forget raises supersede_owed
    ScanWhenOwedOnly,\* the forget pass scans only while supersede_owed is set (FALSE: for every queued forget, as v1.0.0)
    NONE            \* no kernel entry, no address (a model value)

ASSUME MaxHigh \in Nat
ASSUME \A f \in {SkipWhileOwed, ClearOnAccept, ForgetRefusable, MarkSupersedes, ForgetSupersedes, OweOnRefusal, ScanWhenOwedOnly} : f \in BOOLEAN

Highs == 0..MaxHigh
Clear == [ seen |-> FALSE, at |-> NONE, has |-> FALSE, marked |-> 0, owes |-> FALSE, skip |-> FALSE, skipAt |-> NONE ]

\* rocketnet_server_replay_marks_clear_record: the seen record cleared, the
\* skip mark kept (it answers for another address)
ClearRec(r) == [ r EXCEPT !.seen = FALSE, !.at = NONE, !.has = FALSE, !.marked = 0, !.owes = FALSE ]

\* rocketnet_server_replay_marks_clear_skip
ClearSkip(r) == [ r EXCEPT !.skip = FALSE, !.skipAt = NONE ]

\* the record seen at a; a skip mark of a is the record's now
SeenAt(r, a) == IF r.skip /\ r.skipAt = a THEN [ ClearSkip(r) EXCEPT !.seen = TRUE, !.at = a ]
                ELSE [ r EXCEPT !.seen = TRUE, !.at = a ]

(* RecordSkip: an accepted mark of a skipped slot's address is recorded in *)
(* the slot's skip mark (skip_marked, skip_address), and the forget pass    *)
(* forgets that address once the slot is no longer connected at it, kept   *)
(* until the sink accepts like any forget (the design: TRUE).  FALSE is the *)
(* walk before it: the mark had no owner, and a session that ended before   *)
(* its slot's record took the address left the kernel entry for good;       *)
(* MCRNServerReplaySlotsBrokenSkipNotRecorded.cfg must fail                 *)
(* KernelEntryOwned.                                                        *)
RecordSkip == TRUE

(* KeepSkipOwner: a skipped slot whose skip mark still names an address it  *)
(* has left (that forget refused too) is not marked at its new address, so *)
(* the skip mark keeps its owner; nothing holds its greeting, and with the *)
(* XDP sink no slot gets here (ZeroMarkWaitsForForgets) (the design:       *)
(* TRUE).  FALSE marks it and overwrites                                   *)
(* the skip mark; MCRNServerReplaySlotsBrokenSkipOverwritten.cfg, three     *)
(* addresses, must fail KernelEntryOwned.                                   *)
KeepSkipOwner == TRUE

(* ZeroMarkWaitsForForgets: the XDP sink's rule.  It refuses a zero mark    *)
(* while its forget array is full (rocketnet_xdp_replay_sink_mark), and a  *)
(* forget the sink refused in this walk means it is full, so a zero mark   *)
(* is refused in such a walk and the greeting held.  A skipped slot's mark *)
(* is asked at zero and a forget was refused in its walk: no skip mark is  *)
(* taken, and its greeting waits for the record's forget.  FALSE (the default) is any sink, whose answers are free:       *)
(* RecordSkip and KeepSkipOwner keep every entry owned there, and at three *)
(* addresses a Blocked slot is greeted with no entry                       *)
(* (MCRNServerReplaySlotsBrokenZeroMarkTaken.cfg fails GreetedSlotHasEntry; *)
(* MCRNServerReplaySlotsThreeXdpSink.cfg, TRUE, passes it).                 *)
ZeroMarkWaitsForForgets == FALSE

(* KeptKeyZeroMarkWaits: under ZeroMarkWaitsForForgets, a zero mark waits  *)
(* even when the sink keeps a mark of its address that it would replace in *)
(* place (the design: TRUE; rocketnet_xdp_replay_sink_mark tests only the  *)
(* forget array).  The model does not carry the sink's kept marks, so      *)
(* FALSE lets the sink take a skipped slot's zero mark in such a walk, as  *)
(* if a kept mark of its address stood (a first-seen slot's zero mark,     *)
(* which the exception also took, is left refused: such a slot is never    *)
(* one the walk leaves unmarked): the exception of 7307f183e, which        *)
(* recorded a skip mark and then left the slot Blocked, unmarked and not   *)
(* held, at a third address; MCRNServerReplaySlotsBrokenKeptKeyZeroMark.cfg *)
(* must fail GreetedSlotHasEntry.                                          *)
KeptKeyZeroMarkWaits == TRUE

(* MarkWhileSkipped: a connected slot the mark pass skips, its record      *)
(* naming the address whose forget is owed, is still marked at its own     *)
(* address and highest_opened, without taking the record (the design:      *)
(* TRUE).  FALSE is the walk before it: the slot's session was greeted with *)
(* no entry for its address, and a kernel that drops a session packet with  *)
(* no entry (DROPPED_NO_SESSION, RNXdpReplay.tla KernelFilter) dropped its  *)
(* traffic; MCRNServerReplaySlotsBrokenSkipUnmarked.cfg must fail           *)
(* GreetedSlotHasEntry.                                                     *)
MarkWhileSkipped == TRUE

VARIABLES
    conn,           \* conn[i]: the slot's connection, [on, at, high]
    rec,            \* rec[i]: the slot's record, [seen, at (seen_address), has (has_mark), marked (marked_value), owes (mark_owed)]
    kernel,         \* kernel[a]: the replay map's entry for a, NONE for none
    supOwed,        \* supersede_owed: the last walk left a refused forget
    owed,           \* ghost: the <<slot, address>> pairs whose forget is owed
    greeted         \* greeted[i]: the slot's session has had its first keep-alive (the publish after a walk's commit)

vars == <<conn, rec, kernel, supOwed, owed, greeted>>

Init ==
    /\ conn = [ i \in Slots |-> [ on |-> FALSE, at |-> NONE, high |-> 0 ] ]
    /\ rec = [ i \in Slots |-> Clear ]
    /\ kernel = [ a \in Addrs |-> NONE ]
    /\ supOwed = FALSE
    /\ owed = {}
    /\ greeted = [ i \in Slots |-> FALSE ]

\* a client connects in a free slot, at an address no connected slot holds,
\* its session's highest_opened at zero
Connect(i, a) ==
    /\ ~conn[i].on
    /\ \A j \in Slots : conn[j].on => conn[j].at # a
    /\ conn' = [ conn EXCEPT ![i] = [ on |-> TRUE, at |-> a, high |-> 0 ] ]
    /\ greeted' = [ greeted EXCEPT ![i] = FALSE ]
    /\ UNCHANGED <<rec, kernel, supOwed, owed>>

\* a genuine session packet raises highest_opened
Open(i) ==
    /\ conn[i].on
    \* a client sends no session packet before its first keep-alive (the
    \* greeting), so nothing opens before it; the hold then covers exactly
    \* the zero marks, as rocketnet_xdp_replay_sink_mark does
    /\ greeted[i]
    /\ conn[i].high < MaxHigh
    /\ conn' = [ conn EXCEPT ![i].high = @ + 1 ]
    /\ UNCHANGED <<rec, kernel, supOwed, owed, greeted>>

\* the slot is released
Release(i) ==
    /\ conn[i].on
    /\ conn' = [ conn EXCEPT ![i] = [ on |-> FALSE, at |-> NONE, high |-> 0 ] ]
    /\ greeted' = [ greeted EXCEPT ![i] = FALSE ]
    /\ UNCHANGED <<rec, kernel, supOwed, owed>>

\* rocketnet_server_replay_marks_walk, then the transport's commit
Walk ==
    LET F == { i \in Slots : rec[i].seen /\ (~conn[i].on \/ conn[i].at # rec[i].at) }
        \* the skip forgets: a slot whose skip mark names an address it is
        \* no longer connected at
        G == { i \in Slots : rec[i].skip /\ (~conn[i].on \/ conn[i].at # rec[i].skipAt) }
    IN \E fAcc \in IF ForgetRefusable THEN SUBSET F ELSE {F},
          gAcc \in IF ForgetRefusable THEN SUBSET G ELSE {G} :
       LET \* the forgets the sink refused in this walk (refused_forgets > 0)
           refusedAny == (F \ fAcc) # {} \/ (G \ gAcc) # {}
           \* the addresses another slot than k forgets now
           others(k) == { rec[i].at : i \in fAcc \ {k} } \cup { rec[i].skipAt : i \in gAcc \ {k} }
           \* the other slots whose record or skip mark names an address
           \* forgotten now: the code scans for them only while
           \* supersede_owed is set
           FSup == { k \in Slots : ForgetSupersedes /\ (supOwed \/ ~ScanWhenOwedOnly) /\ rec[k].seen /\ rec[k].at \in others(k) }
           GSup == { k \in Slots : ForgetSupersedes /\ (supOwed \/ ~ScanWhenOwedOnly) /\ rec[k].skip /\ rec[k].skipAt \in others(k) }
           r0 == [ i \in Slots |->
                     IF i \in fAcc \/ i \in FSup \/ (i \in F /\ ~ClearOnAccept) THEN ClearRec(rec[i]) ELSE rec[i] ]
           r1 == [ i \in Slots |-> IF i \in gAcc \/ i \in GSup THEN ClearSkip(r0[i]) ELSE r0[i] ]
           forgets == { rec[i].at : i \in fAcc } \cup { rec[i].skipAt : i \in gAcc }
           \* the mark pass's candidates: connected, not skipped
           Cand == { i \in Slots : conn[i].on /\ ~(SkipWhileOwed /\ r1[i].seen /\ r1[i].at # conn[i].at) }
           last(i) == IF r1[i].has THEN r1[i].marked ELSE 0
           Wants == { i \in Cand : ~r1[i].seen \/ r1[i].owes \/ conn[i].high # last(i) }
           \* the skipped connected slots
           Skipped == { i \in Slots : conn[i].on /\ i \notin Cand }
           \* a skipped slot whose skip mark names an address it has left,
           \* that forget refused: not marked, so the skip mark keeps its
           \* owner (KeepSkipOwner)
           Blocked == IF KeepSkipOwner THEN { i \in Skipped : r1[i].skip /\ r1[i].skipAt # conn[i].at } ELSE {}
           \* the other skipped slots, marked at their own address without
           \* taking the record (MarkWhileSkipped)
           SkipWants == IF MarkWhileSkipped THEN Skipped \ Blocked ELSE {}
           \* the marks the sink may take: a zero mark is refused in a walk
           \* that refused a forget (ZeroMarkWaitsForForgets); a skipped
           \* slot's mark is always zero
           takeable(i) == ~(ZeroMarkWaitsForForgets /\ refusedAny /\ conn[i].high = 0)
           skipTakeable == ~(ZeroMarkWaitsForForgets /\ refusedAny) \/ ~KeptKeyZeroMarkWaits
       IN \E mAcc \in SUBSET { i \in Wants : takeable(i) }, sAcc \in SUBSET { i \in SkipWants : skipTakeable } :
          LET \* the addresses another slot than k marks now
              marked(k) == { conn[i].at : i \in (mAcc \cup sAcc) \ {k} }
              \* the other slots whose record or skip mark names an address
              \* marked now: each owes a forget of it, which the queued mark
              \* supersedes; the code scans for them only when a forget of
              \* this walk was refused
              Sup == { k \in Slots : MarkSupersedes /\ refusedAny /\ k \notin Cand /\ r1[k].seen /\ r1[k].at \in marked(k) }
              SkipSup == { k \in Slots : MarkSupersedes /\ refusedAny /\ r1[k].skip /\ r1[k].skipAt \in marked(k) }
              \* the greetings the publish after this commit holds: a slot
              \* not yet greeted whose mark the sink refused (its mark is
              \* the zero of a session that has sent nothing; the code holds
              \* every keep-alive then, rocketnet_xdp_replay_sink_mark). A
              \* Blocked slot asks for no mark and is not held: with
              \* ZeroMarkWaitsForForgets no skip mark is ever taken, so no
              \* slot is Blocked (its witness shows the greeting it costs)
              Held == { i \in (Wants \ mAcc) \cup (SkipWants \ sAcc) : ~greeted[i] }
              q1 == [ i \in Slots |-> IF i \in Sup THEN ClearRec(r1[i]) ELSE r1[i] ]
              q2 == [ i \in Slots |-> IF i \in SkipSup THEN ClearSkip(q1[i]) ELSE q1[i] ]
              r2 == [ i \in Slots |->
                        IF i \notin Cand
                        THEN IF i \in sAcc /\ RecordSkip
                             THEN [ q2[i] EXCEPT !.skip = TRUE, !.skipAt = conn[i].at ]
                             ELSE q2[i]
                        ELSE IF i \notin Wants THEN SeenAt(q2[i], conn[i].at)
                        ELSE IF i \in mAcc
                             THEN [ SeenAt(q2[i], conn[i].at) EXCEPT !.has = TRUE, !.marked = conn[i].high, !.owes = FALSE ]
                             ELSE [ SeenAt(q2[i], conn[i].at) EXCEPT !.owes = TRUE ] ]
              \* a skipped slot's mark is asked at zero (the walk's
              \* sink->mark( address, 0 ) in the skip)
              marks == { <<conn[i].at, conn[i].high>> : i \in mAcc } \cup { <<conn[i].at, 0>> : i \in sAcc }
          IN /\ rec' = r2
             /\ kernel' = [ a \in Addrs |->
                              IF \E m \in marks : m[1] = a THEN (CHOOSE m \in marks : m[1] = a)[2]
                              ELSE IF a \in forgets THEN NONE
                              ELSE kernel[a] ]
             \* the ghost: every address a slot was marked or seen at is owed
             \* its forget, the skipped slots' marks included
             /\ owed' = ((((owed \ { <<i, rec[i].at>> : i \in fAcc \cup FSup })
                                 \ { <<i, rec[i].skipAt>> : i \in gAcc \cup GSup })
                                 \ { <<k, r1[k].at>> : k \in Sup })
                                 \ { <<k, r1[k].skipAt>> : k \in SkipSup })
                          \cup { <<i, conn[i].at>> : i \in Cand \cup sAcc }
             /\ supOwed' = (OweOnRefusal /\ refusedAny)
             /\ greeted' = [ i \in Slots |-> conn[i].on /\ (greeted[i] \/ i \notin Held) ]
    /\ UNCHANGED conn

Next ==
    \/ \E i \in Slots, a \in Addrs : Connect(i, a)
    \/ \E i \in Slots : Open(i) \/ Release(i)
    \/ Walk

Spec == Init /\ [][Next]_vars

----------------------------------------------------------------------------

TypeOK ==
    /\ conn \in [ Slots -> [ on : BOOLEAN, at : Addrs \cup {NONE}, high : Highs ] ]
    /\ rec \in [ Slots -> [ seen : BOOLEAN, at : Addrs \cup {NONE}, has : BOOLEAN, marked : Highs, owes : BOOLEAN,
                            skip : BOOLEAN, skipAt : Addrs \cup {NONE} ] ]
    /\ kernel \in [ Addrs -> Highs \cup {NONE} ]
    /\ supOwed \in BOOLEAN
    /\ greeted \in [ Slots -> BOOLEAN ]

\* a greeted connected slot's address has an entry in the kernel map: its
\* client sends session packets, and the kernel drops a session packet from
\* a source with no entry (RNXdpReplay.tla KernelFilter, DROPPED_NO_SESSION).
\* The one exception is a slot whose own record owes its mark: a forget of
\* the address another slot's stale record asked for deleted the entry and
\* the sink refused the re-mark the same walk asked for (the mark buffer or
\* the kernel map full, an error path). Its session packets drop as
\* no-session until the owed mark lands, at the next walk the sink takes
\* it; before the kernel dropped for no entry they passed unchecked.
GreetedSlotHasEntry ==
    \A i \in Slots : (conn[i].on /\ greeted[i]) =>
                        \/ kernel[conn[i].at] # NONE
                        \/ (rec[i].seen /\ rec[i].at = conn[i].at /\ rec[i].owes)

\* what the forget pass's skipped scan rests on: unless the last walk left a
\* refused forget, no two slots' records or skip marks name one address, so
\* a queued forget has no other record or skip mark to clear
Named(i) == (IF rec[i].seen THEN {rec[i].at} ELSE {}) \cup (IF rec[i].skip THEN {rec[i].skipAt} ELSE {})

DistinctUnlessOwed ==
    ~supOwed => \A i, k \in Slots : i # k => Named(i) \cap Named(k) = {}

\* a forget owed for an address a slot was seen or marked at is never lost:
\* the slot's record or skip mark keeps naming that address until the sink
\* accepts it
ForgetNeverLost ==
    \A p \in owed : \/ rec[p[1]].seen /\ rec[p[1]].at = p[2]
                    \/ rec[p[1]].skip /\ rec[p[1]].skipAt = p[2]

\* a slot answers for two addresses at most: its record's and its skip
\* mark's
OneIdentityPerSlot ==
    \A p, q, r \in owed : (p[1] = q[1] /\ q[1] = r[1]) => (p[2] = q[2] \/ q[2] = r[2] \/ p[2] = r[2])

\* every kernel entry has an owner: a slot whose record or skip mark names
\* its address, whose forget deletes it once the slot leaves.  An entry
\* without one stays in the map until the transport is recreated
KernelEntryOwned ==
    \A a \in Addrs : kernel[a] # NONE =>
        \E i \in Slots : \/ rec[i].seen /\ rec[i].at = a
                         \/ rec[i].skip /\ rec[i].skipAt = a

\* a connected slot whose record holds its current mark, owing nothing, has
\* that mark in the kernel map.  Without MarkSupersedes it fails when the
\* sink refuses a forget: the slot keeps the old address's forget owed, the
\* address connects in another slot and is marked, and the retried forget,
\* applied in a later commit, deletes the newer slot's entry
\* (README-RNServerReplaySlots.md)
KernelHoldsMark ==
    \A i \in Slots :
        (conn[i].on /\ rec[i].seen /\ rec[i].at = conn[i].at /\ rec[i].has /\ ~rec[i].owes)
            => kernel[conn[i].at] = rec[i].marked

=============================================================================
