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
   port) and reads the kept family only. At its exit, after stop, the I/O
   thread (or the caller's own step) reads what is still queued and closes
   every family still open (the race's close operation,
   rocketnet_client_race_close): both while uncommitted, the kept one after
   the commit. The legacy receive worker closes nothing at its exit, for one
   family as for two; the model's exit is the I/O thread's. The caller sends, hands off and commits nothing
   after stop (each entry point returns at stop_requested). Nothing is freed
   by the reader: destroy, on the caller's thread, after stop and join, frees
   both families' memory and the race block.

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
     "noexitclose"    the reader's exit closes nothing it had not closed at
                      a step (the race had no close operation, so a family
                      it kept open stayed open from the exit to destroy):
                      ExitedIsClosed
     "nolivecheck"    destroy frees each family whether or not its memory
                      is still live (rocketnet_client_transport_io_close
                      without its instance check and clear); with
                      "readerfrees" a family is freed twice: FreedOnce
     "destroytwice"   the transport is destroyed a second time; with
                      "nolivecheck" the caller frees each family twice,
                      a repeat by the same owner: FreedOnce
     "nostepclose"    the reader's step does not close the losing family
                      at the first sight of the commit; its exit still
                      does: StepClosesAtSight
     "noreaderclose"  the reading thread closes nothing, at a step or at its
                      exit (a backend with no close operation), so only
                      destroy closes the losing family: LoserClosed

   Destroy's single free per family rests on two things in the code: the
   transport is destroyed once (the API's contract; CallerDestroy's
   ~destroyed guard), and rocketnet_client_transport_io_close destroys a
   family only while its instance is set and then clears it. frees counts
   every free per <<thread, family>>, so a repeat free by the same owner is
   visible to FreedOnce.

   The send's destination. After the commit the models drop a send whose
   destination is not servers[committed]; the code drops by address family:
   rocketnet_client_race_send, rocketnet_client_race_send_progress and
   rocketnet_client_race_send_batch each take a slot's family from its
   destination with rocketnet_client_race_family_of (the family whose server
   has the address family of to) and, after the commit, send only the slots
   of the kept family. These are the same: the protocol only ever sends to
   the two server addresses,
   server_address[0] (IPv4) and server_address[1] (IPv6), so a destination's
   address family names its server; and the commit sets
   protocol.server_address to server_address[kept] and rebinds a Filling
   ring bound to the losing server (rocketnet_client_io_rebind_held), so
   after the commit every send and every ring is addressed to the kept
   server. A slot addressed to the losing server after the commit is the
   "commitkeepsring" defect, never the code. The model's family numbers
   name the servers: the destination is a family.

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
                         "commitkeepsring", "nokeepatcommit", "noexitclose", "nolivecheck",
                         "destroytwice", "nostepclose", "noreaderclose"}

MaxFail == 2

Families == {0, 1}
Threads == {"caller", "reader"}
VARIABLES committed,  \* -1 while both are in flight, else the kept family (caller writes, release)
          seen,       \* the reader's acquire load of committed
          closed,     \* the reader closed the losing family (race->closed)
          closer,     \* ghost: the action that closed the losing family's handle after
                      \* the commit, "none", "step" (ReaderStep), "exit" (ReaderExit)
                      \* or "destroy" (CallerDestroy)
          handle,    \* [Families -> {"open", "closed"}]
          memory,     \* [Families -> {"live", "freed"}]
          frees,      \* [Threads \X Families -> Nat]: the frees each thread made of each family
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
vars == <<committed, seen, closed, closer, handle, memory, frees, reader, joined, destroyed, fault,
          ringTo, ringPost, lost, failures, kept, reported, dipped>>
ringVars == <<ringTo, ringPost, lost>>
failVars == <<failures, kept, reported, dipped>>

