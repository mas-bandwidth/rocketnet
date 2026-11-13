---------------------------- MODULE RNSegmentRefusal ----------------------------
(***************************************************************************)
(* UDP segmentation refusal on the server's Linux transports, per         *)
(* destination (sdk/rocketnet_server_gso.h rocketnet_segment_refused_t;    *)
(* sdk/rocketnet_server_gso_linux.cpp rocketnet_server_gso_flush;          *)
(* sdk/rocketnet_server_uring_linux.cpp rocketnet_server_uring_reap_cqe     *)
(* and rocketnet_server_uring_submit_phase).                               *)
(*                                                                         *)
(* Each step is one send run of more than one packet to one destination    *)
(* (Run).  A run leaves segmented while segmentation is on for the         *)
(* transport and its destination has not refused; a destination whose     *)
(* route cannot take a segmented send (Refusing, an environment fact: an   *)
(* IPsec policy, say), or every destination when the kernel cannot segment *)
(* (KernelSegments = FALSE), refuses it (Refuse).  The refused run is      *)
(* resent plain and its destination remembered, so its later runs leave    *)
(* plain.  Segmentation turns off for the transport only when Threshold    *)
(* destinations refused and no segmented send was ever accepted.  A        *)
(* destination's own plain run, and a run of one packet, change nothing    *)
(* and are not modelled.                                                   *)
(*                                                                         *)
(* Per-client scope (TRANSPORT.md t2.4 rule 7): a run that a destination  *)
(* which accepts segmentation sends plain, after the transport has shown   *)
(* it can segment, is another destination's condition costing it          *)
(* (OtherDestinationUnaffected); a refused run is never dropped            *)
(* (NothingDropped); each destination pays at most one refusal            *)
(* (RefusedOnce, which holds when no two destinations share an entry).    *)
(* The remembered set is the code's direct-mapped table at its layout:    *)
(* Slot names each destination's entry, a refusing destination takes its  *)
(* entry and displaces the one there, which pays one refusal again on its *)
(* next run; a destination that never refused is never in the table,      *)
(* because the table matches whole addresses.  The fallback counts every  *)
(* refusal of a destination the table did not hold, saturating at          *)
(* Threshold, so a displaced destination's second refusal counts and the   *)
(* count is never below the number of distinct refusing destinations      *)
(* (KernelFallback).  Mode "fillcount" is the code before: it counted only *)
(* a refusal that filled an empty entry, so with two refusers sharing an   *)
(* entry the fallback never fired.                                         *)
(*                                                                         *)
(* Mode carries the reversed witnesses, each the code before this rule.    *)
(* "transport" is the io_uring transport's: the first refusal drops the    *)
(* run and turns segmentation off for every destination.  "consecutive"    *)
(* is the gso transport's: the run is resent plain and nothing is          *)
(* remembered, and Threshold refusals with no accepted segmented send      *)
(* between them turn segmentation off for every destination.               *)
(***************************************************************************)
EXTENDS Naturals, FiniteSets

CONSTANTS Dests,            \* the destinations
          Refusing,         \* the destinations whose route refuses a segmented send
          KernelSegments,   \* FALSE: the kernel refuses every segmented send
          Threshold,        \* ROCKETNET_SEGMENT_CONSECUTIVE_REFUSAL_THRESHOLD
          MaxRuns,          \* bound on runs
          Mode,             \* "perdest" (the code), "fillcount", "transport" or "consecutive"
          Slot              \* each destination's entry (rocketnet_segment_refused_entry)

ASSUME Refusing \subseteq Dests
ASSUME KernelSegments \in BOOLEAN
ASSUME Threshold \in Nat \ {0}
ASSUME MaxRuns \in Nat
ASSUME Mode \in {"perdest", "fillcount", "transport", "consecutive"}
ASSUME Slot \in [Dests -> Nat]

NoDest == "none"
Entries == {Slot[d] : d \in Dests}

VARIABLES segOn,        \* the transport's FEATURE_SEGMENT bit
          table,        \* the table: each entry's destination, or NoDest
          count,        \* segment_refused.destinations
          everRefused,  \* destinations that ever refused (not code state: the property's measure)
          accepted,     \* a segmented send was accepted since create
          streak,       \* refusals since the last accepted segmented send ("consecutive")
          refusals,     \* refusals each destination paid
          wronglyPlain, \* destinations that accept segmentation and sent a run plain after one was accepted
          dropped,      \* a refused run was dropped
          runs          \* runs sent

vars == <<segOn, table, count, everRefused, accepted, streak, refusals, wronglyPlain, dropped, runs>>

Refuses(d) == ~KernelSegments \/ d \in Refusing

\* rocketnet_segment_refused_contains: the entry holds this destination
Remembered(d) == table[Slot[d]] = d

Segmented(d) == segOn /\ ~Remembered(d)

Init ==
    /\ segOn = TRUE
    /\ table = [e \in Entries |-> NoDest]
    /\ count = 0
    /\ everRefused = {}
    /\ accepted = FALSE
    /\ streak = 0
    /\ refusals = [d \in Dests |-> 0]
    /\ wronglyPlain = {}
    /\ dropped = FALSE
    /\ runs = 0

\* a run to d leaves segmented and is accepted, or leaves plain
Run(d) ==
    /\ runs < MaxRuns
    /\ runs' = runs + 1
    /\ ~(Segmented(d) /\ Refuses(d))
    /\ IF Segmented(d)
       THEN /\ accepted' = TRUE
            /\ streak' = 0
            /\ UNCHANGED wronglyPlain
       ELSE /\ wronglyPlain' = IF accepted /\ ~Refuses(d) THEN wronglyPlain \cup {d} ELSE wronglyPlain
            /\ UNCHANGED <<accepted, streak>>
    /\ UNCHANGED <<segOn, table, count, everRefused, refusals, dropped>>

\* rocketnet_segment_refused_add: every refusal of a destination the table
\* did not hold counts, saturating at Threshold; "fillcount" counted only a
\* refusal that filled an empty entry
Counted(d) ==
    IF Mode = "fillcount"
    THEN IF table[Slot[d]] = NoDest THEN count + 1 ELSE count
    ELSE IF count < Threshold THEN count + 1 ELSE count

\* a segmented run to d is refused (the kernel's EINVAL, EIO or EOPNOTSUPP)
Refuse(d) ==
    /\ runs < MaxRuns
    /\ runs' = runs + 1
    /\ Segmented(d) /\ Refuses(d)
    /\ refusals' = [refusals EXCEPT ![d] = @ + 1]
    /\ everRefused' = everRefused \cup {d}
    /\ CASE Mode \in {"perdest", "fillcount"} ->
              /\ table' = [table EXCEPT ![Slot[d]] = d]
              /\ count' = Counted(d)
              /\ segOn' = ~(~accepted /\ Counted(d) >= Threshold)
              /\ UNCHANGED <<streak, dropped>>
         [] Mode = "transport" ->
              /\ segOn' = FALSE
              /\ dropped' = TRUE
              /\ UNCHANGED <<table, count, streak>>
         [] Mode = "consecutive" ->
              /\ streak' = streak + 1
              /\ segOn' = (streak + 1 < Threshold)
              /\ UNCHANGED <<table, count, dropped>>
    /\ UNCHANGED <<accepted, wronglyPlain>>

\* the bound on runs is reached
Done ==
    /\ runs = MaxRuns
    /\ UNCHANGED vars

Next ==
    \/ \E d \in Dests : Run(d)
    \/ \E d \in Dests : Refuse(d)
    \/ Done

Spec == Init /\ [][Next]_vars

-----------------------------------------------------------------------------

TypeOK ==
    /\ segOn \in BOOLEAN
    /\ table \in [Entries -> Dests \cup {NoDest}]
    /\ count \in 0..MaxRuns
    /\ everRefused \subseteq Dests
    /\ accepted \in BOOLEAN
    /\ streak \in 0..MaxRuns
    /\ refusals \in [Dests -> 0..MaxRuns]
    /\ wronglyPlain \subseteq Dests
    /\ dropped \in BOOLEAN
    /\ runs \in 0..MaxRuns

\* once the transport has shown it can segment, no destination that accepts
\* segmentation sends a run plain: another destination's refusal costs it
\* nothing
OtherDestinationUnaffected ==
    wronglyPlain = {}

\* a refused run is resent plain, never dropped
NothingDropped ==
    ~dropped

\* a refusing destination pays one refusal, not one a run
RefusedOnce ==
    \A d \in Dests : refusals[d] <= 1

\* a kernel that cannot segment turns segmentation off once Threshold
\* distinct destinations refused, however they share entries
KernelFallback ==
    (Mode \in {"perdest", "fillcount"} /\ ~KernelSegments /\ Cardinality(everRefused) >= Threshold) => ~segOn

=============================================================================
