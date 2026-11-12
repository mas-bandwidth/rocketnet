------------------------- MODULE RNServerSendFair -------------------------
(***************************************************************************)
(* The server's per-update send pass (sdk/rocketnet_message_server.cpp,   *)
(* rn_message_object_send_packets and rn_message_connection_write) over    *)
(* the transport's shared per-update frame capacity K (send_capacity,     *)
(* sdk/rocketnet_server_transport.cpp), per client per update: the due    *)
(* test (rn_endpoint_next_packet_bytes, rn_endpoint_ordered_due), the      *)
(* frame start that takes one unit of the capacity (start_packet: a        *)
(* fetch-add of payload_ring.num_packets), the write                      *)
(* (rn_endpoint_write_packet), and the abort of a frame the write left     *)
(* empty (abort_packet), which does NOT give the unit back                 *)
(* (FIX-PROBE-SLOT section 3: no transport returns it).                   *)
(*                                                                         *)
(* The loop's own arithmetic, as in the code: the pass starts at           *)
(* send_next_connection, visits the clients in cyclic order one frame per  *)
(* client per round, repeats rounds while any client progressed, tests the *)
(* capacity before each client (send_capacity_available <= 0: goto flush), *)
(* and sets the cursor to the client after each one visited.  A visit is   *)
(* one action: rn_message_connection_write runs to its return.  A pass     *)
(* that answered 0 or wrote nothing stays ended until the next flush       *)
(* (rn_endpoint_begin_flush), so a client starts at most one empty frame   *)
(* per update.                                                             *)
(*                                                                         *)
(* Three kinds of client:                                                  *)
(*   healthy: Work frames or fewer of real data each update (the            *)
(*            environment picks how many); every frame started is written. *)
(*   Dark:    in probe hold (MESSAGES.md m9.3, the timeout off): two       *)
(*            reliable numbers on the wire and no acknowledgement, U and a *)
(*            later number H whose last send is older than U's and past    *)
(*            its delay; the walk (rn_ordered_walk) skips H and resends U  *)
(*            only, every Delay updates.  H stays the FIFO head.           *)
(*   Short:   the client's own send budget may refuse its due reliable     *)
(*            packet in an update (rocketnet_send_budget_admit WAIT: the   *)
(*            write returns 0, reliable_waiting ends the pass): a frame    *)
(*            started and aborted.  The v1.0.0 path FIX-PROBE-SLOT section *)
(*            3 names; not in the passing cases (MCRNServerSendFairShort   *)
(*            .cfg records it as the code's open fault).                  *)
(*                                                                         *)
(* DueRule is the due test for a dark client: "shared" (v1.1.1) asks the  *)
(* walk's own hold, so a held head stands for U and U's delay decides      *)
(* (rn_endpoint_fifo_due with rn_window_ack_held and                       *)
(* rn_window_ack_probe_slot); "head" (v1.1.0, the reversed witness         *)
(* MCRNServerSendFairBrokenDueIgnoresHold.cfg) reads the head alone, so H  *)
(* past its delay reports work every update and the walk writes nothing.  *)
(*                                                                         *)
(* Mode keeps the rotation's reversed witnesses: "fromzero" starts the     *)
(* pass at client 1 every update; "inplace" leaves the cursor on the       *)
(* client just visited.                                                    *)
(*                                                                         *)
(* What is claimed (MESSAGES.md m1.8, design (P)): FairService, every      *)
(* client with writable work is written within ceil(N / K) updates;        *)
(* NoUnitWasted, every unit of the capacity charged this update went to a  *)
(* frame that was written; OtherClientUnaffected, no client other than a   *)
(* dark or short one ends an update with work left while a unit of that    *)
(* update went to an aborted frame; DarkTakesOnlyItsProbes, a dark client  *)
(* starts a frame only for its probe resend due that update, and at most   *)
(* one.  Writable work is defined from the client's state, not from the   *)
(* due test: a healthy client's frames left, a dark client's U past its    *)
(* delay, a short client's packet its budget pays.                         *)
(***************************************************************************)
EXTENDS Naturals

CONSTANTS N,          \* connected clients, 1..N
          K,          \* frames the transport starts per update (shared)
          Work,       \* the most frames a healthy client offers per update
          Dark,       \* the clients held by probe pacing
          Short,      \* the clients whose own budget may refuse their packet
          Delay,      \* a dark client's resend delay for U, in updates
          DueRule,    \* "shared" (the rule) or "head" (the reversed witness)
          Mode        \* "fair", "fromzero" or "inplace"

ASSUME N \in Nat \ {0}
ASSUME K \in 1..N
ASSUME Work \in Nat \ {0}
ASSUME Dark \subseteq 1..N /\ Short \subseteq 1..N /\ Dark \cap Short = {}
ASSUME Delay \in Nat \ {0}
ASSUME DueRule \in {"shared", "head"}
ASSUME Mode \in {"fair", "fromzero", "inplace"}

Clients == 1..N
Healthy == Clients \ (Dark \cup Short)

Window == (N + K - 1) \div K

VARIABLES pc,        \* "between" updates or in the send "loop"
          cursor,    \* send_next_connection, the next first client
          start,     \* this update's first client
          off,       \* the offset of the next visit in this round
          progress,  \* a client wrote a frame this round
          used,      \* units of the capacity charged this update
          work,      \* [c -> healthy: frames left to write this update]
          uAge,      \* [c -> dark: updates since U's last send, saturating at Delay]
          pays,      \* [c -> short: its budget pays its packet this update]
          ended,     \* [c -> its pass ended this update]
          started,   \* ghost [c -> frames started for c this update, to 2]
          wrote,     \* ghost [c -> frames written for c this update, to 2]
          writable,  \* ghost [c -> c had writable work when the update began]
          waiting,   \* ghost [c -> updates c had writable work and none written, to Window]
          wasted,    \* ghost: units charged this update to frames aborted
          harmed     \* ghost: a client other than a dark or short one ended an
                     \* update with work left while a unit went to an aborted frame

vars == <<pc, cursor, start, off, progress, used, work, uAge, pays, ended,
          started, wrote, writable, waiting, wasted, harmed>>

Min(a, b) == IF a < b THEN a ELSE b

\* U is past its delay: the walk resends it (the one number the hold allows).
UDue(c) == uAge[c] >= Delay

\* What c can write this update, from its own state.
Writable(c) ==
    CASE c \in Dark  -> UDue(c)
      [] c \in Short -> pays[c]
      [] OTHER       -> work[c] > 0

\* rn_endpoint_next_packet_bytes > 0: the due test.  A healthy client is due
\* while it has frames left.  A short client has a due reliable packet every
\* update, paid or not (the due test does not ask the budget).  A dark
\* client: "shared" asks U's delay (a held head stands for U); "head" reads
\* the head H, on the wire past its delay: due every update.
ReportedDue(c) ==
    CASE c \in Dark  -> IF DueRule = "shared" THEN UDue(c) ELSE TRUE
      [] c \in Short -> TRUE
      [] OTHER       -> work[c] > 0

\* rn_endpoint_write_packet writes a frame: the walk resends U, the budget
\* pays, or the healthy client has data.
Writes(c) == Writable(c)

Init ==
    /\ pc = "between"
    /\ cursor = 1
    /\ start = 1
    /\ off = 0
    /\ progress = FALSE
    /\ used = 0
    /\ work = [c \in Clients |-> 0]
    /\ uAge = [c \in Clients |-> IF c \in Dark THEN Delay - 1 ELSE 0]
    /\ pays = [c \in Clients |-> FALSE]
    /\ ended = [c \in Clients |-> FALSE]
    /\ started = [c \in Clients |-> 0]
    /\ wrote = [c \in Clients |-> 0]
    /\ writable = [c \in Clients |-> FALSE]
    /\ waiting = [c \in Clients |-> 0]
    /\ wasted = 0
    /\ harmed = FALSE

\* The update begins: a dark client's U is one update older; the
\* environment offers each healthy client its frames and each short
\* client's budget pays or not; every pass reopens (rn_endpoint_begin_flush);
\* the loop starts at the cursor.
Aged == [c \in Clients |-> IF c \in Dark THEN Min(uAge[c] + 1, Delay) ELSE 0]
BeginFlush ==
    /\ pc = "between"
    /\ uAge' = Aged
    /\ \E w \in [Healthy -> 0..Work], p \in [Short -> BOOLEAN] :
          /\ work' = [c \in Clients |-> IF c \in Healthy THEN w[c] ELSE 0]
          /\ pays' = [c \in Clients |-> IF c \in Short THEN p[c] ELSE FALSE]
          /\ writable' = [c \in Clients |->
                CASE c \in Dark  -> Aged[c] >= Delay
                  [] c \in Short -> p[c]
                  [] OTHER       -> w[c] > 0]
    /\ pc' = "loop"
    /\ start' = IF Mode = "fromzero" THEN 1 ELSE cursor
    /\ off' = 0
    /\ progress' = FALSE
    /\ used' = 0
    /\ ended' = [c \in Clients |-> FALSE]
    /\ started' = [c \in Clients |-> 0]
    /\ wrote' = [c \in Clients |-> 0]
    /\ wasted' = 0
    /\ UNCHANGED <<cursor, waiting, harmed>>

\* The update ends (goto flush, or a round with no progress): the transport
\* sends and its count resets.
EndFlush(wr, wk, ws) ==
    /\ pc' = "between"
    /\ waiting' = [c \in Clients |->
          IF writable[c] /\ wr[c] = 0 THEN Min(waiting[c] + 1, Window) ELSE 0]
    /\ harmed' = (harmed \/ (ws > 0 /\ \E c \in Healthy : wk[c] > 0))

\* One visit of client i.
Visit ==
    /\ pc = "loop"
    /\ LET i == ((start - 1 + off) % N) + 1
           last == off + 1 = N
       IN
       IF used >= K
       THEN \* send_capacity_available <= 0: goto flush
            /\ EndFlush(wrote, work, wasted)
            /\ UNCHANGED <<cursor, start, off, progress, used, work, uAge, pays, ended,
                           started, wrote, writable, wasted>>
       ELSE
            LET frame == ~ended[i] /\ ReportedDue(i)
                ok    == frame /\ Writes(i)
                prog  == progress \/ ok
                wr1   == [wrote EXCEPT ![i] = IF ok THEN Min(@ + 1, 2) ELSE @]
                wk1   == [work EXCEPT ![i] = IF ok /\ i \in Healthy THEN @ - 1 ELSE @]
                ws1   == IF frame /\ ~ok THEN wasted + 1 ELSE wasted
            IN
            /\ used' = IF frame THEN used + 1 ELSE used
            /\ started' = [started EXCEPT ![i] = IF frame THEN Min(@ + 1, 2) ELSE @]
            /\ wrote' = wr1
            /\ work' = wk1
            /\ uAge' = [uAge EXCEPT ![i] = IF ok /\ i \in Dark THEN 0 ELSE @]
            \* the pass ends when it answers 0 or its frame is aborted; a
            \* short client's written packet ends it too (one due packet)
            /\ ended' = [ended EXCEPT ![i] = ~ok \/ i \in Short \/ @]
            /\ wasted' = ws1
            /\ cursor' = IF Mode = "inplace" THEN i ELSE (i % N) + 1
            /\ IF last /\ ~prog
               THEN /\ EndFlush(wr1, wk1, ws1)
                    /\ UNCHANGED <<start, off, progress>>
               ELSE /\ off' = IF last THEN 0 ELSE off + 1
                    /\ progress' = IF last THEN FALSE ELSE prog
                    /\ UNCHANGED <<pc, waiting, harmed>>
            /\ UNCHANGED <<start, pays, writable>>

Next == BeginFlush \/ Visit

Spec == Init /\ [][Next]_vars

-----------------------------------------------------------------------------

TypeOK ==
    /\ pc \in {"between", "loop"}
    /\ cursor \in Clients /\ start \in Clients
    /\ off \in 0..N-1
    /\ used \in 0..K
    /\ work \in [Clients -> 0..Work]
    /\ uAge \in [Clients -> 0..Delay]
    /\ waiting \in [Clients -> 0..Window]
    /\ wasted \in 0..K

\* Every client with writable work at the start of an update has a frame
\* written within ceil(N / K) updates.
FairService ==
    \A c \in Clients : waiting[c] < Window

\* Every unit of the shared capacity charged this update went to a frame
\* that was written.
NoUnitWasted == wasted = 0

\* (P) per client: a client that is neither dark nor short never ends an
\* update with frames left while a unit of that update's capacity went to
\* an aborted frame.
OtherClientUnaffected == ~harmed

\* A dark client starts a frame only when its probe resend is due, and at
\* most one per update.
DarkTakesOnlyItsProbes ==
    \A d \in Dark : started[d] <= (IF writable[d] THEN 1 ELSE 0)

=============================================================================
