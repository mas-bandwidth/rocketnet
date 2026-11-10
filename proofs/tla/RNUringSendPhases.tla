--------------------------- MODULE RNUringSendPhases ---------------------------
(***************************************************************************)
(* The io_uring server's send staging at the code's layout                *)
(* (sdk/rocketnet_server_uring_linux.cpp rocketnet_server_uring_start_packet, *)
(* rocketnet_server_uring_send_packets, rocketnet_server_uring_submit_phase *)
(* and the send case of rocketnet_server_uring_reap_cqe).                  *)
(*                                                                         *)
(* The staging is Phases phases of Size slots in one array: slot s of      *)
(* phase p is staging slot Base(p) + s, Base(p) = p * Size.  start_packet  *)
(* takes the next index of the fill phase with one fetch_add and returns   *)
(* NULL when the phase is full (Reserve).  send_packets submits the staged *)
(* slots not yet submitted, the committed phase's first, in send runs: a   *)
(* run starts at the first slot that is neither aborted nor unroutable and *)
(* covers up to MaxSegments consecutive staged slots of the same phase,    *)
(* the last at a shorter size or another destination (SubmitRun).  A slot  *)
(* that is aborted or unroutable is skipped without a send (SkipSlot).     *)
(* Each send names its run's head by its staging slot; one completion      *)
(* retires the whole run (KernelComplete, Reap), and the phase of a reaped *)
(* send is its head's quotient by Size.  A submit pass may stop part way,  *)
(* the rest submitted by a later call; at its end send_packets resets      *)
(* every complete phase (every staged slot submitted or skipped, no send   *)
(* of it pending) and, when the fill phase still holds staged slots and    *)
(* the next phase is empty, moves the fill to it (SendEnd: the old fill    *)
(* phase is committed).  receive_packets reaps too, so a send whose        *)
(* completion the send call's budget did not reach is reaped one call late *)
(* (the Recv step).                                                        *)
(*                                                                         *)
(* send_pending counts sends, not slots: PendingMatches is the number of  *)
(* a phase's runs whose completion the kernel has not posted, so a run     *)
(* that retires several slots still lowers one count by one.               *)
(*                                                                         *)
(* LateReap = TRUE is the flood the code meets: every completion of an     *)
(* update's sends is reaped by that update's receive_packets, none         *)
(* necessarily by its send_packets, and the submit pass finishes its       *)
(* staged slots.  Under it the fill phase always has room for an update's  *)
(* sends (NeverRefusedUnderLateReap).  With LateReap = FALSE any           *)
(* completion may wait any number of calls and a submit pass may stop      *)
(* part way; the staging is still safe (NoWriteWhileKernelOwned,           *)
(* PendingMatches).                                                        *)
(*                                                                         *)
(* One constant and three rule switches carry the reversed witnesses.      *)
(* Phases = 1 is the staging before the phases: its index stays while a   *)
(* send is pending, so a late reap makes the next update's start_packet    *)
(* refuse (NeverRefusedUnderLateReap).  BaseRule = FALSE drops the phase   *)
(* base from start_packet's slot, so the second phase hands out the first  *)
(* phase's slots while the kernel owns them (NoWriteWhileKernelOwned).     *)
(* QuotientRule = FALSE takes a reaped send's phase as the remainder of    *)
(* its slot, so the pending count of the wrong phase falls                 *)
(* (PendingMatches).  CountRule = FALSE makes the submit count the run's    *)
(* slots while the reap retires one send, so a multi-slot run breaks       *)
(* PendingMatches.                                                          *)
(***************************************************************************)
EXTENDS Naturals, Sequences, FiniteSets

CONSTANTS Phases,       \* send phases (the code: ROCKETNET_URING_SEND_PHASES)
          Size,         \* slots per phase (ROCKETNET_URING_SEND_QUEUE_SIZE)
          MaxSegments,  \* slots one send run may retire (ROCKETNET_URING_MAX_SEGMENTS)
          MaxUpdates,   \* bound on updates
          LateReap,     \* every send's completion is reaped by the next receive call
          BaseRule,     \* start_packet adds the phase base to the index
          QuotientRule, \* a reaped send's phase is its head slot's quotient by Size
          CountRule     \* submit counts one send (TRUE) or the run's slots (FALSE); reap retires one

ASSUME Phases \in Nat \ {0} /\ Size \in Nat \ {0} /\ MaxUpdates \in Nat
ASSUME MaxSegments \in Nat \ {0}
ASSUME LateReap \in BOOLEAN /\ BaseRule \in BOOLEAN /\ QuotientRule \in BOOLEAN
ASSUME CountRule \in BOOLEAN

Ph    == 0..(Phases - 1)
Slots == 0..(Phases * Size - 1)

\* the staging slot of index i of phase p, as start_packet computes it
Base(p) == IF BaseRule THEN p * Size ELSE 0
SlotOf(p, i) == Base(p) + i

\* the phase of a reaped send's head staging slot, as the CQE handler computes it
PhaseOf(g) == IF QuotientRule THEN g \div Size ELSE g % Size
\* the phase a send's head slot really belongs to
SendPhase(g) == g \div Size

Range(s) == {s[i] : i \in 1..Len(s)}

VARIABLES fill,       \* send.fill: the phase start_packet reserves from
          num,        \* send.num_packets[p]: each phase's reservation index
          submitted,  \* send_submitted[p]: indices below this were submitted or skipped
          pendC,      \* send_pending[p]: its sends in flight, not its slots
          unposted,   \* send heads submitted whose completion the kernel has not posted
          cq,         \* posted send completions (their head slots), oldest first
          runLen,     \* each in-flight run head's slots, by its head staging slot
          written,    \* staging slots the fill threads wrote since the last submit
          step,       \* the update's step: fill, send (after the submit) or recv
          upd,        \* updates begun
          demand,     \* start_packet calls this update
          refused     \* a start_packet returned NULL

vars == <<fill, num, submitted, pendC, unposted, cq, runLen, written, step, upd, demand, refused>>

\* the slots of the run a send head names
RunOf(g) == IF runLen[g] = 0 THEN {} ELSE g..(g + runLen[g] - 1)

\* the slots the kernel owns: every slot of every send not yet reaped
Kern == UNION {RunOf(g) : g \in (unposted \cup Range(cq))}

Init ==
    /\ fill = 0
    /\ num = [p \in Ph |-> 0]
    /\ submitted = [p \in Ph |-> 0]
    /\ pendC = [p \in Ph |-> 0]
    /\ unposted = {}
    /\ cq = <<>>
    /\ runLen = [g \in Slots |-> 0]
    /\ written = {}
    /\ step = "fill"
    /\ upd = 0
    /\ demand = 0
    /\ refused = FALSE

\* start_packet: one fetch_add on the fill phase's index; past Size it
\* returns NULL. An update asks for at most Size sends.
Reserve ==
    /\ step = "fill" /\ upd < MaxUpdates
    /\ demand < Size
    /\ demand' = demand + 1
    /\ IF num[fill] < Size
       THEN /\ num' = [num EXCEPT ![fill] = @ + 1]
            /\ written' = written \cup {SlotOf(fill, num[fill])}
            /\ UNCHANGED refused
       ELSE /\ refused' = TRUE
            /\ UNCHANGED <<num, written>>
    /\ UNCHANGED <<fill, submitted, pendC, unposted, cq, runLen, step, upd>>

\* send_packets, fill threads joined: the call begins.  The staged slots
\* not yet submitted are handed to the kernel by SubmitRun and SkipSlot
\* below, in runs, and the call may end before all of them are
SendBegin ==
    /\ step = "fill" /\ upd < MaxUpdates
    /\ written' = {}
    /\ step' = "send"
    /\ UNCHANGED <<fill, num, submitted, pendC, unposted, cq, runLen, upd, demand, refused>>

\* one send run of phase p, from its next unsubmitted index: at most a
\* phase's remaining slots, and at most MaxSegments slots.  The head names
\* the send; the kernel owns every slot of the run until its one CQE
SubmitRun ==
    /\ step = "send"
    /\ \E p \in Ph :
         /\ submitted[p] < num[p]
         /\ \E L \in 1..MaxSegments :
              LET i == submitted[p]
              IN /\ L <= num[p] - i
                 /\ submitted' = [submitted EXCEPT ![p] = @ + L]
                 /\ unposted' = unposted \cup {SlotOf(p, i)}
                 /\ pendC' = [pendC EXCEPT ![p] = @ + (IF CountRule THEN 1 ELSE L)]
                 /\ runLen' = [runLen EXCEPT ![SlotOf(p, i)] = L]
                 /\ UNCHANGED <<fill, num, cq, written, step, upd, demand, refused>>

\* an aborted or unroutable staged slot: submit_packets copies no run from
\* it, so it is dropped (counted) and the submission moves past it
SkipSlot ==
    /\ step = "send"
    /\ \E p \in Ph :
         /\ submitted[p] < num[p]
         /\ submitted' = [submitted EXCEPT ![p] = @ + 1]
         /\ UNCHANGED <<fill, num, pendC, unposted, cq, runLen, written, step, upd, demand, refused>>

\* the kernel posts a send's completion (environment action)
KernelComplete(g) ==
    /\ g \in unposted
    /\ unposted' = unposted \ {g}
    /\ cq' = Append(cq, g)
    /\ UNCHANGED <<fill, num, submitted, pendC, runLen, written, step, upd, demand, refused>>

\* a reap takes the oldest completion, in send_packets or in
\* receive_packets, and lowers its phase's pending count by one send
Reap ==
    /\ step \in {"send", "recv"}
    /\ cq # <<>>
    /\ PhaseOf(Head(cq)) \in Ph
    /\ pendC[PhaseOf(Head(cq))] > 0
    /\ LET g == Head(cq)
       IN /\ pendC' = [pendC EXCEPT ![PhaseOf(g)] = @ - 1]
          /\ cq' = Tail(cq)
          /\ runLen' = [runLen EXCEPT ![g] = 0]
    /\ UNCHANGED <<fill, num, submitted, unposted, written, step, upd, demand, refused>>

\* the end of send_packets: every complete phase reset; then, when the fill
\* phase still holds staged slots and the next phase is empty, the fill
\* moves to it and the old fill phase is committed.  Under LateReap the
\* submit pass does not stop part way
Complete(p) == pendC[p] = 0 /\ submitted[p] >= num[p]
Next1 == (fill + 1) % Phases

SendEnd ==
    /\ step = "send"
    /\ ( ~LateReap \/ \A p \in Ph : submitted[p] >= num[p] )
    /\ LET n == [p \in Ph |-> IF Complete(p) THEN 0 ELSE num[p]]
       IN /\ num' = n
          /\ submitted' = [p \in Ph |-> IF Complete(p) THEN 0 ELSE submitted[p]]
          /\ fill' = IF n[fill] # 0 /\ n[Next1] = 0 THEN Next1 ELSE fill
    /\ step' = "recv"
    /\ UNCHANGED <<pendC, unposted, cq, runLen, written, upd, demand, refused>>

\* the end of receive_packets and of the update: under LateReap every send
\* submitted is complete and reaped by now
RecvEnd ==
    /\ step = "recv"
    /\ LateReap => Kern = {}
    /\ step' = "fill"
    /\ upd' = upd + 1
    /\ demand' = 0
    /\ UNCHANGED <<fill, num, submitted, pendC, unposted, cq, runLen, written, refused>>

\* the bound on updates is reached; the run ends here
Done ==
    /\ step = "fill" /\ upd = MaxUpdates
    /\ UNCHANGED vars

Next ==
    \/ Reserve
    \/ SendBegin
    \/ SubmitRun
    \/ SkipSlot
    \/ \E g \in Slots : KernelComplete(g)
    \/ Reap
    \/ SendEnd
    \/ RecvEnd
    \/ Done

Spec == Init /\ [][Next]_vars

-----------------------------------------------------------------------------

TypeOK ==
    /\ fill \in Ph
    /\ num \in [Ph -> 0..Size]
    /\ submitted \in [Ph -> 0..Size]
    /\ pendC \in [Ph -> Nat]
    /\ unposted \subseteq Slots
    /\ Range(cq) \subseteq Slots
    /\ runLen \in [Slots -> 0..MaxSegments]
    /\ written \subseteq Nat
    /\ step \in {"fill", "send", "recv"}
    /\ upd \in 0..MaxUpdates
    /\ demand \in 0..Size
    /\ refused \in BOOLEAN

\* no fill thread writes a staging slot the kernel still owns
NoWriteWhileKernelOwned ==
    written \cap Kern = {}

\* each phase's pending count is the number of its sends the kernel owns
PendingMatches ==
    \A p \in Ph : pendC[p] = Cardinality({g \in (unposted \cup Range(cq)) : SendPhase(g) = p})

\* under a late reap, start_packet never runs out of slots: an update's
\* sends, at most a phase's worth, always fit the fill phase
NeverRefusedUnderLateReap ==
    ~refused

=============================================================================
