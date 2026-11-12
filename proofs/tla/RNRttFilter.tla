------------------------------ MODULE RNRttFilter ------------------------------
(***************************************************************************)
(* rocketnet: the round-trip filter of a connection with the reliable       *)
(* timeout off (standard/MESSAGES.md m9.2; the code is                      *)
(* rn_window_ack_on_acks and rn_window_ack_on_ack, with                     *)
(* rn_window_ack_record_first_send and rn_window_ack_refresh_oldest, in     *)
(* sdk/rocketnet_window_ack.cpp).                                           *)
(*                                                                          *)
(* The code's state, as the code keeps it: the estimate srtt (has_rtt), the *)
(* last update that acknowledged a packet (last_ack, has_last_ack), the end *)
(* of the latest acknowledgement silence (silence_end), and the oldest      *)
(* unacknowledged number's first send (has_oldest, oldest_number,           *)
(* oldest_first_send).  Times are kept as ages in ticks, saturating at Cap, *)
(* past every comparison the model makes.                                   *)
(*                                                                          *)
(* The sender: reliable numbers U..next-1, each sent when numbered (Send),  *)
(* resent at any time (Resend; the resend timing is RNResendTiming), at     *)
(* most M on the wire; packets in flight, at most P, each with its send age *)
(* and the path's round trip when it was sent; each packet carries one      *)
(* number.  The path: a round trip of RttLow ticks that may slow once to    *)
(* RttHigh (both under Probe: the guarantee is stated for round trips under *)
(* one second), outages of up to OutageMax ticks in which nothing is        *)
(* delivered, and loss.  A packet's acknowledgement arrives in the update   *)
(* its round trip ends, unless the path is dark then, when it arrives in the *)
(* first update after the outage, or never (lost).  After Good the path    *)
(* neither loses nor goes dark (the liveness instance).                     *)
(*                                                                          *)
(* An acknowledged packet (Ack) is the code's two steps in order:           *)
(* rn_window_ack_on_acks ends a silence and moves last_ack, then            *)
(* rn_window_ack_on_ack samples (or refuses) and marks the number acked,    *)
(* refreshing the oldest when U moves.  The estimate moves toward each      *)
(* sample taken: strictly, by any amount up to the sample (the direction    *)
(* of srtt = 0.95 srtt + 0.05 rtt; its size is abstract); the first sample  *)
(* sets it.                                                                 *)
(*                                                                          *)
(* What is checked, against definitions that do not read the code's state: *)
(* owedQuiet counts the ticks in which some number was on the wire          *)
(* unacknowledged and no packet was acknowledged (from the sender's own     *)
(* numbers and the acks that arrived), and realEndAge is the age of the     *)
(* last ack that ended such a run of Probe ticks: a real owed silence.      *)
(*   NoSampleAcrossSilence: no sample from a packet sent before the end of  *)
(*     the latest real owed silence less srtt (an ack that waited out an    *)
(*     outage measures the outage).                                         *)
(*   RefuseOnlyAfterOwedSilence: a sample is refused only for such a packet. *)
(*   OutageNeverPoisons: the estimate stays under RttHigh + Probe: an       *)
(*     outage of a second or more never reaches it.                         *)
(*   SlowPathRaisesEstimate: after the path slows (and the bad intervals    *)
(*     end), the estimate reaches the new round trip, for a sparse sender   *)
(*     too.                                                                 *)
(* Rule "owed" is the code (v1.1.1); the reversed witnesses: "gap" (v1.1.0: *)
(* any Probe ticks with no ack is a silence, owed or not) never raises a    *)
(* sparse sender's estimate and refuses samples no outage delayed; "none"   *)
(* (no filter) takes the outage's samples and the estimate is poisoned.     *)
(***************************************************************************)
EXTENDS Integers, Sequences, FiniteSets

CONSTANTS
    Probe,      \* RN_PROBE_SECONDS in ticks
    RttLow,     \* the path's round trip before it slows, ticks
    RttHigh,    \* the path's round trip after it slows, ticks
    OutageMax,  \* the longest outage, ticks
    M,          \* reliable numbers on the wire at most
    P,          \* packets in flight at most
    Rule,       \* "owed" (the code), "gap" (v1.1.0) or "none" (no filter)
    FirstAck    \* TRUE: no update has acknowledged a packet yet (has_last_ack = 0),
                \* the estimator seeded (m9.2); FALSE: a sparse sender's previous ack

ASSUME /\ Probe \in Nat \ {0}
       /\ RttLow \in Nat /\ RttHigh \in Nat /\ RttLow <= RttHigh /\ RttHigh < Probe
       /\ OutageMax \in Nat
       /\ M \in Nat \ {0} /\ P \in Nat \ {0}
       /\ Rule \in {"owed", "gap", "none"}
       /\ FirstAck \in BOOLEAN

Cap == Probe + RttHigh + OutageMax + 1
SrttMax == RttHigh + OutageMax
Mod == M + 1            \* numbers modulo: U..next-1 never wraps onto itself

Min(a, b) == IF a < b THEN a ELSE b
Max(a, b) == IF a > b THEN a ELSE b
Age(a) == Min(a + 1, Cap)

VARIABLES
    \* the code's state (rn_window_ack_t)
    srtt, hasRtt,
    lastAckAge, hasLastAck,
    silenceEndAge,
    hasOldest, oldestNum, oldestFirstAge,
    \* the sender: numbers U..next-1 (all on the wire), packets in flight
    ubase,      \* U modulo Mod
    nums,       \* << [first |-> age of the first send, acked |-> BOOLEAN] >> for U..next-1
    pkts,       \* [1..P -> packet]: used, age, prtt, num (modulo Mod; -1 once its number left)
    \* the path
    rttNow, dark, darkLeft, good,
    \* the definitions the properties read
    owedQuiet,  \* ticks with a number on the wire, unacknowledged, and no ack (to Probe)
    realEndAge, \* age of the last ack that ended a run of Probe such ticks
    badSample, badRefusal

codeVars == <<srtt, hasRtt, lastAckAge, hasLastAck, silenceEndAge, hasOldest, oldestNum, oldestFirstAge>>
sendVars == <<ubase, nums, pkts>>
pathVars == <<rttNow, dark, darkLeft, good>>
ghostVars == <<owedQuiet, realEndAge, badSample, badRefusal>>
vars == <<codeVars, sendVars, pathVars, ghostVars>>

U == ubase
NumAt(i) == (ubase + i - 1) % Mod      \* the number at position i of nums
Slots == 1..P
NoPkt == [used |-> FALSE, age |-> 0, prtt |-> 0, num |-> -1]
InFlight == {s \in Slots : pkts[s].used}
Free == Slots \ InFlight
\* a packet goes in the lowest free slot
Put(p) == [pkts EXCEPT ![CHOOSE s \in Free : \A o \in Free : s <= o] = p]

Init ==
    /\ srtt = RttLow /\ hasRtt = TRUE
    /\ hasLastAck = ~FirstAck
    /\ lastAckAge \in (IF FirstAck THEN {Cap} ELSE 0..Probe)
    /\ silenceEndAge = Cap
    /\ hasOldest = FALSE /\ oldestNum = 0 /\ oldestFirstAge = Cap
    /\ ubase = 0 /\ nums = << >> /\ pkts = [s \in Slots |-> NoPkt]
    /\ rttNow = RttLow /\ dark = FALSE /\ darkLeft = 0 /\ good = FALSE
    /\ owedQuiet = 0 /\ realEndAge = Cap
    /\ badSample = FALSE /\ badRefusal = FALSE

\* A new number, sent at once (rn_window_ack_record_first_send: when it is U
\* and the oldest is not tracked at it, the oldest is now this send).
Send ==
    /\ Len(nums) < M
    /\ Free # {}
    /\ LET n == NumAt(Len(nums) + 1) IN
       /\ nums' = Append(nums, [first |-> 0, acked |-> FALSE])
       /\ pkts' = Put([used |-> TRUE, age |-> 0, prtt |-> rttNow, num |-> n])
       /\ IF n = U /\ (~hasOldest \/ oldestNum # n)
          THEN /\ hasOldest' = TRUE /\ oldestNum' = n /\ oldestFirstAge' = 0
          ELSE UNCHANGED <<hasOldest, oldestNum, oldestFirstAge>>
    /\ UNCHANGED <<srtt, hasRtt, lastAckAge, hasLastAck, silenceEndAge, ubase, pathVars, ghostVars>>

\* A number on the wire, unacknowledged, sent again (first send unchanged).
Resend(i) ==
    /\ i \in 1..Len(nums)
    /\ ~nums[i].acked
    /\ Free # {}
    /\ LET p == [used |-> TRUE, age |-> 0, prtt |-> rttNow, num |-> NumAt(i)] IN
       /\ \A s \in InFlight : pkts[s] # p
       /\ pkts' = Put(p)
    /\ UNCHANGED <<codeVars, ubase, nums, pathVars, ghostVars>>

\* The path loses a packet (or its acknowledgement): it is never acked.
Lose(s) ==
    /\ ~good
    /\ s \in InFlight
    /\ pkts' = [pkts EXCEPT ![s] = NoPkt]
    /\ UNCHANGED <<codeVars, ubase, nums, pathVars, ghostVars>>

\* The estimate after a sample taken: the first sets it; then it moves
\* strictly toward the sample, by any amount up to it.
Toward(s, r) == IF r > s THEN (s + 1)..r ELSE IF r < s THEN r..(s - 1) ELSE {s}

\* rn_window_ack_on_acks: the silence test, before this update's acks.
\* "owed": has_last_ack, has_oldest, U < next, and now - max(last_ack,
\* oldest_first_send) >= RN_PROBE_SECONDS.  "gap": has_last_ack and
\* now - last_ack >= RN_PROBE_SECONDS.  "none": never (no filter).
EndsSilence ==
    CASE Rule = "owed" -> hasLastAck /\ hasOldest /\ Len(nums) > 0
                          /\ Min(lastAckAge, oldestFirstAge) >= Probe
      [] Rule = "gap"  -> hasLastAck /\ lastAckAge >= Probe
      [] OTHER         -> FALSE

\* The acknowledgement of the packet in slot s arrives
\* (rn_window_ack_on_acks, then rn_window_ack_on_ack for it).  A second ack
\* in the same update finds last_ack = now (age 0), as the batch does.  A
\* packet whose number already left the window still gives its sample (its
\* sent-map entry is still there) and acknowledges nothing.
Ack(s) ==
    /\ s \in InFlight
    /\ ~dark
    /\ pkts[s].age >= pkts[s].prtt
    /\ LET p      == pkts[s]
           sEnd   == IF EndsSilence THEN 0 ELSE silenceEndAge
           \* the filter: sent before silence_end - srtt
           stale  == p.age > sEnd + srtt
           take   == ~hasRtt \/ ~stale
           \* the definitions: a real owed silence ends with this ack
           rEnd   == IF owedQuiet >= Probe THEN 0 ELSE realEndAge
           across == p.age > rEnd + srtt
           pos    == CHOOSE i \in 0..Len(nums) :
                        \/ i = 0 /\ \A j \in 1..Len(nums) : NumAt(j) # p.num
                        \/ i > 0 /\ NumAt(i) = p.num
           marked == IF pos > 0 THEN [nums EXCEPT ![pos].acked = TRUE] ELSE nums
           \* the acked prefix leaves the window: U moves past k numbers
           k      == CHOOSE j \in 0..Len(marked) :
                        /\ \A q \in 1..j : marked[q].acked
                        /\ (j = Len(marked) \/ ~marked[j + 1].acked)
           gone   == {NumAt(q) : q \in 1..k}
           rest   == SubSeq(marked, k + 1, Len(marked))
           ub     == (ubase + k) % Mod
       IN
       /\ silenceEndAge' = sEnd
       /\ lastAckAge' = 0
       /\ hasLastAck' = TRUE
       /\ IF ~hasRtt
          THEN srtt' = Min(p.age, SrttMax) /\ hasRtt' = TRUE
          ELSE IF take
               THEN srtt' \in Toward(srtt, Min(p.age, SrttMax)) /\ UNCHANGED hasRtt
               ELSE UNCHANGED <<srtt, hasRtt>>
       /\ nums' = rest
       /\ ubase' = ub
       \* rn_window_ack_refresh_oldest when U moved
       /\ IF k > 0
          THEN IF rest = << >>
               THEN hasOldest' = FALSE /\ oldestNum' = ub /\ oldestFirstAge' = Cap
               ELSE hasOldest' = TRUE /\ oldestNum' = ub /\ oldestFirstAge' = rest[1].first
          ELSE UNCHANGED <<hasOldest, oldestNum, oldestFirstAge>>
       /\ pkts' = [o \in Slots |->
                     IF o = s THEN NoPkt
                     ELSE IF pkts[o].used /\ pkts[o].num \in gone THEN [pkts[o] EXCEPT !.num = -1]
                     ELSE pkts[o]]
       /\ owedQuiet' = 0
       /\ realEndAge' = rEnd
       /\ badSample' = (badSample \/ (hasRtt /\ take /\ across))
       /\ badRefusal' = (badRefusal \/ (hasRtt /\ ~take /\ ~across))
    /\ UNCHANGED pathVars

\* The update ends and time moves on one tick.  While the path delivers,
\* every acknowledgement due has arrived (or was lost) first.  The path
\* may go dark (for up to OutageMax ticks), slow once, or turn good.
Tick ==
    /\ dark \/ \A s \in InFlight : pkts[s].age < pkts[s].prtt
    /\ pkts' = [s \in Slots |-> IF pkts[s].used THEN [pkts[s] EXCEPT !.age = Age(@)] ELSE NoPkt]
    /\ nums' = [i \in 1..Len(nums) |-> [nums[i] EXCEPT !.first = Age(@)]]
    /\ lastAckAge' = Age(lastAckAge)
    /\ silenceEndAge' = Age(silenceEndAge)
    /\ oldestFirstAge' = Age(oldestFirstAge)
    /\ realEndAge' = Age(realEndAge)
    /\ owedQuiet' = IF nums # << >> THEN Min(owedQuiet + 1, Probe) ELSE 0
    /\ rttNow' \in (IF rttNow = RttLow THEN {RttLow, RttHigh} ELSE {RttHigh})
    /\ IF dark /\ darkLeft > 1
       THEN dark' = TRUE /\ darkLeft' = darkLeft - 1 /\ good' = good
       ELSE \/ dark' = FALSE /\ darkLeft' = 0 /\ good' \in (IF good THEN {TRUE} ELSE BOOLEAN)
            \/ ~good /\ OutageMax > 0 /\ dark' = TRUE /\ darkLeft' \in 1..OutageMax /\ good' = FALSE
    /\ UNCHANGED <<srtt, hasRtt, hasLastAck, hasOldest, oldestNum, ubase, badSample, badRefusal>>

Next ==
    \/ Send
    \/ \E i \in 1..M : Resend(i)
    \/ \E s \in Slots : Lose(s)
    \/ \E s \in Slots : Ack(s)
    \/ Tick

Spec == Init /\ [][Next]_vars

\* Liveness: time goes on, the sender keeps sending and resends what is
\* unacknowledged (RNResendTiming), a due ack arrives, and the path turns
\* good (the last bad interval ends).
Fairness ==
    /\ WF_vars(Tick)
    /\ WF_vars(Send)
    /\ WF_vars(\E i \in 1..M : Resend(i))
    /\ WF_vars(\E s \in Slots : Ack(s))
    /\ SF_vars(Tick /\ good')
LiveSpec == Spec /\ Fairness

-----------------------------------------------------------------------------

TypeOK ==
    /\ srtt \in 0..SrttMax /\ hasRtt \in BOOLEAN
    /\ lastAckAge \in 0..Cap /\ hasLastAck \in BOOLEAN
    /\ silenceEndAge \in 0..Cap
    /\ hasOldest \in BOOLEAN /\ oldestNum \in 0..Mod - 1 /\ oldestFirstAge \in 0..Cap
    /\ ubase \in 0..Mod - 1 /\ Len(nums) <= M
    /\ pkts \in [Slots -> [used : BOOLEAN, age : 0..Cap, prtt : 0..RttHigh, num : -1..Mod - 1]]
    /\ owedQuiet \in 0..Probe /\ realEndAge \in 0..Cap

\* m9.2: no round-trip sample from a packet sent before the end of the
\* latest acknowledgement silence (a real one: a number owed for Probe
\* ticks with no ack) less srtt; the first sample is always taken.
NoSampleAcrossSilence == ~badSample

\* m9.2: a sample is refused only for such a packet; a gap between acks in
\* which nothing was owed is no silence.
RefuseOnlyAfterOwedSilence == ~badRefusal

\* An outage never poisons the estimate: every sample taken is a round trip
\* of the path plus less than Probe ticks of delay, or no more than srtt.
OutageNeverPoisons == srtt < RttHigh + Probe

\* After the path slows to RttHigh and the bad intervals end, the estimate
\* reaches RttHigh and stays there: a sparse sender's estimate rises.
SlowPathRaisesEstimate == (rttNow = RttHigh /\ good) ~> [](srtt = RttHigh)

=============================================================================
