---------------------------- MODULE RNClientRace ----------------------------
(* The client family race's ownership: which thread touches, closes and frees
   each family's socket (sdk/rocketnet_client_transport.cpp,
   rocketnet_client_race_t; standard/TRANSPORT.md t11 rule 3).

   A token naming both families makes the client open one socket per family.
   The caller's thread sends to both until it commits (the first cookie, in
   update), then to the kept family only. The reading thread (the client I/O
   thread, the legacy receive worker, or the caller's own step) reads both
   until it observes the commit with acquire, then closes the losing family's
   operating-system handles (the backend's close: socket, wake, completion
   port) and reads the kept family only. Nothing is freed then: destroy, on
   the caller's thread, after stop and join, frees both families' memory and
   the race block.

   A family's state is its handle (open or closed) and its memory (live or
   freed). Every use of a family by either thread is checked against both:
   a send, receive or wait on a closed handle or freed memory is a fault.
   Every free records the thread that made it.

   Defect, a set of defect names, empty in the passing case, selects the
   reversed witnesses:
     "readerfrees"    the reading thread frees the losing family at the
                      close (the code before this model): FreedOnlyByCaller
     "callerusesloser" the caller sends to the losing family after its own
                      commit: NoUseAfterClose
     "freebeforejoin" destroy frees before the reading thread is joined:
                      FreedOnlyAfterJoin
     "readerclosesearly" the reading thread closes a family before the
                      commit: NoUseAfterClose
     "commitkeepsring" the commit leaves a Filling ring a deferred hand-off
                      bound to the losing server bound there: a slot the
                      attempt commits after the commit leaves for the losing
                      family and the race's send drops it: PostCommitSlotsKept
     "nokeepatcommit" the commit does not keep the losing family's send
                      failures; only the reader's close stores them, so a
                      SEND_FAILED read between the two leaves them out:
                      SendFailedMonotonic

   The Filling ring of the I/O ring path (rocketnet_client_io_hand_off): a
   hand-off attempt binds the ring's destination to the protocol's server, the
   primary family's before the commit and the kept family's after it, and a
   LATE attempt leaves it bound for the next. Only whether the ring holds a
   slot committed after the commit is tracked: a slot committed before it was
   for both families while both were in flight.

   Send failures: each family's count goes up while its handle is open, at
   most MaxFail. SEND_FAILED is read on the caller's thread as the race's
   send_failures computes it.

   The model is finite: the caller may send, fill and hand off, commit, read
   SEND_FAILED, stop, join and destroy; the reader may step any number of
   times until it exits. It does not model the bytes, the protocol, the
   cookie or C++ ordering beyond the one release store and acquire load of
   the commit. *)
EXTENDS Integers, FiniteSets
CONSTANTS Defect
ASSUME Defect \subseteq {"readerfrees", "callerusesloser", "freebeforejoin", "readerclosesearly",
                         "commitkeepsring", "nokeepatcommit"}

MaxFail == 2

Families == {0, 1}
Threads == {"caller", "reader"}
VARIABLES committed,  \* -1 while both are in flight, else the kept family (caller writes, release)
          seen,       \* the reader's acquire load of committed
          closed,     \* the reader closed the losing family (race->closed)
          handle,     \* [Families -> {"open", "closed"}]
          memory,     \* [Families -> {"live", "freed"}]
          frees,      \* the set of <<thread, family>> frees made
          reader,     \* "running", "stopping", "exited"
          joined,     \* the caller joined the reading thread
          destroyed,  \* the caller destroyed the transport
          fault,      \* a use of a closed handle or freed memory: "none" or the thread
          ringTo,     \* the Filling ring's bound destination family, or -1 (io->to_held clear)
          ringPost,   \* the Filling ring holds a slot committed after the commit
          lost,       \* a slot committed after the commit left for the losing family and was dropped
          failures,   \* [Families -> 0..MaxFail]: each family's send failures
          kept,       \* race->closed_send_failures
          reported,   \* the last SEND_FAILED the caller read
          dipped      \* a SEND_FAILED read was below the one before it
vars == <<committed, seen, closed, handle, memory, frees, reader, joined, destroyed, fault,
          ringTo, ringPost, lost, failures, kept, reported, dipped>>
ringVars == <<ringTo, ringPost, lost>>
failVars == <<failures, kept, reported, dipped>>

Loser == 1 - committed
\* the protocol's server: the primary family's until the commit, then the kept family's
Server == IF committed >= 0 THEN committed ELSE 0
\* rocketnet_client_race_send_failures
SendFailed == IF committed >= 0 THEN failures[committed] + kept ELSE failures[0] + failures[1]
Usable(f) == handle[f] = "open" /\ memory[f] = "live"
Use(t, fs) == fault' = IF \A f \in fs : Usable(f) THEN fault ELSE t

Init ==
    /\ committed = -1 /\ seen = -1 /\ closed = FALSE
    /\ handle = [f \in Families |-> "open"]
    /\ memory = [f \in Families |-> "live"]
    /\ frees = {}
    /\ reader = "running" /\ joined = FALSE /\ destroyed = FALSE
    /\ fault = "none"
    /\ ringTo = -1 /\ ringPost = FALSE /\ lost = FALSE
    /\ failures = [f \in Families |-> 0] /\ kept = 0 /\ reported = 0 /\ dipped = FALSE

