----------------------- MODULE RNSourceSnapshotLayout -----------------------
(***************************************************************************)
(* The connected-source snapshot (sdk/rocketnet_source_snapshot.h) at its  *)
(* layout: two slots of Cap cells, each an open-addressing hash set of     *)
(* addresses with a linear probe that wraps at the slot's end; one writer  *)
(* (main) and the readers (the receive workers).  The published word is    *)
(* pub, a slot and a generation; each reader announces a slot in its own   *)
(* word ann[r].                                                            *)
(*                                                                         *)
(* The writer, one step per code call: Scan(skip) is                       *)
(* rocketnet_source_snapshot_writer_scan for the next reader, in reader    *)
(* order, with skip the bool it returns: a scan that finds the reader on   *)
(* the target ends the update with no publish (begin returns NONE and the  *)
(* caller counts it, skipped).  Fill is                                    *)
(* rocketnet_source_snapshot_writer_fill: the target cleared.  Add(a) and  *)
(* Refuse(a) are rocketnet_source_snapshot_writer_add, which inserts a or  *)
(* refuses it when the target holds MaxEntries already.  Publish is        *)
(* rocketnet_source_snapshot_writer_publish: one store of the next         *)
(* generation's word; the other slot becomes the target.                   *)
(*                                                                         *)
(* A reader, one step per code call: Load(r, w)                            *)
(* (rocketnet_source_snapshot_reader_load, w the word it returns),         *)
(* Announce (_reader_announce), Recheck(r, ok) (_reader_recheck, ok its    *)
(* result: the published word is still the one loaded, or the             *)
(* announcement is withdrawn and the reader starts again),                 *)
(* Lookup(r, a, found) (rocketnet_source_snapshot_contains, found its      *)
(* result, as many as the batch makes) and Release (_reader_release).      *)
(* What a call returns is an argument of its action, so the trace check    *)
(* (TraceRNSourceSnapshotLayout) checks it.                                *)
(*                                                                         *)
(* Each step is atomic.  A step of the code is several memory operations, *)
(* but only the published word and the announcements are shared; the      *)
(* cells a step writes are read by nobody until a later step, which is     *)
(* what NoTornRead checks, and the atomics are sequentially consistent     *)
(* where the argument needs one total order (the header's comment).       *)
(*                                                                         *)
(* Ghost state: wset, the addresses the writer added to the target this   *)
(* update; pset, the set the published word stands for (wset at its        *)
(* publish); held[r], the set published at reader r's load, which its      *)
(* lookups must answer while it holds the slot.                            *)
(*                                                                         *)
(* Reversed witnesses:                                                     *)
(*   HalfPublished  the publish store before the last add: the writer may  *)
(*                  publish and then make one more add, into the slot now  *)
(*                  published (the trace check's plant);                   *)
(*   NoRecheck      Announce takes the slot with no re-check;              *)
(*   LookupNoWrap   the lookup's probe stops at the slot's end instead of  *)
(*                  wrapping to cell 0.                                    *)
(***************************************************************************)
EXTENDS Integers, FiniteSets

CONSTANTS
    Readers,        \* reader indices, 1..N (the code's reader r is r - 1)
    Addrs,          \* addresses, 1..A; cell value 0 is an empty cell (ROCKETNET_ADDRESS_NONE)
    Cap,            \* cells per slot: rocketnet_source_snapshot_capacity( MaxEntries )
    MaxEntries,     \* the snapshot's max_entries
    HomeAt,         \* the home cells: a * Cap + h for address a homed on cell h
    MaxUpdates,     \* exploration bound: updates begun (publishes plus skips)
    MaxRefused,     \* exploration bound: refused adds
    HalfPublished,  \* defect: publish before the last add
    NoRecheck,      \* defect: announce with no re-check
    LookupNoWrap    \* defect: the lookup does not wrap at the slot's end

ASSUME Cap >= 2 * MaxEntries /\ MaxEntries >= 1
\* every address has exactly one home cell in HomeAt (a set of integers,
\* which a cfg can carry, where a function cannot)
ASSUME 0 \notin Addrs /\ \A a \in Addrs : Cardinality({h \in 0..(Cap - 1) : a * Cap + h \in HomeAt}) = 1

\* an address's home cell: rocketnet_source_snapshot_home
Home(a) == CHOOSE h \in 0..(Cap - 1) : a * Cap + h \in HomeAt
ASSUME HalfPublished \in BOOLEAN /\ NoRecheck \in BOOLEAN /\ LookupNoWrap \in BOOLEAN

Slots == {0, 1}
Cells == 0..(Cap - 1)
None == -1
NoWord == [slot |-> None, gen |-> None]
NoLook == [addr |-> 0, found |-> FALSE]

VARIABLES
    slots,      \* slots[s][i]: cell i of slot s, an address or 0 (snapshot->cells)
    pub,        \* the published word: [slot, gen] (published->word, ( gen << 1 ) | slot)
    ann,        \* ann[r]: reader r's announced slot or None (reader[r - 1].slot)
    wpc,        \* the writer: "idle", "scan", "add", or "late" (HalfPublished only)
    wi,         \* readers scanned this update (writer_begin's loop index)
    wslot,      \* the writer's target (writer->target)
    wset,       \* ghost: addresses added to the target this update
    pset,       \* ghost: the set the published word stands for
    skipped,    \* updates whose publish was skipped (counted by the caller)
    refused,    \* adds refused full (counted by the caller)
    rpc,        \* rpc[r]: "idle", "loaded", "announced", "held"
    rl,         \* rl[r]: the word reader r loaded, or NoWord
    held,       \* ghost: held[r], the set published at reader r's load
    look        \* look[r]: reader r's last lookup, [addr, found], or NoLook

vars == <<slots, pub, ann, wpc, wi, wslot, wset, pset, skipped, refused, rpc, rl, held, look>>

\* ---- the slot arithmetic, the code's own ----

Probe(h, i) == (h + i) % Cap

\* rocketnet_source_snapshot_contains: probe from home, wrapping, stopping at
\* the address or an empty cell.  LookupNoWrap stops at the slot's end.
Contains(t, a) ==
    IF LookupNoWrap
    THEN \E i \in 0..(Cap - 1 - Home(a)) :
            /\ t[Home(a) + i] = a
            /\ \A k \in 0..(i - 1) : t[Home(a) + k] # 0
    ELSE \E i \in Cells :
            /\ t[Probe(Home(a), i)] = a
            /\ \A k \in 0..(i - 1) : t[Probe(Home(a), k)] # 0

\* rocketnet_source_snapshot_writer_add's cell for a new address: the first
\* empty cell on its probe path (the path holds no copy of a: Add is taken
\* only for an address not yet added)
FreeCell(t, a) ==
    Probe(Home(a), CHOOSE i \in Cells :
                        /\ t[Probe(Home(a), i)] = 0
                        /\ \A k \in 0..(i - 1) : t[Probe(Home(a), k)] # 0)

Empty == [i \in Cells |-> 0]

\* the slot the writer is writing, if it is writing one
Writing == wpc \in {"add", "late"}
WSlot == IF wpc = "late" THEN pub.slot ELSE wslot

Init ==
    /\ slots = [s \in Slots |-> Empty]
    /\ pub = [slot |-> 0, gen |-> 0]
    /\ ann = [r \in Readers |-> None]
    /\ wpc = "idle"
    /\ wi = 0
    /\ wslot = 1
    /\ wset = {}
    /\ pset = {}
    /\ skipped = 0
    /\ refused = 0
    /\ rpc = [r \in Readers |-> "idle"]
    /\ rl = [r \in Readers |-> NoWord]
    /\ held = [r \in Readers |-> {}]
    /\ look = [r \in Readers |-> NoLook]

ReaderVars == <<rpc, rl, held, look>>

\* ---- the writer ----

\* writer_scan of reader wi + 1 (readers in order), which returns skip:
\* on the target the update ends, skipped; otherwise the next reader
Scan(skip) ==
    /\ \/ wpc = "idle" /\ pub.gen + skipped < MaxUpdates
       \/ wpc = "scan"
    /\ wi < Cardinality(Readers)
    /\ skip = (ann[wi + 1] = wslot)
    /\ IF skip
       THEN /\ skipped' = skipped + 1
            /\ wpc' = "idle"
            /\ wi' = 0
       ELSE /\ wpc' = "scan"
            /\ wi' = wi + 1
            /\ UNCHANGED skipped
    /\ UNCHANGED <<slots, pub, ann, wslot, wset, pset, refused>>
    /\ UNCHANGED ReaderVars

\* writer_fill: every reader scanned clean; the target is cleared
Fill ==
    /\ wpc = "scan"
    /\ wi = Cardinality(Readers)
    /\ slots' = [slots EXCEPT ![wslot] = Empty]
    /\ wpc' = "add"
    /\ wi' = 0
    /\ wset' = {}
    /\ UNCHANGED <<pub, ann, wslot, pset, skipped, refused>>
    /\ UNCHANGED ReaderVars

\* writer_add of an address not yet added, with room
Add(a) ==
    /\ wpc \in {"add", "late"}
    /\ a \notin wset
    /\ Cardinality(wset) < MaxEntries
    /\ slots' = [slots EXCEPT ![WSlot] = [@ EXCEPT ![FreeCell(slots[WSlot], a)] = a]]
    /\ wset' = wset \cup {a}
    /\ wpc' = IF wpc = "late" THEN "idle" ELSE "add"
    /\ UNCHANGED <<pub, ann, wi, wslot, pset, skipped, refused>>
    /\ UNCHANGED ReaderVars

\* writer_add of a new address to a full target: refused, counted
Refuse(a) ==
    /\ wpc = "add"
    /\ a \notin wset
    /\ Cardinality(wset) = MaxEntries
    /\ refused < MaxRefused
    /\ refused' = refused + 1
    /\ UNCHANGED <<slots, pub, ann, wpc, wi, wslot, wset, pset, skipped>>
    /\ UNCHANGED ReaderVars

\* writer_publish: one store of the next word; the other slot is the target
Publish ==
    /\ wpc = "add"
    /\ pub' = [slot |-> wslot, gen |-> pub.gen + 1]
    /\ pset' = wset
    /\ wslot' = 1 - wslot
    /\ wpc' = "idle"
    /\ UNCHANGED <<slots, ann, wi, wset, skipped, refused>>
    /\ UNCHANGED ReaderVars

\* HalfPublished: the publish store before the last add; the add after it
\* goes to the slot just published (WSlot in "late")
EarlyPublish ==
    /\ HalfPublished
    /\ wpc = "add"
    /\ pub' = [slot |-> wslot, gen |-> pub.gen + 1]
    /\ pset' = wset
    /\ wslot' = 1 - wslot
    /\ wpc' = "late"
    /\ UNCHANGED <<slots, ann, wi, wset, skipped, refused>>
    /\ UNCHANGED ReaderVars

\* ---- a reader ----

WriterVars == <<slots, pub, wpc, wi, wslot, wset, pset, skipped, refused>>

\* reader_load returns w, the published word
Load(r, w) ==
    /\ rpc[r] = "idle"
    /\ w = pub
    /\ rl' = [rl EXCEPT ![r] = w]
    /\ held' = [held EXCEPT ![r] = pset]
    /\ rpc' = [rpc EXCEPT ![r] = "loaded"]
    /\ UNCHANGED <<ann, look>>
    /\ UNCHANGED WriterVars

Announce(r) ==
    /\ rpc[r] = "loaded"
    /\ ann' = [ann EXCEPT ![r] = rl[r].slot]
    /\ rpc' = [rpc EXCEPT ![r] = IF NoRecheck THEN "held" ELSE "announced"]
    /\ UNCHANGED <<rl, held, look>>
    /\ UNCHANGED WriterVars

\* reader_recheck returns ok: the published word is still the one loaded
Recheck(r, ok) ==
    /\ rpc[r] = "announced"
    /\ ok = (pub = rl[r])
    /\ IF ok
       THEN /\ rpc' = [rpc EXCEPT ![r] = "held"]
            /\ UNCHANGED <<ann, rl, held>>
       ELSE /\ rpc' = [rpc EXCEPT ![r] = "idle"]
            /\ ann' = [ann EXCEPT ![r] = None]
            /\ rl' = [rl EXCEPT ![r] = NoWord]
            /\ held' = [held EXCEPT ![r] = {}]
    /\ UNCHANGED look
    /\ UNCHANGED WriterVars

\* contains returns found
Lookup(r, a, found) ==
    /\ rpc[r] = "held"
    /\ found = Contains(slots[ann[r]], a)
    /\ look' = [look EXCEPT ![r] = [addr |-> a, found |-> found]]
    /\ UNCHANGED <<ann, rpc, rl, held>>
    /\ UNCHANGED WriterVars

Release(r) ==
    /\ rpc[r] = "held"
    /\ ann' = [ann EXCEPT ![r] = None]
    /\ rpc' = [rpc EXCEPT ![r] = "idle"]
    /\ rl' = [rl EXCEPT ![r] = NoWord]
    /\ held' = [held EXCEPT ![r] = {}]
    /\ look' = [look EXCEPT ![r] = NoLook]
    /\ UNCHANGED WriterVars

Next ==
    \/ \E skip \in BOOLEAN : Scan(skip)
    \/ Fill \/ Publish \/ EarlyPublish
    \/ \E a \in Addrs : Add(a) \/ Refuse(a)
    \/ \E r \in Readers :
        \/ Load(r, pub) \/ Announce(r) \/ Release(r)
        \/ \E ok \in BOOLEAN : Recheck(r, ok)
        \/ \E a \in Addrs, found \in BOOLEAN : Lookup(r, a, found)

Spec == Init /\ [][Next]_vars

\* ---- invariants ----

Words == [slot : Slots, gen : Nat] \cup {NoWord}

TypeOK ==
    /\ slots \in [Slots -> [Cells -> Addrs \cup {0}]]
    /\ pub \in [slot : Slots, gen : Nat]
    /\ ann \in [Readers -> Slots \cup {None}]
    /\ wpc \in {"idle", "scan", "add", "late"}
    /\ wi \in 0..Cardinality(Readers)
    /\ wslot \in Slots
    /\ wset \subseteq Addrs /\ pset \subseteq Addrs
    /\ skipped \in Nat /\ refused \in Nat
    /\ rpc \in [Readers -> {"idle", "loaded", "announced", "held"}]
    /\ rl \in [Readers -> Words]
    /\ held \in [Readers -> SUBSET Addrs]
    /\ look \in [Readers -> [addr : Addrs \cup {0}, found : BOOLEAN]]

\* the writer's target is never the published slot (but in HalfPublished)
TargetUnpublished == wpc # "late" => wslot # pub.slot

\* a reader holding a slot never holds the one the writer is writing
NoTornRead ==
    \A r \in Readers : rpc[r] = "held" => ~(Writing /\ WSlot = ann[r])

\* the writer never writes a slot a reader announced, but under an
\* announcement its re-check will refuse: one on a slot not published,
\* made after the writer's scan of that reader
WriterNeverWritesAnnounced ==
    Writing =>
        \A r \in Readers :
            ann[r] = WSlot => (rpc[r] = "announced" /\ pub.slot # ann[r])

\* a held slot answers every lookup with membership in the set published at
\* the reader's load, and the last lookup made said so
LookupMatchesPublished ==
    \A r \in Readers :
        rpc[r] = "held" =>
            /\ \A a \in Addrs : Contains(slots[ann[r]], a) <=> (a \in held[r])
            /\ look[r] # NoLook => (look[r].found <=> (look[r].addr \in held[r]))

\* the published slot holds exactly the published set (while nobody writes it)
PublishedExact ==
    ~(Writing /\ WSlot = pub.slot) =>
        {slots[pub.slot][i] : i \in Cells} \ {0} = pset

\* every stored address is reachable from its home with no empty cell
\* between, and appears once: the probe chain the lookup walks
ChainIntact ==
    \A s \in Slots : \A j \in Cells :
        LET a == slots[s][j] IN
        a # 0 =>
            /\ \E i \in Cells :
                /\ Probe(Home(a), i) = j
                /\ \A k \in 0..(i - 1) : slots[s][Probe(Home(a), k)] # 0
            /\ \A j2 \in Cells : slots[s][j2] = a => j2 = j

\* a slot is at most half full
AtMostHalf ==
    \A s \in Slots : Cardinality({i \in Cells : slots[s][i] # 0}) <= MaxEntries

=============================================================================