Loser == 1 - committed
\* the protocol's server: the primary family's until the commit, then the kept family's
Server == IF committed >= 0 THEN committed ELSE 0
\* rocketnet_client_race_send_failures
SendFailed == IF committed >= 0 THEN failures[committed] + kept ELSE failures[0] + failures[1]
\* the reader's step closes the losing family at its first sight of the commit
StepCloses == "nostepclose" \notin Defect /\ "noreaderclose" \notin Defect
Usable(f) == handle[f] = "open" /\ memory[f] = "live"
Use(t, fs) == fault' = IF \A f \in fs : Usable(f) THEN fault ELSE t

Init ==
    /\ committed = -1 /\ seen = -1 /\ closed = FALSE /\ closer = "none"
    /\ handle = [f \in Families |-> "open"]
    /\ memory = [f \in Families |-> "live"]
    /\ frees = [p \in Threads \X Families |-> 0]
    /\ reader = "running" /\ joined = FALSE /\ destroyed = FALSE
    /\ fault = "none"
    /\ ringTo = -1 /\ ringPost = FALSE /\ lost = FALSE
    /\ failures = [f \in Families |-> 0] /\ kept = 0 /\ reported = 0 /\ dipped = FALSE

(* rocketnet_client_race_send / send_batch, the caller: both families while
   in flight, the kept one after the commit. *)
CallerSend ==
    /\ ~destroyed /\ reader = "running"
    /\ IF committed < 0 THEN Use("caller", Families)
       ELSE IF "callerusesloser" \in Defect THEN Use("caller", Families)
       ELSE Use("caller", {committed})
    /\ UNCHANGED <<committed, seen, closed, closer, handle, memory, frees, reader, joined, destroyed>>
    /\ UNCHANGED <<ringVars, failVars>>

(* rocketnet_client_transport_io_hand_off, then rocketnet_client_io_hand_off
   returning LATE: a slot is committed into the Filling ring, the first
   attempt binds its destination to the protocol's server, and the ring stays
   with the caller. *)
CallerHandOffLate ==
    /\ ~destroyed /\ reader = "running"
    /\ ringTo' = IF ringTo < 0 THEN Server ELSE ringTo
    /\ ringPost' = (ringPost \/ committed >= 0)
    /\ UNCHANGED <<committed, seen, closed, closer, handle, memory, frees, reader, joined, destroyed, fault>>
    /\ UNCHANGED <<lost, failVars>>

(* the same hand-off returning HANDED_OFF: the ring leaves for its bound
   destination through rocketnet_client_race_send_batch, which after the
   commit sends only what is addressed to the kept family. A slot committed
   after the commit is in it when this attempt commits one after the commit
   or an earlier LATE did. *)
CallerHandOff ==
    /\ ~destroyed /\ reader = "running"
    /\ LET to   == IF ringTo < 0 THEN Server ELSE ringTo
           post == ringPost \/ committed >= 0
       IN lost' = (lost \/ (post /\ to # committed))
    /\ ringTo' = -1 /\ ringPost' = FALSE
    /\ UNCHANGED <<committed, seen, closed, closer, handle, memory, frees, reader, joined, destroyed, fault>>
    /\ UNCHANGED failVars