(* rocketnet_client_race_send / send_batch, the caller: both families while
   in flight, the kept one after the commit. *)
CallerSend ==
    /\ ~destroyed
    /\ IF committed < 0 THEN Use("caller", Families)
       ELSE IF "callerusesloser" \in Defect THEN Use("caller", Families)
       ELSE Use("caller", {committed})
    /\ UNCHANGED <<committed, seen, closed, handle, memory, frees, reader, joined, destroyed>>
    /\ UNCHANGED <<ringVars, failVars>>

(* rocketnet_client_transport_io_hand_off, then rocketnet_client_io_hand_off
   returning LATE: a slot is committed into the Filling ring, the first
   attempt binds its destination to the protocol's server, and the ring stays
   with the caller. *)
CallerHandOffLate ==
    /\ ~destroyed
    /\ ringTo' = IF ringTo < 0 THEN Server ELSE ringTo
    /\ ringPost' = (ringPost \/ committed >= 0)
    /\ UNCHANGED <<committed, seen, closed, handle, memory, frees, reader, joined, destroyed, fault>>
    /\ UNCHANGED <<lost, failVars>>

(* the same hand-off returning HANDED_OFF: the ring leaves for its bound
   destination through rocketnet_client_race_send_batch, which after the
   commit sends only what is addressed to the kept family. A slot committed
   after the commit is in it when this attempt commits one after the commit
   or an earlier LATE did. *)
CallerHandOff ==
    /\ ~destroyed
    /\ LET to   == IF ringTo < 0 THEN Server ELSE ringTo
           post == ringPost \/ committed >= 0
       IN lost' = (lost \/ (post /\ to # committed))
    /\ ringTo' = -1 /\ ringPost' = FALSE
    /\ UNCHANGED <<committed, seen, closed, handle, memory, frees, reader, joined, destroyed, fault>>
    /\ UNCHANGED failVars

