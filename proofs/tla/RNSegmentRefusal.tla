---------------------------- MODULE RNSegmentRefusal ----------------------------
(***************************************************************************)
(* UDP segmentation refusal on the server's segmenting transports, per    *)
(* destination (sdk/rocketnet_segment_refused.h                            *)
(* rocketnet_segment_refused_t; sdk/rocketnet_server_gso_linux.cpp         *)
(* rocketnet_server_gso_flush; sdk/rocketnet_server_uring_linux.cpp        *)
(* rocketnet_server_uring_reap_cqe and rocketnet_server_uring_submit_phase; *)
(* sdk/rocketnet_server_iocp_windows.cpp rocketnet_iocp_send_bank and      *)
(* rocketnet_iocp_post_send_run, Windows USO).                             *)
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
(* (RefusedOnce).  The code's remembered set is a direct-mapped table: a   *)
(* refusing destination displaced by another pays one refusal again, which *)
(* changes only RefusedOnce and is not modelled; a destination that never  *)
(* refused is never in the table, because the table matches whole          *)
(* addresses.                                                              *)
(*                                                                         *)
(* Mode carries the reversed witnesses, each the code before this rule.    *)
(* "transport" is the io_uring transport's: the first refusal drops the    *)
(* run and turns segmentation off for every destination.  "consecutive"    *)
(* is the gso transport's: the run is resent plain and nothing is          *)
(* remembered, and Threshold refusals with no accepted segmented send      *)
(* between them turn segmentation off for every destination.  "first" is   *)
(* the iocp transport's before v1.1.8: the run is resent plain, nothing is *)
(* remembered, and the first refusal turns USO off for every destination.  *)
(***************************************************************************)
EXTENDS Naturals, FiniteSets

CONSTANTS Dests,            \* the destinations
          Refusing,         \* the destinations whose route refuses a segmented send
          KernelSegments,   \* FALSE: the kernel refuses every segmented send
          Threshold,        \* ROCKETNET_SEGMENT_CONSECUTIVE_REFUSAL_THRESHOLD
          MaxRuns,          \* bound on runs
          Mode              \* "perdest" (the code), "transport", "consecutive" or "first"

ASSUME Refusing \subseteq Dests
ASSUME KernelSegments \in BOOLEAN
ASSUME Threshold \in Nat \ {0}
ASSUME MaxRuns \in Nat
ASSUME Mode \in {"perdest", "transport", "consecutive", "first"}

VARIABLES segOn,        \* the transport's FEATURE_SEGMENT bit
          remembered,   \* destinations that refused (the table)
          accepted,     \* a segmented send was accepted since create
          streak,       \* refusals since the last accepted segmented send ("consecutive")
          refusals,     \* refusals each destination paid
          wronglyPlain, \* destinations that accept segmentation and sent a run plain after one was accepted
          dropped,      \* a refused run was dropped
          runs          \* runs sent

vars == <<segOn, remembered, accepted, streak, refusals, wronglyPlain, dropped, runs>>

Refuses(d) == ~KernelSegments \/ d \in Refusing

Segmented(d) == segOn /\ d \notin remembered

Init ==
    /\ segOn = TRUE
    /\ remembered = {}
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
    /\ UNCHANGED <<segOn, remembered, refusals, dropped>>

\* a segmented run to d is refused (the kernel's EINVAL, EIO or EOPNOTSUPP)
Refuse(d) ==
    /\ runs < MaxRuns
    /\ runs' = runs + 1
    /\ Segmented(d) /\ Refuses(d)
    /\ refusals' = [refusals EXCEPT ![d] = @ + 1]
    /\ CASE Mode = "perdest" ->
              /\ remembered' = remembered \cup {d}
              /\ segOn' = ~(~accepted /\ Cardinality(remembered \cup {d}) >= Threshold)
              /\ UNCHANGED <<streak, dropped>>
         [] Mode = "transport" ->
              /\ segOn' = FALSE
              /\ dropped' = TRUE
              /\ UNCHANGED <<remembered, streak>>
         [] Mode = "consecutive" ->
              /\ streak' = streak + 1
              /\ segOn' = (streak + 1 < Threshold)
              /\ UNCHANGED <<remembered, dropped>>
         [] Mode = "first" ->
              /\ segOn' = FALSE
              /\ UNCHANGED <<remembered, streak, dropped>>
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
    /\ remembered \subseteq Dests
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
\* destinations refused
KernelFallback ==
    (Mode = "perdest" /\ ~KernelSegments /\ Cardinality(remembered) >= Threshold) => ~segOn

=============================================================================
