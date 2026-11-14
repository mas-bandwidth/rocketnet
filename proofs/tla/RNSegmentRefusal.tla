---------------------------- MODULE RNSegmentRefusal ----------------------------
(***************************************************************************)
(* UDP segmentation refusal on the server's Linux transports, per         *)
(* destination (sdk/rocketnet_server_gso.h rocketnet_segment_refused_t;    *)
(* sdk/rocketnet_server_gso_linux.cpp rocketnet_server_gso_flush;          *)
(* sdk/rocketnet_uring_queue_worker.cpp                                    *)
(* rocketnet_uring_queue_worker_send_one and                               *)
(* rocketnet_uring_queue_worker_send_cqe).                                 *)
(*                                                                         *)
(* A run is a send of more than one packet to one destination.  Run(d) is *)
(* the worker sending d's next run: it leaves segmented when the bank it   *)
(* came from was gathered with segmentation on (gathered: the gso flush    *)
(* reads FEATURE_SEGMENT once a pass and the io_uring gather cuts runs at  *)
(* max_segments, so a run gathered before a fallback still leaves          *)
(* segmented after it) and its destination is not in the table; plain     *)
(* otherwise.  A segmented run to a destination that accepts segmentation  *)
(* is accepted (accepted).  A segmented run to a destination whose route   *)
(* cannot take one (Refusing, an environment fact: an IPsec policy, say), *)
(* or to any destination when the kernel cannot segment (KernelSegments = *)
(* FALSE), goes into the kernel (inflight) and comes back refused:         *)
(* Refuse(d) is the worker handling that answer.  The io_uring worker can  *)
(* have several of one destination's runs in the kernel before the first  *)
(* answer (InFlight); the gso flush sends one message at a time and       *)
(* handles its answer before the next (InFlight = 1).  A refused run is    *)
(* resent plain and its destination remembered, so its later runs leave    *)
(* plain; a plain resend can itself fail and drop its packet, counted      *)
(* (PlainFails, plainDropped).  Segmentation turns off (for the queue, or *)
(* for the whole gso tier: FallbackScope) only when Threshold refusals     *)
(* counted and no segmented send was ever accepted.  A destination's own   *)
(* plain run, and a run of one packet, change nothing and are not          *)
(* modelled.                                                               *)
(*                                                                         *)
(* Per-client scope (TRANSPORT.md t2.4 rule 7): a run that a destination  *)
(* which accepts segmentation sends plain, after the transport has shown   *)
(* it can segment, is another destination's condition costing it          *)
(* (OtherDestinationUnaffected); a refusal never drops the run, it is      *)
(* resent plain (NothingDropped; a plain send that then fails is that      *)
(* send's drop, counted, not the refusal's); each destination pays at most *)
(* InFlight refusals, the runs it had in the kernel when the first answer  *)
(* came (RefusalsBounded, which holds when no two destinations share an   *)
(* entry).  The remembered set is the code's direct-mapped table at its   *)
(* layout: Slot names each destination's entry, a refusing destination     *)
(* takes its entry and displaces the one there, which pays a refusal again *)
(* on its next run; a destination that never refused is never in the      *)
(* table, because the table matches whole addresses.  The fallback counts  *)
(* every refused answer (rocketnet_segment_refused_add, saturating at     *)
(* Threshold): a displaced destination's next refusal, and each answer of  *)
(* one destination's runs that were in the kernel together, so the count   *)
(* is never below the number of distinct refusing destinations             *)
(* (KernelFallback) and may pass it.  Mode "fillcount" is the code before: *)
(* it counted only a refusal that filled an empty entry, so with two       *)
(* refusers sharing an entry the fallback never fired.                     *)
(*                                                                         *)
(* Mode carries the reversed witnesses, each the code before this rule.    *)
(* "transport" is the io_uring transport's: the first refusal drops the    *)
(* run and turns segmentation off for every destination.  "consecutive"    *)
(* is the gso transport's: the run is resent plain and nothing is          *)
(* remembered, and Threshold refusals with no accepted segmented send      *)
(* between them turn segmentation off for every destination.               *)
(*                                                                         *)
(* Queues: each destination's sends go through one queue (Queue, the       *)
(* io_uring worker or gso lane of rocketnet_send_queue_of), and each queue *)
(* keeps its own table and count.  The segment flag is the queue's for    *)
(* io_uring (FallbackScope "queue": the worker's config.segment) and the   *)
(* tier's for gso (FallbackScope "transport": one FEATURE_SEGMENT bit, so *)
(* a lane that reaches the threshold turns it off for every lane).         *)
(* Whether a segmented send was ever accepted is the transport's           *)
(* (AcceptedRule "transport": the shared segment_accepted the refusal      *)
(* reads), so a queue whose own destinations all refuse keeps segmenting   *)
(* once another queue's segmented send was accepted.  AcceptedRule "queue" *)
(* is the io_uring code before: the flag was the queue's alone.  Refuse    *)
(* is one step: the worker reads segment_accepted, counts and decides; the *)
(* flag is relaxed, so "accepted" is what the refusing worker sees at its *)
(* read (README-RNSegmentRefusal.md).  An accepted run's answer is taken   *)
(* at its send, Run: for io_uring that is earlier than the reap that sets *)
(* the flag in the code, and the window it leaves is that same read.       *)
(***************************************************************************)
EXTENDS Naturals, FiniteSets

CONSTANTS Dests,            \* the destinations
          Refusing,         \* the destinations whose route refuses a segmented send
          KernelSegments,   \* FALSE: the kernel refuses every segmented send
          Threshold,        \* ROCKETNET_SEGMENT_CONSECUTIVE_REFUSAL_THRESHOLD
          MaxRuns,          \* bound on runs
          Mode,             \* "perdest" (the code), "fillcount", "transport" or "consecutive"
          Slot,             \* each destination's entry (rocketnet_segment_refused_entry)
          Queue,            \* each destination's queue (rocketnet_send_queue_of)
          AcceptedRule,     \* "transport" (the code) or "queue"
          FallbackScope,    \* "queue" (io_uring) or "transport" (gso)
          InFlight,         \* one destination's segmented runs in the kernel at once (gso 1)
          PlainFails        \* a plain resend may fail with a hard error

ASSUME Refusing \subseteq Dests
ASSUME KernelSegments \in BOOLEAN
ASSUME Threshold \in Nat \ {0}
ASSUME MaxRuns \in Nat
ASSUME Mode \in {"perdest", "fillcount", "transport", "consecutive"}
ASSUME Slot \in [Dests -> Nat]
ASSUME Queue \in [Dests -> Nat]
ASSUME AcceptedRule \in {"transport", "queue"}
ASSUME FallbackScope \in {"queue", "transport"}
ASSUME InFlight \in Nat \ {0}
ASSUME PlainFails \in BOOLEAN

NoDest == "none"
Entries == {Slot[d] : d \in Dests}
Queues == {Queue[d] : d \in Dests}

VARIABLES segOn,        \* each queue's segment flag (config.segment; the gso FEATURE_SEGMENT bit)
          gathered,     \* each queue's held bank was gathered with segmentation on
          table,        \* each queue's table: each entry's destination, or NoDest
          count,        \* each queue's segment_refused.destinations
          inflight,     \* each destination's segmented runs in the kernel, to come back refused
          everRefused,  \* destinations that ever refused (not code state: the property's measure)
          accepted,     \* a segmented send was accepted since create, on any queue (segment_accepted)
          qAccepted,    \* each queue's own segment_refused.accepted ("queue")
          streak,       \* refusals since the last accepted segmented send ("consecutive")
          refusals,     \* refusals each destination paid
          wronglyPlain, \* destinations that accept segmentation and sent a run plain after one was accepted
          offAccepted,  \* a queue turned segmentation off after a segmented send was accepted (not code state)
          segAfterOff,  \* a run left segmented while its queue's flag was off (not code state)
          dropped,      \* a refusal dropped its run instead of resending it ("transport")
          plainDropped, \* a plain resend failed and its packet was dropped, counted (not code state)
          runs          \* runs sent

vars == <<segOn, gathered, table, count, inflight, everRefused, accepted, qAccepted, streak,
          refusals, wronglyPlain, offAccepted, segAfterOff, dropped, plainDropped, runs>>

Refuses(d) == ~KernelSegments \/ d \in Refusing

\* rocketnet_segment_refused_contains: the entry of d's queue holds d
Remembered(d) == table[Queue[d]][Slot[d]] = d

\* what a refusal on queue q reads as accepted
Accepted(q) == IF AcceptedRule = "transport" THEN accepted ELSE qAccepted[q]

\* the queues a fallback on queue q turns off
OffScope(q) == IF FallbackScope = "queue" THEN {q} ELSE Queues

Init ==
    /\ segOn = [q \in Queues |-> TRUE]
    /\ gathered = [q \in Queues |-> TRUE]
    /\ table = [q \in Queues |-> [e \in Entries |-> NoDest]]
    /\ count = [q \in Queues |-> 0]
    /\ inflight = [d \in Dests |-> 0]
    /\ everRefused = {}
    /\ accepted = FALSE
    /\ qAccepted = [q \in Queues |-> FALSE]
    /\ streak = 0
    /\ refusals = [d \in Dests |-> 0]
    /\ wronglyPlain = {}
    /\ offAccepted = FALSE
    /\ segAfterOff = FALSE
    /\ dropped = FALSE
    /\ plainDropped = FALSE
    /\ runs = 0

\* the worker sends d's next run, from the bank it holds or from a new bank
\* it gathers now (fresh: the gather reads the queue's flag).  The run
\* leaves segmented when its bank was gathered with segmentation on and d
\* is not in the table; to a destination that refuses it goes into the
\* kernel and comes back refused (Refuse), otherwise it is accepted.
Run(d) ==
    LET q == Queue[d] IN
    /\ runs < MaxRuns
    /\ runs' = runs + 1
    /\ \E fresh \in BOOLEAN :
         LET g == IF fresh THEN segOn[q] ELSE gathered[q] IN
         /\ gathered' = [gathered EXCEPT ![q] = g]
         /\ IF g /\ ~Remembered(d)
            THEN /\ segAfterOff' = (segAfterOff \/ ~segOn[q])
                 /\ IF Refuses(d)
                    THEN /\ inflight[d] < InFlight
                         /\ inflight' = [inflight EXCEPT ![d] = @ + 1]
                         /\ UNCHANGED <<accepted, qAccepted, streak, wronglyPlain>>
                    ELSE /\ accepted' = TRUE
                         /\ qAccepted' = [qAccepted EXCEPT ![q] = TRUE]
                         /\ streak' = 0
                         /\ UNCHANGED <<inflight, wronglyPlain>>
            ELSE /\ wronglyPlain' = IF accepted /\ ~Refuses(d) THEN wronglyPlain \cup {d} ELSE wronglyPlain
                 /\ UNCHANGED <<inflight, accepted, qAccepted, streak, segAfterOff>>
    /\ UNCHANGED <<segOn, table, count, everRefused, refusals, offAccepted, dropped, plainDropped>>

\* rocketnet_segment_refused_add: every refused answer counts, saturating
\* at Threshold (an answer for a destination already in the table, one of
\* its runs that was in the kernel with the first, included); "fillcount"
\* counted only a refusal that filled an empty entry
Counted(d) ==
    LET q == Queue[d] IN
    IF Mode = "fillcount"
    THEN IF table[q][Slot[d]] = NoDest THEN count[q] + 1 ELSE count[q]
    ELSE IF count[q] < Threshold THEN count[q] + 1 ELSE count[q]

\* the kernel's answer to one of d's segmented runs is a refusal (EINVAL,
\* EIO or EOPNOTSUPP) and the worker handles it: it reads the accepted
\* flag, rocketnet_segment_refused_add remembers d and counts, and the run
\* is resent plain (rocketnet_server_gso_resend_plain; the io_uring hand-back
\* one packet a run), a plain send that may fail and drop its packet
Refuse(d) ==
    LET q == Queue[d] IN
    /\ inflight[d] > 0
    /\ inflight' = [inflight EXCEPT ![d] = @ - 1]
    /\ refusals' = [refusals EXCEPT ![d] = @ + 1]
    /\ everRefused' = everRefused \cup {d}
    /\ CASE Mode \in {"perdest", "fillcount"} ->
              /\ table' = [table EXCEPT ![q][Slot[d]] = d]
              /\ count' = [count EXCEPT ![q] = Counted(d)]
              /\ segOn' = IF ~Accepted(q) /\ Counted(d) >= Threshold
                          THEN [p \in Queues |-> IF p \in OffScope(q) THEN FALSE ELSE segOn[p]]
                          ELSE segOn
              /\ \E lost \in (IF PlainFails THEN BOOLEAN ELSE {FALSE}) :
                    plainDropped' = (plainDropped \/ lost)
              /\ UNCHANGED <<streak, dropped>>
         [] Mode = "transport" ->
              /\ segOn' = [p \in Queues |-> FALSE]
              /\ dropped' = TRUE
              /\ UNCHANGED <<table, count, streak, plainDropped>>
         [] Mode = "consecutive" ->
              /\ streak' = streak + 1
              /\ segOn' = [p \in Queues |-> streak + 1 < Threshold]
              /\ \E lost \in (IF PlainFails THEN BOOLEAN ELSE {FALSE}) :
                    plainDropped' = (plainDropped \/ lost)
              /\ UNCHANGED <<table, count, dropped>>
    /\ offAccepted' = (offAccepted \/ (accepted /\ \E p \in Queues : segOn[p] /\ ~segOn'[p]))
    /\ UNCHANGED <<gathered, accepted, qAccepted, wronglyPlain, segAfterOff, runs>>

\* the bound on runs is reached and every refused run was answered
Done ==
    /\ runs = MaxRuns
    /\ \A d \in Dests : inflight[d] = 0
    /\ UNCHANGED vars

Next ==
    \/ \E d \in Dests : Run(d)
    \/ \E d \in Dests : Refuse(d)
    \/ Done

Spec == Init /\ [][Next]_vars

-----------------------------------------------------------------------------

TypeOK ==
    /\ segOn \in [Queues -> BOOLEAN]
    /\ gathered \in [Queues -> BOOLEAN]
    /\ table \in [Queues -> [Entries -> Dests \cup {NoDest}]]
    /\ count \in [Queues -> 0..MaxRuns]
    /\ inflight \in [Dests -> 0..InFlight]
    /\ everRefused \subseteq Dests
    /\ accepted \in BOOLEAN
    /\ qAccepted \in [Queues -> BOOLEAN]
    /\ streak \in 0..MaxRuns
    /\ refusals \in [Dests -> 0..MaxRuns]
    /\ wronglyPlain \subseteq Dests
    /\ offAccepted \in BOOLEAN
    /\ segAfterOff \in BOOLEAN
    /\ dropped \in BOOLEAN
    /\ plainDropped \in BOOLEAN
    /\ runs \in 0..MaxRuns

\* once the transport has shown it can segment, no destination that accepts
\* segmentation sends a run plain: another destination's refusal costs it
\* nothing
OtherDestinationUnaffected ==
    wronglyPlain = {}

\* t2.4 rule 7 for the transport: once a segmented send was accepted on any
\* queue, no queue's refusals turn its segmentation off (a fallback taken
\* before any was accepted stays, as the rule allows)
NoFallbackAfterAccepted ==
    ~offAccepted

\* a refusal never drops its run: the run is resent plain.  A plain resend
\* that then fails drops that packet, counted in SEND_DROPPED, as any plain
\* send's hard error does (plainDropped, not this rule)
NothingDropped ==
    ~dropped

\* a refusing destination pays at most one refusal for each of its runs
\* that was in the kernel when the first answer came, not one a run after
\* that (InFlight = 1: one refusal)
RefusalsBounded ==
    \A d \in Dests : refusals[d] <= InFlight

\* a kernel that cannot segment turns segmentation off for a queue once
\* Threshold distinct destinations of that queue refused, however they share
\* entries
KernelFallback ==
    \A q \in Queues :
        (Mode \in {"perdest", "fillcount"} /\ ~KernelSegments
         /\ Cardinality({d \in everRefused : Queue[d] = q}) >= Threshold) => ~segOn[q]

=============================================================================