(* rocketnet_client_race_commit, the caller in update: rebinds a Filling ring
   bound to the losing server (rocketnet_client_io_rebind_held) and keeps the
   losing family's send failures, then the release store. *)
CallerCommit(f) ==
    /\ ~destroyed /\ committed < 0
    /\ committed' = f
    /\ ringTo' = IF ringTo = 1 - f /\ "commitkeepsring" \notin Defect THEN f ELSE ringTo
    /\ kept' = IF "nokeepatcommit" \in Defect THEN kept ELSE failures[1 - f]
    /\ UNCHANGED <<seen, closed, handle, memory, frees, reader, joined, destroyed, fault>>
    /\ UNCHANGED <<ringPost, lost, failures, reported, dipped>>

(* a send on an open family fails: the I/O thread's or the caller's, before
   the commit or, on the losing family, between the commit and the close *)
SendFails(f) ==
    /\ handle[f] = "open" /\ memory[f] = "live" /\ failures[f] < MaxFail
    /\ failures' = [failures EXCEPT ![f] = @ + 1]
    /\ UNCHANGED <<committed, seen, closed, handle, memory, frees, reader, joined, destroyed, fault>>
    /\ UNCHANGED <<ringVars, kept, reported, dipped>>

(* rocketnet_client_transport_counters on the caller's thread *)
CallerRead ==
    /\ ~destroyed
    /\ reported' = SendFailed
    /\ dipped' = (dipped \/ SendFailed < reported)
    /\ UNCHANGED <<committed, seen, closed, handle, memory, frees, reader, joined, destroyed, fault>>
    /\ UNCHANGED <<ringVars, failures, kept>>

(* rocketnet_client_race_kept, then the receive or wait: the reader's
   acquire load; on the first sight of the commit it closes the losing
   family's handles, then reads only the kept family. *)
ReaderStep ==
    /\ reader \in {"running", "stopping"}
    /\ seen' = committed
    /\ IF committed >= 0 /\ ~closed
       THEN /\ closed' = TRUE
            /\ kept' = failures[Loser]
            /\ handle' = [handle EXCEPT ![Loser] = "closed"]
            /\ IF "readerfrees" \in Defect
               THEN /\ memory' = [memory EXCEPT ![Loser] = "freed"]
                    /\ frees' = frees \cup {<<"reader", Loser>>}
               ELSE UNCHANGED <<memory, frees>>
            /\ Use("reader", {committed})
       ELSE /\ UNCHANGED <<closed, handle, memory, frees, kept>>
            /\ IF committed >= 0 THEN Use("reader", {committed}) ELSE Use("reader", Families)
    /\ UNCHANGED <<committed, reader, joined, destroyed>>
    /\ UNCHANGED <<ringVars, failures, reported, dipped>>

(* the broken reader: closes a family it still reads, before any commit *)
ReaderCloseEarly(f) ==
    /\ "readerclosesearly" \in Defect
    /\ reader \in {"running", "stopping"} /\ committed < 0 /\ handle[f] = "open"
    /\ handle' = [handle EXCEPT ![f] = "closed"]
    /\ UNCHANGED <<committed, seen, closed, memory, frees, reader, joined, destroyed, fault>>
    /\ UNCHANGED <<ringVars, failVars>>

(* rocketnet_client_transport_stop: the request, then the reader's exit *)
CallerStop ==
    /\ reader = "running"
    /\ reader' = "stopping"
    /\ UNCHANGED <<committed, seen, closed, handle, memory, frees, joined, destroyed, fault>>
    /\ UNCHANGED <<ringVars, failVars>>

ReaderExit ==
    /\ reader = "stopping"
    /\ reader' = "exited"
    /\ UNCHANGED <<committed, seen, closed, handle, memory, frees, joined, destroyed, fault>>
    /\ UNCHANGED <<ringVars, failVars>>

CallerJoin ==
    /\ reader = "exited" /\ ~joined
    /\ joined' = TRUE
    /\ UNCHANGED <<committed, seen, closed, handle, memory, frees, reader, destroyed, fault>>
    /\ UNCHANGED <<ringVars, failVars>>

(* rocketnet_client_transport_destroy, then rocketnet_client_race_destroy:
   the caller closes what is still open and frees both families. *)
CallerDestroy ==
    /\ ~destroyed
    /\ joined \/ ("freebeforejoin" \in Defect /\ reader # "running")
    /\ destroyed' = TRUE
    /\ handle' = [f \in Families |-> "closed"]
    /\ memory' = [f \in Families |-> "freed"]
    /\ frees' = frees \cup {<<"caller", f>> : f \in {g \in Families : memory[g] = "live"}}
    /\ UNCHANGED <<committed, seen, closed, reader, joined, fault>>
    /\ UNCHANGED <<ringVars, failVars>>

Done ==
    /\ destroyed
    /\ UNCHANGED vars

Next ==
    \/ CallerSend
    \/ \E f \in Families : CallerCommit(f)
    \/ CallerHandOffLate \/ CallerHandOff
    \/ \E f \in Families : SendFails(f)
    \/ CallerRead
    \/ ReaderStep
    \/ \E f \in Families : ReaderCloseEarly(f)
    \/ CallerStop \/ ReaderExit \/ CallerJoin \/ CallerDestroy
    \/ Done

Spec == Init /\ [][Next]_vars
FairSpec == Spec
    /\ WF_vars(ReaderStep) /\ WF_vars(CallerStop) /\ WF_vars(ReaderExit)
    /\ WF_vars(CallerJoin) /\ WF_vars(CallerDestroy)

TypeOK ==
    /\ committed \in {-1, 0, 1} /\ seen \in {-1, 0, 1} /\ closed \in BOOLEAN
    /\ handle \in [Families -> {"open", "closed"}]
    /\ memory \in [Families -> {"live", "freed"}]
    /\ frees \subseteq (Threads \X Families)
    /\ reader \in {"running", "stopping", "exited"}
    /\ joined \in BOOLEAN /\ destroyed \in BOOLEAN
    /\ fault \in {"none"} \cup Threads
    /\ ringTo \in {-1} \cup Families /\ ringPost \in BOOLEAN /\ lost \in BOOLEAN
    /\ failures \in [Families -> 0..MaxFail] /\ kept \in 0..MaxFail
    /\ reported \in 0..(2 * MaxFail) /\ dipped \in BOOLEAN

(* No thread sends, receives or waits on a closed handle or freed memory. *)
NoUseAfterClose == fault = "none"
(* Only the caller's thread frees. *)
FreedOnlyByCaller == \A r \in frees : r[1] = "caller"
(* Memory is freed only after the reading thread is joined. *)
FreedOnlyAfterJoin == \A f \in Families : memory[f] = "freed" => joined
(* Each family is freed at most once, and a freed family is closed. *)
FreedOnce == \A f \in Families : Cardinality({r \in frees : r[2] = f}) <= 1
FreedIsClosed == \A f \in Families : memory[f] = "freed" => handle[f] = "closed"
(* The reader closes only the losing family, and only after the commit. *)
ClosedOnlyLoser == \A f \in Families : handle[f] = "closed" /\ ~destroyed => committed >= 0 /\ f = Loser
(* Once the reader sees the commit, the losing family is closed (it closes
   in the same step that first sees it). *)
SeenMeansClosed == seen >= 0 /\ ~destroyed => handle[1 - seen] = "closed"

(* Every slot the attempt commits after the commit leaves for the kept
   family: none is dropped as the losing family's. *)
PostCommitSlotsKept == ~lost
(* SEND_FAILED, read on the caller's thread, never goes down. *)
SendFailedMonotonic == ~dipped

(* With the reader and the caller fair, the transport is destroyed with
   every family freed, by the caller, once. *)
EverythingFreed == <>(destroyed /\ \A f \in Families : memory[f] = "freed")
(* A commit the reader goes on running past closes the losing family. *)
LoserClosed == committed >= 0 ~> (committed >= 0 /\ handle[Loser] = "closed")
=============================================================================