(* rocketnet_client_race_commit, the caller in update: rebinds a Filling ring
   bound to the losing server (rocketnet_client_io_rebind_held) and keeps the
   losing family's send failures, then the release store. *)
CallerCommit(f) ==
    /\ ~destroyed /\ reader = "running" /\ committed < 0
    /\ committed' = f
    /\ ringTo' = IF ringTo = 1 - f /\ "commitkeepsring" \notin Defect THEN f ELSE ringTo
    /\ kept' = IF "nokeepatcommit" \in Defect THEN kept ELSE failures[1 - f]
    /\ UNCHANGED <<seen, closed, closer, handle, memory, frees, reader, joined, destroyed, fault>>
    /\ UNCHANGED <<ringPost, lost, failures, reported, dipped>>

(* a send on an open family fails: the I/O thread's or the caller's, before
   the commit or, on the losing family, between the commit and the close *)
SendFails(f) ==
    /\ handle[f] = "open" /\ memory[f] = "live" /\ failures[f] < MaxFail
    /\ failures' = [failures EXCEPT ![f] = @ + 1]
    /\ UNCHANGED <<committed, seen, closed, closer, handle, memory, frees, reader, joined, destroyed, fault>>
    /\ UNCHANGED <<ringVars, kept, reported, dipped>>

(* rocketnet_client_transport_counters on the caller's thread *)
CallerRead ==
    /\ ~destroyed
    /\ reported' = SendFailed
    /\ dipped' = (dipped \/ SendFailed < reported)
    /\ UNCHANGED <<committed, seen, closed, closer, handle, memory, frees, reader, joined, destroyed, fault>>
    /\ UNCHANGED <<ringVars, failures, kept>>

(* rocketnet_client_race_kept, then the receive or wait: the reader's
   acquire load; on the first sight of the commit it closes the losing
   family's handles, then reads only the kept family. *)
ReaderStep ==
    /\ reader \in {"running", "stopping"}
    /\ seen' = committed
    /\ IF committed >= 0 /\ ~closed /\ StepCloses
       THEN /\ closed' = TRUE
            /\ kept' = failures[Loser]
            /\ handle' = [handle EXCEPT ![Loser] = "closed"]
            /\ closer' = IF handle[Loser] = "open" THEN "step" ELSE closer
            /\ IF "readerfrees" \in Defect
               THEN /\ memory' = [memory EXCEPT ![Loser] = "freed"]
                    /\ frees' = [frees EXCEPT ![<<"reader", Loser>>] = @ + 1]
               ELSE UNCHANGED <<memory, frees>>
            /\ Use("reader", {committed})
       ELSE /\ UNCHANGED <<closed, handle, memory, frees, kept, closer>>
            /\ IF committed >= 0 THEN Use("reader", {committed}) ELSE Use("reader", Families)
    /\ UNCHANGED <<committed, reader, joined, destroyed>>
    /\ UNCHANGED <<ringVars, failures, reported, dipped>>

(* the broken reader: closes a family it still reads, before any commit *)
ReaderCloseEarly(f) ==
    /\ "readerclosesearly" \in Defect
    /\ reader \in {"running", "stopping"} /\ committed < 0 /\ handle[f] = "open"
    /\ handle' = [handle EXCEPT ![f] = "closed"]
    /\ UNCHANGED <<committed, seen, closed, closer, memory, frees, reader, joined, destroyed, fault>>
    /\ UNCHANGED <<ringVars, failVars>>

(* rocketnet_client_transport_stop: the request, then the reader's exit *)
CallerStop ==
    /\ reader = "running"
    /\ reader' = "stopping"
    /\ UNCHANGED <<committed, seen, closed, closer, handle, memory, frees, joined, destroyed, fault>>
    /\ UNCHANGED <<ringVars, failVars>>

(* rocketnet_client_io_discard_at_stop, the reader's exit: its discard reads
   through rocketnet_client_race_kept, which closes the losing family on a
   commit not yet seen, then the race's close operation
   (rocketnet_client_race_close) closes every family still open. This is the
   I/O thread's exit; the legacy receive worker runs no exit discard and
   closes nothing at its exit, for one family as for two, and destroy closes
   what it left open. *)
ReaderExit ==
    /\ reader = "stopping"
    /\ reader' = "exited"
    /\ seen' = committed
    /\ IF committed >= 0 THEN Use("reader", {committed}) ELSE Use("reader", Families)
    /\ IF committed >= 0 /\ ~closed
       THEN kept' = failures[Loser]
       ELSE UNCHANGED kept
    /\ IF "noreaderclose" \in Defect
       THEN UNCHANGED <<closed, handle>>
       ELSE IF "noexitclose" \in Defect
       THEN /\ closed' = (closed \/ committed >= 0)
            /\ handle' = IF committed >= 0 THEN [handle EXCEPT ![Loser] = "closed"] ELSE handle
       ELSE /\ closed' = TRUE
            /\ handle' = [f \in Families |-> "closed"]
    /\ closer' = IF committed >= 0 /\ handle[Loser] = "open" /\ handle'[Loser] = "closed"
                 THEN "exit" ELSE closer
    /\ UNCHANGED <<committed, memory, frees, joined, destroyed>>
    /\ UNCHANGED <<ringVars, failures, reported, dipped>>

CallerJoin ==
    /\ reader = "exited" /\ ~joined
    /\ joined' = TRUE
    /\ UNCHANGED <<committed, seen, closed, closer, handle, memory, frees, reader, destroyed, fault>>
    /\ UNCHANGED <<ringVars, failVars>>

(* rocketnet_client_transport_destroy, then rocketnet_client_race_destroy:
   the caller closes what is still open and frees both families, each
   through rocketnet_client_transport_io_close, which destroys a family only
   while its instance is set and then clears it (a family whose memory is
   already gone is not freed again). The transport is destroyed once. *)
CallerDestroy ==
    /\ \/ ~destroyed
       \/ "destroytwice" \in Defect /\ \A p \in Threads \X Families : frees[p] < 2
    /\ joined \/ ("freebeforejoin" \in Defect /\ reader # "running")
    /\ destroyed' = TRUE
    /\ handle' = [f \in Families |-> "closed"]
    /\ memory' = [f \in Families |-> "freed"]
    /\ frees' = [p \in Threads \X Families |->
                    IF p[1] = "caller" /\ (memory[p[2]] = "live" \/ "nolivecheck" \in Defect)
                    THEN frees[p] + 1 ELSE frees[p]]
    /\ closer' = IF committed >= 0 /\ handle[Loser] = "open" THEN "destroy" ELSE closer
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
    /\ closer \in {"none", "step", "exit", "destroy"}
    /\ handle \in [Families -> {"open", "closed"}]
    /\ memory \in [Families -> {"live", "freed"}]
    /\ frees \in [Threads \X Families -> 0..2]
    /\ reader \in {"running", "stopping", "exited"}
    /\ joined \in BOOLEAN /\ destroyed \in BOOLEAN
    /\ fault \in {"none"} \cup Threads
    /\ ringTo \in {-1} \cup Families /\ ringPost \in BOOLEAN /\ lost \in BOOLEAN
    /\ failures \in [Families -> 0..MaxFail] /\ kept \in 0..MaxFail
    /\ reported \in 0..(2 * MaxFail) /\ dipped \in BOOLEAN

(* No thread sends, receives or waits on a closed handle or freed memory. *)
NoUseAfterClose == fault = "none"
(* Only the caller's thread frees. *)
FreedOnlyByCaller == \A f \in Families : frees[<<"reader", f>>] = 0
(* Memory is freed only after the reading thread is joined. *)
FreedOnlyAfterJoin == \A f \in Families : memory[f] = "freed" => joined
(* Each family is freed at most once, by any thread, counting repeats by the
   same thread, and a freed family is closed. *)
FreedOnce == \A f \in Families : frees[<<"caller", f>>] + frees[<<"reader", f>>] <= 1
FreedIsClosed == \A f \in Families : memory[f] = "freed" => handle[f] = "closed"
(* Before its exit the reader closes only the losing family, and only after
   the commit. *)
ClosedOnlyLoser == \A f \in Families : handle[f] = "closed" /\ ~destroyed /\ reader # "exited" => committed >= 0 /\ f = Loser
(* Once the reader (the I/O thread, or the caller's step) has exited, no
   family is open: a datagram that arrives
   after the exit's discard meets a closed port, and none waits unread for
   destroy to free it (standard/TRANSPORT.md t13.3). *)
ExitedIsClosed == reader = "exited" => \A f \in Families : handle[f] = "closed"
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
(* A commit leads to the reading thread closing the losing family: its step
   at the first sight of the commit, or its exit when it is stopped first
   (the I/O thread's exit; the legacy receive worker's exit closes nothing
   and is not modelled, see ReaderExit). It is stated over the ghost closer,
   so the close every family gets at destroy (CallerDestroy) does not
   satisfy it. *)
LoserClosed == committed >= 0 ~> closer \in {"step", "exit"}
(* While the reader has not exited, a reader that has seen the commit closed
   the losing family in that step: the exit's close does not count. *)
StepClosesAtSight == seen >= 0 /\ reader # "exited" => closer = "step"
=============================================================================
