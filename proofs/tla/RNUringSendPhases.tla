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
(* NULL when the phase is full (Reserve).  send_packets submits every      *)
(* staged slot not yet submitted, the committed phase's first, each send   *)
(* naming its staging slot (SendBegin); the kernel posts each send's       *)
(* completion (KernelComplete) and a reap takes them oldest first from the *)
(* completion queue (Reap), the phase of a reaped send being its slot's    *)
(* quotient by Size; at its end send_packets resets every complete phase   *)
(* (every staged slot submitted, no send of it pending) and, when the      *)
(* fill phase still holds staged slots and the next phase is empty, moves  *)
(* the fill to it (SendEnd: the old fill phase is committed).              *)
(* receive_packets reaps too, so a send whose completion the send call's   *)
(* budget did not reach is reaped one call late (the Recv step).           *)
(*                                                                         *)
(* LateReap = TRUE is the flood the code meets: every completion of an     *)
(* update's sends is reaped by that update's receive_packets, none         *)
(* necessarily by its send_packets.  Under it the fill phase always has    *)
(* room for an update's sends (NeverRefusedUnderLateReap).  With           *)
(* LateReap = FALSE any completion may wait any number of calls; the       *)
(* staging is still safe (NoWriteWhileKernelOwned, PendingMatches).        *)
(*                                                                         *)
(* One constant and two rule switches carry the reversed witnesses.        *)
(* Phases = 1 is the staging before the phases: its index stays while a   *)
(* send is pending, so a late reap makes the next update's start_packet    *)
(* refuse (NeverRefusedUnderLateReap).  BaseRule = FALSE drops the phase   *)
(* base from start_packet's slot, so the second phase hands out the first  *)
(* phase's slots while the kernel owns them (NoWriteWhileKernelOwned).     *)
(* QuotientRule = FALSE takes a reaped send's phase as the remainder of    *)
(* its slot, so the pending count of the wrong phase falls                 *)
(* (PendingMatches).                                                       *)
(***************************************************************************)
EXTENDS Naturals, Sequences, FiniteSets

CONSTANTS Phases,       \* send phases (the code: ROCKETNET_URING_SEND_PHASES)
          Size,         \* slots per phase (ROCKETNET_URING_SEND_QUEUE_SIZE)
          MaxUpdates,   \* bound on updates
          LateReap,     \* every send's completion is reaped by the next receive call
          BaseRule,     \* start_packet adds the phase base to the index
          QuotientRule  \* a reaped send's phase is its slot's quotient by Size

ASSUME Phases \in Nat \ {0} /\ Size \in Nat \ {0} /\ MaxUpdates \in Nat
ASSUME LateReap \in BOOLEAN /\ BaseRule \in BOOLEAN /\ QuotientRule \in BOOLEAN

Ph    == 0..(Phases - 1)
Slots == 0..(Phases * Size - 1)

\* the staging slot of index i of phase p, as start_packet computes it
Base(p) == IF BaseRule THEN p * Size ELSE 0
SlotOf(p, i) == Base(p) + i

\* the phase of a reaped send's staging slot, as the CQE handler computes it
PhaseOf(g) == IF QuotientRule THEN g \div Size ELSE g % Size

Range(s) == {s[i] : i \in 1..Len(s)}

VARIABLES fill,       \* send.fill: the phase start_packet reserves from
          num,        \* send.num_packets[p]: each phase's reservation index
          submitted,  \* send_submitted[p]
          pendC,      \* send_pending[p]
          unposted,   \* sends submitted whose completion the kernel has not posted
          cq,         \* posted send completions, oldest first
          written,    \* staging slots the fill threads wrote since the last submit
          step,       \* the update's step: fill, send (after the submit) or recv
          upd,        \* updates begun
          demand,     \* start_packet calls this update
          refused     \* a start_packet returned NULL

vars == <<fill, num, submitted, pendC, unposted, cq, written, step, upd, demand, refused>>

\* the slots the kernel owns: submitted, completion not yet reaped
Kern == unposted \cup Range(cq)

Init ==
    /\ fill = 0
    /\ num = [p \in Ph |-> 0]
    /\ submitted = [p \in Ph |-> 0]
    /\ pendC = [p \in Ph |-> 0]
    /\ unposted = {}
    /\ cq = <<>>
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
    /\ UNCHANGED <<fill, submitted, pendC, unposted, cq, step, upd>>

\* send_packets, fill threads joined: every staged slot not yet submitted,
\* the committed phases' and the fill phase's, one send each, naming its
\* staging slot Base(p) + i with the base the submit loop adds
NewSends(p) == {p * Size + i : i \in submitted[p]..(num[p] - 1)}

SendBegin ==
    /\ step = "fill" /\ upd < MaxUpdates
    /\ unposted' = unposted \cup UNION {NewSends(p) : p \in Ph}
    /\ pendC' = [p \in Ph |-> pendC[p] + (num[p] - submitted[p])]
    /\ submitted' = num
    /\ written' = {}
    /\ step' = "send"
    /\ UNCHANGED <<fill, num, cq, upd, demand, refused>>

\* the kernel posts a send's completion (environment action)
KernelComplete(g) ==
    /\ g \in unposted
    /\ unposted' = unposted \ {g}
    /\ cq' = Append(cq, g)
    /\ UNCHANGED <<fill, num, submitted, pendC, written, step, upd, demand, refused>>

\* a reap takes the oldest completion, in send_packets or in
\* receive_packets, and lowers its phase's pending count
Reap ==
    /\ step \in {"send", "recv"}
    /\ cq # <<>>
    /\ PhaseOf(Head(cq)) \in Ph
    /\ pendC[PhaseOf(Head(cq))] > 0
    /\ pendC' = [pendC EXCEPT ![PhaseOf(Head(cq))] = @ - 1]
    /\ cq' = Tail(cq)
    /\ UNCHANGED <<fill, num, submitted, unposted, written, step, upd, demand, refused>>

\* the end of send_packets: every complete phase reset; then, when the fill
\* phase still holds staged slots and the next phase is empty, the fill
\* moves to it and the old fill phase is committed
Complete(p) == pendC[p] = 0 /\ submitted[p] >= num[p]
Next1 == (fill + 1) % Phases

SendEnd ==
    /\ step = "send"
    /\ LET n == [p \in Ph |-> IF Complete(p) THEN 0 ELSE num[p]]
       IN /\ num' = n
          /\ submitted' = [p \in Ph |-> IF Complete(p) THEN 0 ELSE submitted[p]]
          /\ fill' = IF n[fill] # 0 /\ n[Next1] = 0 THEN Next1 ELSE fill
    /\ step' = "recv"
    /\ UNCHANGED <<pendC, unposted, cq, written, upd, demand, refused>>

\* the end of receive_packets and of the update: under LateReap every send
\* submitted is complete and reaped by now
RecvEnd ==
    /\ step = "recv"
    /\ LateReap => Kern = {}
    /\ step' = "fill"
    /\ upd' = upd + 1
    /\ demand' = 0
    /\ UNCHANGED <<fill, num, submitted, pendC, unposted, cq, written, refused>>

\* the bound on updates is reached; the run ends here
Done ==
    /\ step = "fill" /\ upd = MaxUpdates
    /\ UNCHANGED vars

Next ==
    \/ Reserve
    \/ SendBegin
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
    \A p \in Ph : pendC[p] = Cardinality({g \in Kern : g \div Size = p})

\* under a late reap, start_packet never runs out of slots: an update's
\* sends, at most a phase's worth, always fit the fill phase
NeverRefusedUnderLateReap ==
    ~refused

=============================================================================
