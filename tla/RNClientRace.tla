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
   The model is finite with no counters: the caller may send, commit, stop,
   join and destroy; the reader may step any number of times until it exits.
   It does not model the bytes, the protocol, the cookie or C++ ordering
   beyond the one release store and acquire load of the commit. *)
EXTENDS Integers, FiniteSets
CONSTANTS Defect
ASSUME Defect \subseteq {"readerfrees", "callerusesloser", "freebeforejoin", "readerclosesearly"}

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
          fault       \* a use of a closed handle or freed memory: "none" or the thread
vars == <<committed, seen, closed, handle, memory, frees, reader, joined, destroyed, fault>>

Loser == 1 - committed
Usable(f) == handle[f] = "open" /\ memory[f] = "live"
Use(t, fs) == fault' = IF \A f \in fs : Usable(f) THEN fault ELSE t

Init ==
    /\ committed = -1 /\ seen = -1 /\ closed = FALSE
    /\ handle = [f \in Families |-> "open"]
    /\ memory = [f \in Families |-> "live"]
    /\ frees = {}
    /\ reader = "running" /\ joined = FALSE /\ destroyed = FALSE
    /\ fault = "none"

(* rocketnet_client_race_send / send_batch, the caller: both families while
   in flight, the kept one after the commit. *)
CallerSend ==
    /\ ~destroyed
    /\ IF committed < 0 THEN Use("caller", Families)
       ELSE IF "callerusesloser" \in Defect THEN Use("caller", Families)
       ELSE Use("caller", {committed})
    /\ UNCHANGED <<committed, seen, closed, handle, memory, frees, reader, joined, destroyed>>

(* rocketnet_client_race_commit, the caller in update: the release store. *)
CallerCommit(f) ==
    /\ ~destroyed /\ committed < 0
    /\ committed' = f
    /\ UNCHANGED <<seen, closed, handle, memory, frees, reader, joined, destroyed, fault>>

(* rocketnet_client_race_kept, then the receive or wait: the reader's
   acquire load; on the first sight of the commit it closes the losing
   family's handles, then reads only the kept family. *)
ReaderStep ==
    /\ reader \in {"running", "stopping"}
    /\ seen' = committed
    /\ IF committed >= 0 /\ ~closed
       THEN /\ closed' = TRUE
            /\ handle' = [handle EXCEPT ![Loser] = "closed"]
            /\ IF "readerfrees" \in Defect
               THEN /\ memory' = [memory EXCEPT ![Loser] = "freed"]
                    /\ frees' = frees \cup {<<"reader", Loser>>}
               ELSE UNCHANGED <<memory, frees>>
            /\ Use("reader", {committed})
       ELSE /\ UNCHANGED <<closed, handle, memory, frees>>
            /\ IF committed >= 0 THEN Use("reader", {committed}) ELSE Use("reader", Families)
    /\ UNCHANGED <<committed, reader, joined, destroyed>>

(* the broken reader: closes a family it still reads, before any commit *)
ReaderCloseEarly(f) ==
    /\ "readerclosesearly" \in Defect
    /\ reader \in {"running", "stopping"} /\ committed < 0 /\ handle[f] = "open"
    /\ handle' = [handle EXCEPT ![f] = "closed"]
    /\ UNCHANGED <<committed, seen, closed, memory, frees, reader, joined, destroyed, fault>>

(* rocketnet_client_transport_stop: the request, then the reader's exit *)
CallerStop ==
    /\ reader = "running"
    /\ reader' = "stopping"
    /\ UNCHANGED <<committed, seen, closed, handle, memory, frees, joined, destroyed, fault>>

ReaderExit ==
    /\ reader = "stopping"
    /\ reader' = "exited"
    /\ UNCHANGED <<committed, seen, closed, handle, memory, frees, joined, destroyed, fault>>

CallerJoin ==
    /\ reader = "exited" /\ ~joined
    /\ joined' = TRUE
    /\ UNCHANGED <<committed, seen, closed, handle, memory, frees, reader, destroyed, fault>>

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

Done ==
    /\ destroyed
    /\ UNCHANGED vars

Next ==
    \/ CallerSend
    \/ \E f \in Families : CallerCommit(f)
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

(* With the reader and the caller fair, the transport is destroyed with
   every family freed, by the caller, once. *)
EverythingFreed == <>(destroyed /\ \A f \in Families : memory[f] = "freed")
(* A commit the reader goes on running past closes the losing family. *)
LoserClosed == committed >= 0 ~> (committed >= 0 /\ handle[Loser] = "closed")
=============================================================================
