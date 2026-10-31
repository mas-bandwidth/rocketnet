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
(*                    the record (the memset)                              *)
(*   the mark pass    every connected slot: a slot whose record still      *)
(*                    names another address is skipped (its forget is      *)
(*                    owed first); otherwise the record is seen at the     *)
(*                    address, and a slot first seen, owing a mark or      *)
(*                    whose highest_opened differs from its last mark asks *)
(*                    the sink to mark it; a refused mark is owed          *)
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
(* accepted.                                                               *)
(*                                                                         *)
(* Reversed witnesses (each flag FALSE is a real bug class):               *)
(*   SkipWhileOwed  the mark pass does not skip a slot whose record names  *)
(*                  another address, and overwrites the identity whose     *)
(*                  forget is owed                                         *)
(*   ClearOnAccept  the forget pass clears the record although the sink    *)
(*                  refused the forget                                     *)
(***************************************************************************)
EXTENDS Integers, FiniteSets

CONSTANTS
    Slots,          \* client slots
    Addrs,          \* client addresses
    MaxHigh,        \* highest_opened runs 0..MaxHigh
    SkipWhileOwed,  \* the mark pass skips a slot seen at another address
    ClearOnAccept,  \* the forget pass clears a record only on an accepted forget
    ForgetRefusable,\* the sink may refuse a forget (its array full)
    NONE            \* no kernel entry, no address (a model value)

ASSUME MaxHigh \in Nat
ASSUME \A f \in {SkipWhileOwed, ClearOnAccept, ForgetRefusable} : f \in BOOLEAN

Highs == 0..MaxHigh
Clear == [ seen |-> FALSE, at |-> NONE, has |-> FALSE, marked |-> 0, owes |-> FALSE ]

VARIABLES
    conn,           \* conn[i]: the slot's connection, [on, at, high]
    rec,            \* rec[i]: the slot's record, [seen, at (seen_address), has (has_mark), marked (marked_value), owes (mark_owed)]
    kernel,         \* kernel[a]: the replay map's entry for a, NONE for none
    owed            \* ghost: the <<slot, address>> pairs whose forget is owed

vars == <<conn, rec, kernel, owed>>

Init ==
    /\ conn = [ i \in Slots |-> [ on |-> FALSE, at |-> NONE, high |-> 0 ] ]
    /\ rec = [ i \in Slots |-> Clear ]
    /\ kernel = [ a \in Addrs |-> NONE ]
    /\ owed = {}

\* a client connects in a free slot, at an address no connected slot holds,
\* its session's highest_opened at zero
Connect(i, a) ==
    /\ ~conn[i].on
    /\ \A j \in Slots : conn[j].on => conn[j].at # a
    /\ conn' = [ conn EXCEPT ![i] = [ on |-> TRUE, at |-> a, high |-> 0 ] ]
    /\ UNCHANGED <<rec, kernel, owed>>

\* a genuine session packet raises highest_opened
Open(i) ==
    /\ conn[i].on
    /\ conn[i].high < MaxHigh
    /\ conn' = [ conn EXCEPT ![i].high = @ + 1 ]
    /\ UNCHANGED <<rec, kernel, owed>>

\* the slot is released
Release(i) ==
    /\ conn[i].on
    /\ conn' = [ conn EXCEPT ![i] = [ on |-> FALSE, at |-> NONE, high |-> 0 ] ]
    /\ UNCHANGED <<rec, kernel, owed>>

\* rocketnet_server_replay_marks_walk, then the transport's commit
Walk ==
    LET F == { i \in Slots : rec[i].seen /\ (~conn[i].on \/ conn[i].at # rec[i].at) }
    IN \E fAcc \in IF ForgetRefusable THEN SUBSET F ELSE {F} :
       LET r1 == [ i \in Slots |->
                     IF i \in fAcc \/ (i \in F /\ ~ClearOnAccept) THEN Clear ELSE rec[i] ]
           forgets == { rec[i].at : i \in fAcc }
           \* the mark pass's candidates: connected, not skipped
           Cand == { i \in Slots : conn[i].on /\ ~(SkipWhileOwed /\ r1[i].seen /\ r1[i].at # conn[i].at) }
           last(i) == IF r1[i].has THEN r1[i].marked ELSE 0
           Wants == { i \in Cand : ~r1[i].seen \/ r1[i].owes \/ conn[i].high # last(i) }
       IN \E mAcc \in SUBSET Wants :
          LET r2 == [ i \in Slots |->
                        IF i \notin Cand THEN r1[i]
                        ELSE IF i \notin Wants THEN [ r1[i] EXCEPT !.seen = TRUE, !.at = conn[i].at ]
                        ELSE IF i \in mAcc
                             THEN [ seen |-> TRUE, at |-> conn[i].at, has |-> TRUE,
                                    marked |-> conn[i].high, owes |-> FALSE ]
                             ELSE [ r1[i] EXCEPT !.seen = TRUE, !.at = conn[i].at, !.owes = TRUE ] ]
              marks == { <<conn[i].at, conn[i].high>> : i \in mAcc }
          IN /\ rec' = r2
             /\ kernel' = [ a \in Addrs |->
                              IF \E m \in marks : m[1] = a THEN (CHOOSE m \in marks : m[1] = a)[2]
                              ELSE IF a \in forgets THEN NONE
                              ELSE kernel[a] ]
             /\ owed' = (owed \ { <<i, rec[i].at>> : i \in fAcc })
                          \cup { <<i, conn[i].at>> : i \in Cand }
    /\ UNCHANGED conn

Next ==
    \/ \E i \in Slots, a \in Addrs : Connect(i, a)
    \/ \E i \in Slots : Open(i) \/ Release(i)
    \/ Walk

Spec == Init /\ [][Next]_vars

----------------------------------------------------------------------------

TypeOK ==
    /\ conn \in [ Slots -> [ on : BOOLEAN, at : Addrs \cup {NONE}, high : Highs ] ]
    /\ rec \in [ Slots -> [ seen : BOOLEAN, at : Addrs \cup {NONE}, has : BOOLEAN, marked : Highs, owes : BOOLEAN ] ]
    /\ kernel \in [ Addrs -> Highs \cup {NONE} ]

\* a forget owed for an address a slot was seen at is never lost: the
\* slot's record keeps naming that address until the sink accepts it
ForgetNeverLost ==
    \A p \in owed : rec[p[1]].seen /\ rec[p[1]].at = p[2]

\* a slot's record names one address at a time
OneIdentityPerSlot ==
    \A p, q \in owed : p[1] = q[1] => p[2] = q[2]

\* a connected slot whose record holds its current mark, owing nothing, has
\* that mark in the kernel map.  It fails when the sink refuses a forget:
\* the slot keeps the old address's forget owed, the address connects in
\* another slot and is marked, and the retried forget, applied in a later
\* commit, deletes the newer slot's entry (README-RNServerReplaySlots.md,
\* the finding)
KernelHoldsMark ==
    \A i \in Slots :
        (conn[i].on /\ rec[i].seen /\ rec[i].at = conn[i].at /\ rec[i].has /\ ~rec[i].owes)
            => kernel[conn[i].at] = rec[i].marked

=============================================================================
