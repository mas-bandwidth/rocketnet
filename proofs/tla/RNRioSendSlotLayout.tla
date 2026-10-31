-------------------------- MODULE RNRioSendSlotLayout --------------------------
(***************************************************************************)
(* The RIO server transport's send slots at their layout                  *)
(* (sdk/rocketnet_server_rio_windows.cpp): three phases of Q slots, the    *)
(* per-phase reservation counter next[phase] advanced by one fetch_add per *)
(* rocketnet_server_rio_start_packet, the open phase fill, phase_state,    *)
(* phase_count and the send_ready FIFO of phases (head and count, tail     *)
(* ( head + count ) % ROCKETNET_RIO_SEND_PHASES).  RNRioDriver checks the  *)
(* slots with a reservation that stops at Q; this model carries the        *)
(* counter's own width, so a counter that wraps is visible.                *)
(*                                                                         *)
(* A fill thread's start_packet takes slot = next[fill]++ and refuses a   *)
(* slot outside 0..Q-1.  While main cannot hand the fill phase (the next   *)
(* phase is not idle: its sends have not all completed), the fill threads  *)
(* keep calling start_packet and every call advances the counter, refused  *)
(* or not.  Width is the counter's modulus: 0 is a counter that never      *)
(* wraps within the checked attempts (a 64-bit counter: 2^64 calls), and   *)
(* an even Width > 0 is a signed counter of that many values (an int:      *)
(* 2^32), which wraps from its largest value to its most negative and      *)
(* climbs back to 0.  Tries bounds the attempts in one open of a phase;    *)
(* with Width > 0 and Tries >= Width the wrap is in reach.                 *)
(*                                                                         *)
(* The checked claims:                                                     *)
(*   NoReissue     a reservation returns only a free slot: no slot is      *)
(*                 handed to a second packet while it is reserved,         *)
(*                 finished or owned by the kernel.                        *)
(*   ReadyMatches  the send_ready positions from the head are the phases   *)
(*                 handed and not taken, in hand order.                    *)
(***************************************************************************)
EXTENDS Integers, Sequences, FiniteSets

CONSTANTS Q,          \* ROCKETNET_RIO_SEND_QUEUE_SIZE: slots per phase
          P,          \* ROCKETNET_RIO_SEND_PHASES (3)
          Width,      \* the counter's modulus: 0 never wraps, else a signed width
          Tries,      \* start_packet calls in one open of a phase
          MaxHands    \* hands the harness makes in all

ASSUME Q >= 1 /\ P >= 3 /\ Tries >= 1 /\ MaxHands >= 1
ASSUME Width = 0 \/ (Width >= 2 /\ Width % 2 = 0)

Phases == 0..P - 1
Slots == 0..Q - 1
States == {"Free", "Reserved", "Finished", "Posted"}

\* fetch_add on the counter: two's-complement wrap at Width
Wrap(x) == IF Width = 0 THEN x
           ELSE LET r == (x + Width \div 2) % Width IN r - Width \div 2

VARIABLES next,       \* next[p]: the reservation counter
          slot,       \* slot[p][s]: free, reserved, finished, posted
          fill,       \* rio->fill
          pstate,     \* phase_state: "Fill", "Ready", "Posted", "Idle"
          pcount,     \* phase_count
          readyArr,   \* send_ready
          readyHead,  \* send_ready_head
          readyCount, \* send_ready_count
          tries,      \* ghost: start_packet calls in this open of the fill phase
          hands,      \* ghost: hands so far
          reissued,   \* ghost: a reservation returned a slot that was not free
          absReady    \* ghost: phases handed and not taken, in order

vars == <<next, slot, fill, pstate, pcount, readyArr, readyHead, readyCount, tries,
          hands, reissued, absReady>>

Init ==
    /\ next = [p \in Phases |-> 0]
    /\ slot = [p \in Phases |-> [s \in Slots |-> "Free"]]
    /\ fill = 0
    /\ pstate = [p \in Phases |-> IF p = 0 THEN "Fill" ELSE "Idle"]
    /\ pcount = [p \in Phases |-> 0]
    /\ readyArr = [p \in Phases |-> 0]
    /\ readyHead = 0
    /\ readyCount = 0
    /\ tries = 0
    /\ hands = 0
    /\ reissued = FALSE
    /\ absReady = <<>>

\* rocketnet_server_rio_start_packet: slot = next[phase].fetch_add( 1 ); a slot
\* outside 0..Q-1 is refused (SEND_FULL), any other is reserved.
Reserve ==
    /\ tries < Tries
    /\ LET s == next[fill]
       IN /\ next' = [next EXCEPT ![fill] = Wrap(s + 1)]
          /\ IF s < 0 \/ s >= Q
               THEN UNCHANGED <<slot, reissued>>
               ELSE /\ reissued' = (reissued \/ slot[fill][s] # "Free")
                    /\ slot' = [slot EXCEPT ![fill][s] = "Reserved"]
    /\ tries' = tries + 1
    /\ UNCHANGED <<fill, pstate, pcount, readyArr, readyHead, readyCount, hands, absReady>>

\* rocketnet_server_rio_finish_packet / rocketnet_server_rio_abort_packet: the
\* reserved slot records its bytes (or zero, and is never posted).
Finish(s) ==
    /\ slot[fill][s] = "Reserved"
    /\ \/ slot' = [slot EXCEPT ![fill][s] = "Finished"]
       \/ slot' = [slot EXCEPT ![fill][s] = "Free"]
    /\ UNCHANGED <<next, fill, pstate, pcount, readyArr, readyHead, readyCount, tries,
                   hands, reissued, absReady>>

\* rocketnet_server_rio_send_packets, with the fill threads joined: count =
\* next[fill], nothing when count <= 0, clamped to Q; the phase is handed onto
\* the send_ready tail only when the next phase is idle, then next[next] = 0 and
\* fill = next.
SendPackets ==
    /\ hands < MaxHands
    /\ \A s \in Slots : slot[fill][s] # "Reserved"
    /\ LET count == IF next[fill] > Q THEN Q ELSE next[fill]
           nx == (fill + 1) % P
           tail == (readyHead + readyCount) % P
       IN /\ count > 0
          /\ pstate[nx] = "Idle"
          /\ pstate' = [pstate EXCEPT ![fill] = "Ready", ![nx] = "Fill"]
          /\ pcount' = [pcount EXCEPT ![fill] = count]
          /\ readyArr' = [readyArr EXCEPT ![tail] = fill]
          /\ readyCount' = readyCount + 1
          /\ next' = [next EXCEPT ![nx] = 0]
          /\ fill' = nx
          /\ absReady' = Append(absReady, fill)
    /\ tries' = 0
    /\ hands' = hands + 1
    /\ UNCHANGED <<slot, readyHead, reissued>>

\* rocketnet_rio_worker_pass WTake and rocketnet_rio_post_phase: the oldest
\* handed phase becomes posted; each finished slot below its count is posted,
\* every other slot below it is zeroed.
TakeAndPost ==
    /\ readyCount > 0
    /\ LET p == readyArr[readyHead]
       IN /\ pstate' = [pstate EXCEPT ![p] = "Posted"]
          /\ slot' = [slot EXCEPT ![p] = [s \in Slots |->
                         IF s < pcount[p] /\ slot[p][s] = "Finished" THEN "Posted"
                         ELSE IF s < pcount[p] THEN "Free" ELSE slot[p][s]]]
    /\ readyHead' = (readyHead + 1) % P
    /\ readyCount' = readyCount - 1
    /\ absReady' = Tail(absReady)
    /\ UNCHANGED <<next, fill, pcount, readyArr, tries, hands, reissued>>

\* rocketnet_rio_dequeue_send for one completion: the kernel returns the slot.
Complete(p, s) ==
    /\ slot[p][s] = "Posted"
    /\ slot' = [slot EXCEPT ![p][s] = "Free"]
    /\ UNCHANGED <<next, fill, pstate, pcount, readyArr, readyHead, readyCount, tries,
                   hands, reissued, absReady>>

\* rocketnet_rio_worker_pass WPublish: a posted phase with nothing outstanding
\* is idle again.
Retire(p) ==
    /\ pstate[p] = "Posted"
    /\ \A s \in Slots : slot[p][s] # "Posted"
    /\ pstate' = [pstate EXCEPT ![p] = "Idle"]
    /\ UNCHANGED <<next, slot, fill, pcount, readyArr, readyHead, readyCount, tries,
                   hands, reissued, absReady>>

Next ==
    \/ Reserve
    \/ \E s \in Slots : Finish(s)
    \/ SendPackets
    \/ TakeAndPost
    \/ \E p \in Phases, s \in Slots : Complete(p, s)
    \/ \E p \in Phases : Retire(p)

Spec == Init /\ [][Next]_vars

-----------------------------------------------------------------------------

TypeOK ==
    /\ fill \in Phases
    /\ pstate \in [Phases -> {"Fill", "Ready", "Posted", "Idle"}]
    /\ slot \in [Phases -> [Slots -> States]]
    /\ readyHead \in Phases
    /\ readyCount \in 0..P
    /\ tries \in 0..Tries

NoReissue == ~reissued

ReadySeq == [i \in 1..readyCount |-> readyArr[(readyHead + i - 1) % P]]
ReadyMatches ==
    /\ ReadySeq = absReady
    /\ \A p \in Phases : (pstate[p] = "Ready") <=> (\E i \in 1..Len(absReady) : absReady[i] = p)

\* Exactly one phase is open to the fill threads, and it is fill.
OneFill == \A p \in Phases : (pstate[p] = "Fill") <=> (p = fill)

=============================================================================
