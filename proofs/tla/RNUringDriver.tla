---------------------------- MODULE RNUringDriver ----------------------------
(***************************************************************************)
(* The io_uring server driver (sdk/rocketnet_server_uring_linux.cpp, not  *)
(* yet written; THE DESIGN of card rocketnet-io-uring-driver-core).  One  *)
(* io_uring per driver, used only on the main thread.  The model owns the *)
(* three state machines of the design:                                    *)
(*                                                                         *)
(*  SEND.  A staging of Slots frame slots.  start_packet reserves the     *)
(*  next slot with one fetch_add (Reserve), finish records the bytes      *)
(*  (Finish), abort zeroes them (Abort).  send_packets, with the fill     *)
(*  threads joined, submits one SENDMSG per send run of slots with bytes  *)
(*  not yet submitted (Submit) and reaps within its budget (Reap).  A run  *)
(*  is one slot, or with UDP_SEGMENT consecutive slots sent as one        *)
(*  segmented SENDMSG: the run's first slot (its head) names the send,    *)
(*  and the run's one CQE (KernelComplete of the head) gives every slot   *)
(*  of the run back.  It resets the *)
(*  reservation index to 0 only when every send has its CQE (ResetIndex); *)
(*  otherwise it returns with the index unchanged (SendReturn) and a      *)
(*  later call retires the rest.  Between Submit and the CQE the kernel   *)
(*  owns the frame and msghdr (kown).                                     *)
(*                                                                         *)
(*  BUDGET.  The main thread reaps only inside a call: send_packets       *)
(*  (call = "send") or receive_packets (call = "recv", from RecvBegin to  *)
(*  Swap).  work counts the CQEs the current call has reaped, and a call  *)
(*  reaps at most Budget of them; CQEs past it stay in cq for a later     *)
(*  call.  ReapWithinBudget is the property every Reap step keeps.  A     *)
(*  receive CQE is reaped only while the fill bank has Room for a whole   *)
(*  coalesced receive (Segments datagrams); one without room stays in cq  *)
(*  and ends the call, so FillWithinRoom holds however many the kernel    *)
(*  posts between two swaps.                                              *)
(*                                                                         *)
(*  RECEIVE.  One multishot RECVMSG over a ring of Buffers provided       *)
(*  buffers.  The kernel fills a ring buffer with one to Segments         *)
(*  datagrams (several with UDP_GRO: one coalesced receive) and posts a   *)
(*  receive CQE (KernelFill); the main thread reaps it and copies every   *)
(*  segment into the FILL bank (Reap: the copy takes them all, inside the *)
(*  one reap on the one main thread) and returns the buffer to the ring   *)
(*  after the last copy (ReturnBuffer).  Receive                          *)
(*  CQEs may be reaped inside send_packets and always go to the fill      *)
(*  bank.  receive_packets swaps the banks (Swap): the old fill bank      *)
(*  becomes the EXPOSED bank the transport reads (TransportReads) until   *)
(*  the next Swap.  A CQE without IORING_CQE_F_MORE (KernelStop, the      *)
(*  ENOBUFS case included) ends the multishot: the one-shot re-arm the    *)
(*  CQE handler attempts may find no submission entry, so a receive       *)
(*  re-arms a multishot left disarmed.                                    *)
(*                                                                         *)
(*  DESTROY.  Cancels the multishot (Destroy) and waits for every         *)
(*  outstanding CQE, sends and cancels, before freeing slots, banks and   *)
(*  the ring (Free).                                                      *)
(*                                                                         *)
(* Six rule switches carry the reversed witnesses.  ReapWithinRoom =      *)
(* FALSE reaps a receive CQE into a fill bank that has no room for it     *)
(* (the segments past Room are dropped in the code).  ResetWaitsForReap = *)
(* FALSE lets ResetIndex run right after Submit; ReapIntoFill = FALSE     *)
(* writes reaped receive datagrams into the exposed bank; BoundedReap =   *)
(* FALSE is the drain until empty: no budget guard on Reap, and a call    *)
(* ends only with cq empty (receive) or every send reaped (send);         *)
(* RearmOnReceive = FALSE leaves a multishot that ends disarmed for good; *)
(* RunRetiredByCqe = FALSE hands a run's slots after its head back to the *)
(* staging at Submit, while the kernel still reads their frames.          *)
(*                                                                         *)
(* Two definitions a configuration may replace with <- (so a cfg that     *)
(* does not name them checks the rule): Segments, the most datagrams one  *)
(* KernelFill puts in a buffer, and the sixth rule switch                 *)
(* ReturnAfterLastCopy; FALSE marks the buffer copied, so ReturnBuffer    *)
(* may run, once its first segment is copied, the rest still to copy.     *)
(***************************************************************************)
EXTENDS Naturals, FiniteSets

CONSTANTS Slots,              \* send staging slots per phase
          Buffers,            \* provided receive buffers in the ring
          MaxSteps,           \* bound on datagrams and on bank swaps
          Budget,             \* CQEs one send or receive call may reap
          Room,               \* datagrams the fill bank holds between two swaps
          ReapWithinRoom,     \* a receive CQE is reaped only with Room for Segments more
          ResetWaitsForReap,  \* ResetIndex waits for every send CQE
          ReapIntoFill,       \* reaped receive datagrams go to the fill bank
          BoundedReap,        \* Reap stops at Budget; a call may return early
          RearmOnReceive,     \* receive_packets re-arms a disarmed multishot
          RunRetiredByCqe     \* a run's slots stay the kernel's until its CQE

ASSUME Slots \in Nat \ {0} /\ Buffers \in Nat \ {0} /\ MaxSteps \in Nat \ {0}
ASSUME Budget \in Nat \ {0}
ASSUME Room \in Nat \ {0} /\ ReapWithinRoom \in BOOLEAN
ASSUME ResetWaitsForReap \in BOOLEAN /\ ReapIntoFill \in BOOLEAN
ASSUME BoundedReap \in BOOLEAN
ASSUME RearmOnReceive \in BOOLEAN
ASSUME RunRetiredByCqe \in BOOLEAN

\* the most datagrams one receive buffer carries (UDP_GRO coalescing) and
\* the rule switch on the buffer's return: definitions, so a cfg replaces
\* them with <- (MCRNUringDriver.tla gives the replacement values)
Segments == 2
ReturnAfterLastCopy == TRUE

SlotIds == 1..Slots
BufIds  == 1..Buffers
Dgs     == 1..MaxSteps

\* completion queue entries: a send CQE names its run's head slot, a receive CQE its
\* ring buffer; Term is a multishot CQE without F_MORE, Cancel the CQE of
\* the cancel SQE destroy submits
SendCqe(s) == [k |-> "send", x |-> s]
RecvCqe(b) == [k |-> "recv", x |-> b]
Term       == [k |-> "term", x |-> 0]
Cancel     == [k |-> "cancel", x |-> 0]
Cqes == {SendCqe(s) : s \in SlotIds} \cup {RecvCqe(b) : b \in BufIds}
        \cup {Term, Cancel}

VARIABLES sstate,   \* per slot: Free, Reserved, Finished or Kernel
          bytes,    \* per slot: finish recorded bytes (abort zeroes them)
          idx,      \* the reservation index
          kown,     \* slots whose frame and msghdr the kernel owns
          head,     \* per slot: the head slot of the kernel-owned send carrying it, 0 none
          pending,  \* heads of sends submitted, CQE not reaped
          sending,  \* the main thread is inside send_packets
          bstate,   \* per ring buffer: Ring, Filled or Copied
          bufdg,    \* per ring buffer: the datagrams filled into it not yet in a bank
          copied,   \* datagrams copied out into a bank
          fill,     \* the fill bank
          exposed,  \* the exposed bank the transport reads
          snap,     \* the exposed bank as the last Swap left it
          gen,      \* the exposed generation, one per Swap
          cq,       \* pending completions
          armed,    \* the multishot receive is armed in the kernel
          nextdg,   \* the next datagram id the kernel delivers
          phase,    \* Running, Destroying or Freed
          call,     \* the main thread's call: none, send or recv
          work      \* CQEs the current call has reaped

vars == <<sstate, bytes, idx, kown, head, pending, sending, bstate, bufdg, copied,
          fill, exposed, snap, gen, cq, armed, nextdg, phase, call, work>>

Init ==
    /\ sstate = [s \in SlotIds |-> "Free"]
    /\ bytes = [s \in SlotIds |-> FALSE]
    /\ idx = 0
    /\ kown = {}
    /\ head = [s \in SlotIds |-> 0]
    /\ pending = {}
    /\ sending = FALSE
    /\ bstate = [b \in BufIds |-> "Ring"]
    /\ bufdg = [b \in BufIds |-> {}]
    /\ copied = {}
    /\ fill = {}
    /\ exposed = {}
    /\ snap = {}
    /\ gen = 0
    /\ cq = {}
    /\ armed = TRUE
    /\ nextdg = 1
    /\ phase = "Running"
    /\ call = "none"
    /\ work = 0

RecvVars == <<bstate, bufdg, copied, fill, exposed, snap, gen, armed, nextdg>>

\* the segments one reap copies out of a buffer's datagrams D: all of them,
\* or under ReturnAfterLastCopy = FALSE only the first before the buffer
\* counts as copied
FirstSegment(D) == {d \in D : \A e \in D : d <= e}
CopiedSegments(D) == IF ReturnAfterLastCopy THEN D ELSE FirstSegment(D)
SendVars == <<sstate, bytes, idx, kown, head, pending, sending>>
CallVars == <<call, work>>

\* start_packet: one fetch_add takes slot idx + 1; NULL (no step) when full
Reserve ==
    /\ phase = "Running" /\ ~sending
    /\ idx < Slots
    /\ sstate' = [sstate EXCEPT ![idx + 1] = "Reserved"]
    /\ bytes' = [bytes EXCEPT ![idx + 1] = FALSE]
    /\ idx' = idx + 1
    /\ UNCHANGED <<kown, head, pending, sending, cq, phase>>
    /\ UNCHANGED RecvVars
    /\ UNCHANGED CallVars

\* finish_packet: the bytes are recorded
Finish(s) ==
    /\ sstate[s] = "Reserved"
    /\ sstate' = [sstate EXCEPT ![s] = "Finished"]
    /\ bytes' = [bytes EXCEPT ![s] = TRUE]
    /\ UNCHANGED <<idx, kown, head, pending, sending, cq, phase>>
    /\ UNCHANGED RecvVars
    /\ UNCHANGED CallVars

\* abort_packet: the bytes are zeroed; the slot is skipped by send_packets
Abort(s) ==
    /\ sstate[s] = "Reserved"
    /\ sstate' = [sstate EXCEPT ![s] = "Finished"]
    /\ bytes' = [bytes EXCEPT ![s] = FALSE]
    /\ UNCHANGED <<idx, kown, head, pending, sending, cq, phase>>
    /\ UNCHANGED RecvVars
    /\ UNCHANGED CallVars

\* send_packets, fill threads joined: the slots with bytes in
\* 0..min(reserved, size) cut into send runs, one SENDMSG per run, then
\* submit; the kernel owns every slot of every run.  H is the set of run
\* heads: each slot to send belongs to the run of the nearest head at or
\* below it, so every run is consecutive sending slots (the code also ends
\* a run at an aborted slot, a longer packet, another destination or a
\* bound, which picks one of these cuts).  H = out is one SENDMSG per
\* slot, the plain tier.  Under RunRetiredByCqe = FALSE the slots of a run
\* after its head go back to the staging at once, as if the segmented send
\* had copied them, while the kernel still owns their frames
\* (sdk/rocketnet_server_uring_linux.cpp rocketnet_server_uring_send_packets)
HeadOf(H, s) == CHOOSE h \in H : h <= s /\ \A g \in H : g <= s => g <= h

Submit ==
    /\ phase = "Running" /\ ~sending /\ call = "none"
    /\ \A s \in SlotIds : sstate[s] # "Reserved"
    /\ LET out == {s \in SlotIds : sstate[s] = "Finished" /\ bytes[s]}
       IN \E H \in SUBSET out :
          /\ \A s \in out : \E h \in H : h <= s
          /\ head' = [s \in SlotIds |-> IF s \in out THEN HeadOf(H, s) ELSE head[s]]
          /\ sstate' = [s \in SlotIds |->
                           IF s \in out /\ (RunRetiredByCqe \/ s \in H) THEN "Kernel"
                           ELSE IF s \in out THEN "Free"
                           ELSE sstate[s]]
          /\ kown' = kown \cup out
          /\ pending' = pending \cup H
    /\ sending' = TRUE
    /\ call' = "send"
    /\ work' = 0
    /\ UNCHANGED <<bytes, idx, cq, phase>>
    /\ UNCHANGED RecvVars

\* the kernel finishes a send (any res, a negative one is SEND_DROPPED for
\* every packet of the run): it gives up the frames of every slot of the
\* run at once and posts the run's one send CQE, named by its head
KernelComplete(s) ==
    /\ s \in kown
    /\ head[s] = s
    /\ kown' = kown \ {t \in SlotIds : head[t] = s}
    /\ head' = [t \in SlotIds |-> IF head[t] = s THEN 0 ELSE head[t]]
    /\ cq' = cq \cup {SendCqe(s)}
    /\ UNCHANGED <<sstate, bytes, idx, pending, sending, phase>>
    /\ UNCHANGED RecvVars
    /\ UNCHANGED CallVars

\* the kernel fills a ring buffer from the armed multishot receive: one
\* datagram, or with UDP_GRO a coalesced run of up to Segments of them in
\* the one buffer and the one CQE
KernelFill(b) ==
    /\ armed
    /\ bstate[b] = "Ring"
    /\ nextdg <= MaxSteps
    /\ \E n \in 1..Segments :
          /\ nextdg + n - 1 <= MaxSteps
          /\ bufdg' = [bufdg EXCEPT ![b] = nextdg..(nextdg + n - 1)]
          /\ nextdg' = nextdg + n
    /\ bstate' = [bstate EXCEPT ![b] = "Filled"]
    /\ cq' = cq \cup {RecvCqe(b)}
    /\ UNCHANGED <<copied, fill, exposed, snap, gen, armed, phase>>
    /\ UNCHANGED SendVars
    /\ UNCHANGED CallVars

\* the multishot ends with a CQE without F_MORE (ENOBUFS among the causes)
KernelStop ==
    /\ armed
    /\ Term \notin cq
    /\ armed' = FALSE
    /\ cq' = cq \cup {Term}
    /\ UNCHANGED <<bstate, bufdg, copied, fill, exposed, snap, gen, nextdg, phase>>
    /\ UNCHANGED SendVars
    /\ UNCHANGED CallVars

\* the fill bank has room for a whole coalesced receive: the code's
\* rocketnet_server_uring_reap_has_room, num_packets + MAX_SEGMENTS <= BANK_SIZE
HasRoom == Cardinality(fill) + Segments <= Room

\* a receive CQE waits in cq for room: the code's reap stops at it, so the
\* call ends as it does on a spent budget
RecvWaitsForRoom == ReapWithinRoom /\ ~HasRoom /\ \E c \in cq : c.k = "recv"

\* the main thread reaps one CQE: inside a send_packets or receive_packets
\* call, at most Budget of them per call under BoundedReap, or in destroy's
\* drain (destroy's rounds are bounded in the code, not here); a receive
\* CQE only with room for its segments in the fill bank
Reap(c) ==
    /\ c \in cq
    /\ phase # "Freed"
    /\ \/ /\ phase = "Running"
          /\ call # "none"
          /\ BoundedReap => work < Budget
          /\ (ReapWithinRoom /\ c.k = "recv") => HasRoom
          /\ work' = work + 1
       \/ /\ phase = "Destroying"
          /\ UNCHANGED work
    /\ UNCHANGED call
    /\ cq' = cq \ {c}
    /\ CASE c.k = "send" ->
              \* frees nothing by itself: the run is reaped
              /\ pending' = pending \ {c.x}
              /\ UNCHANGED <<sstate, bytes, idx, kown, head, sending>>
              /\ UNCHANGED RecvVars
         [] c.k = "recv" ->
              \* rocketnet_server_uring_receive_cqe's loop over the
              \* rocketnet_coalesce_split segments: every segment into the
              \* bank (a segment dropped by size or a full bank is counted in
              \* place of its copy), then the buffer is copied
              /\ bstate' = [bstate EXCEPT ![c.x] = "Copied"]
              /\ copied' = copied \cup CopiedSegments(bufdg[c.x])
              /\ IF ReapIntoFill
                 THEN /\ fill' = fill \cup CopiedSegments(bufdg[c.x])
                      /\ UNCHANGED exposed
                 ELSE /\ exposed' = exposed \cup CopiedSegments(bufdg[c.x])
                      /\ UNCHANGED fill
              \* the copied segments leave the buffer: what remains is the
              \* datagrams still to copy, none once every segment is in
              /\ bufdg' = [bufdg EXCEPT ![c.x] = @ \ CopiedSegments(@)]
              /\ UNCHANGED <<snap, gen, armed, nextdg>>
              /\ UNCHANGED SendVars
         [] c.k = "term" ->
              \* the multishot ended. while running, the CQE handler's one-shot
              \* re-arm either succeeds or finds no submission entry, and then
              \* the multishot is disarmed until a receive re-arms it; during
              \* destroy it is never re-armed
              /\ armed' \in IF phase = "Running" THEN {FALSE, TRUE} ELSE {FALSE}
              /\ UNCHANGED <<bstate, bufdg, copied, fill, exposed, snap, gen,
                             nextdg>>
              /\ UNCHANGED SendVars
         [] c.k = "cancel" ->
              /\ UNCHANGED RecvVars
              /\ UNCHANGED SendVars
    /\ UNCHANGED phase

\* the copied buffer goes back to the provided buffer ring, after the copy
\* of its last segment (under ReturnAfterLastCopy = FALSE, after the first)
ReturnBuffer(b) ==
    /\ bstate[b] = "Copied"
    /\ phase # "Freed"
    /\ bstate' = [bstate EXCEPT ![b] = "Ring"]
    /\ UNCHANGED <<bufdg, copied, fill, exposed, snap, gen, armed,
                   nextdg, cq, phase>>
    /\ UNCHANGED SendVars
    /\ UNCHANGED CallVars

\* the end of send_packets: reset the reservation index to 0, only when no
\* slot is kernel owned and every send submitted is reaped
ResetIndex ==
    /\ sending
    /\ ResetWaitsForReap => (kown = {} /\ pending = {})
    /\ sstate' = [s \in SlotIds |-> "Free"]
    /\ bytes' = [s \in SlotIds |-> FALSE]
    /\ idx' = 0
    /\ sending' = FALSE
    /\ call' = "none"
    /\ work' = 0
    /\ UNCHANGED <<kown, head, pending, cq, phase>>
    /\ UNCHANGED RecvVars

\* the other end of send_packets: some send is still without its CQE and
\* the call has spent its budget or found the cq empty after its one wait;
\* it returns with the index and every slot as they are, and a later
\* send_packets retires the rest
SendReturn ==
    /\ sending
    /\ BoundedReap
    /\ pending # {}
    /\ (cq = {} \/ work >= Budget \/ RecvWaitsForRoom)
    /\ sending' = FALSE
    /\ call' = "none"
    /\ work' = 0
    /\ UNCHANGED <<sstate, bytes, idx, kown, head, pending, cq, phase>>
    /\ UNCHANGED RecvVars

\* receive_packets begins: its reaps count against the budget
RecvBegin ==
    /\ phase = "Running" /\ ~sending /\ call = "none"
    /\ (gen < MaxSteps \/ ~armed)
    /\ call' = "recv"
    /\ work' = 0
    /\ UNCHANGED <<bstate, bufdg, copied, fill, exposed, snap, gen, armed,
                   nextdg, cq, phase>>
    /\ UNCHANGED SendVars

\* the end of receive_packets, once its reap stops (the cq is empty, or
\* the budget is spent): the fill bank becomes the exposed bank, a new fill
\* bank is emptied. after the generation bound only a re-arm is left, so
\* the generation stays put while a disarmed multishot is armed again
Swap ==
    /\ phase = "Running" /\ call = "recv"
    /\ (cq = {} \/ (BoundedReap /\ work >= Budget) \/ RecvWaitsForRoom)
    /\ call' = "none"
    /\ work' = 0
    /\ exposed' = fill
    /\ snap' = fill
    /\ fill' = {}
    /\ gen' = IF gen < MaxSteps THEN gen + 1 ELSE gen
    \* receive_packets re-arms a disarmed multishot when the rule has the fix
    /\ armed' = (RearmOnReceive \/ armed)
    /\ UNCHANGED <<bstate, bufdg, copied, nextdg, cq, phase>>
    /\ UNCHANGED SendVars

\* the transport reads the views of the exposed bank: a read changes no
\* driver state, so it is a stutter here; what it relies on is that the
\* bank is the one the last Swap exposed (ExposedStable)
TransportReads ==
    /\ phase = "Running" /\ ~sending /\ call = "none"
    /\ UNCHANGED vars

\* destroy: cancel every multishot receive; the cancel posts its own CQE
\* and an armed multishot ends with a CQE without F_MORE
Destroy ==
    /\ phase = "Running" /\ ~sending /\ call = "none"
    /\ phase' = "Destroying"
    /\ armed' = FALSE
    /\ cq' = cq \cup {Cancel} \cup (IF armed THEN {Term} ELSE {})
    /\ UNCHANGED <<bstate, bufdg, copied, fill, exposed, snap, gen, nextdg>>
    /\ UNCHANGED SendVars
    /\ UNCHANGED CallVars

\* destroy's drain ends: every outstanding CQE, sends and cancels, is in;
\* slots, banks and the ring are freed
Free ==
    /\ phase = "Destroying"
    /\ kown = {} /\ cq = {} /\ ~armed
    /\ phase' = "Freed"
    /\ sstate' = [s \in SlotIds |-> "Free"]
    /\ UNCHANGED <<bytes, idx, kown, head, pending, sending, cq>>
    /\ UNCHANGED RecvVars
    /\ UNCHANGED CallVars

\* the driver is gone; the run ends here
Done ==
    /\ phase = "Freed"
    /\ UNCHANGED vars

Next ==
    \/ Reserve
    \/ \E s \in SlotIds : Finish(s) \/ Abort(s) \/ KernelComplete(s)
    \/ Submit
    \/ \E b \in BufIds : KernelFill(b) \/ ReturnBuffer(b)
    \/ KernelStop
    \/ \E c \in Cqes : Reap(c)
    \/ ResetIndex
    \/ SendReturn
    \/ RecvBegin
    \/ Swap
    \/ TransportReads
    \/ Destroy
    \/ Free
    \/ Done

Spec == Init /\ [][Next]_vars

\* the kernel completes every send, the main thread reaps every CQE and
\* returns every copied buffer, a call that may end does end, the game
\* finishes what it reserves and keeps calling send_packets, and destroy's
\* drain ends.  A CQ is FIFO, so each CQE is fair on its own.  A send CQE
\* is strongly fair: a budget disables Reap at the end of every call, but a
\* pending CQE is reaped before any CQE posted after it, so a send CQE
\* enabled at the start of infinitely many calls is never passed over
\* forever (the receive side's CQEs keep weak fairness: no property here
\* waits on one while a budget is spent)
SpecFair ==
    /\ Spec
    /\ \A s \in SlotIds : WF_vars(KernelComplete(s))
    /\ \A s \in SlotIds : WF_vars(Finish(s) \/ Abort(s))
    /\ \A s \in SlotIds : SF_vars(Reap(SendCqe(s)))
    /\ \A c \in Cqes : WF_vars(Reap(c))
    /\ \A b \in BufIds : WF_vars(ReturnBuffer(b))
    /\ SF_vars(Submit)
    /\ WF_vars(ResetIndex)
    /\ WF_vars(SendReturn)
    /\ SF_vars(RecvBegin)
    /\ WF_vars(Swap)
    /\ WF_vars(Free)

-----------------------------------------------------------------------------

TypeOK ==
    /\ sstate \in [SlotIds -> {"Free", "Reserved", "Finished", "Kernel"}]
    /\ bytes \in [SlotIds -> BOOLEAN]
    /\ idx \in 0..Slots
    /\ kown \subseteq SlotIds
    /\ head \in [SlotIds -> 0..Slots]
    /\ pending \subseteq SlotIds
    /\ sending \in BOOLEAN
    /\ bstate \in [BufIds -> {"Ring", "Filled", "Copied"}]
    /\ bufdg \in [BufIds -> SUBSET Dgs]
    /\ copied \subseteq Dgs
    /\ fill \subseteq Dgs
    /\ exposed \subseteq Dgs
    /\ snap \subseteq Dgs
    /\ gen \in 0..MaxSteps
    /\ cq \subseteq Cqes
    /\ armed \in BOOLEAN
    /\ nextdg \in 1..(MaxSteps + 1)
    /\ phase \in {"Running", "Destroying", "Freed"}
    /\ call \in {"none", "send", "recv"}
    /\ work \in Nat

\* Reserve never returns a slot the kernel owns: a slot the staging holds
\* (free for the next Reserve, reserved or finished by a fill thread) is
\* never one the kernel still owns, whichever send of a run carries it
NoReuseWhileKernelOwned ==
    \A s \in SlotIds : sstate[s] \in {"Free", "Reserved", "Finished"} => s \notin kown

\* the exposed bank never changes between two Swaps: every read of it by
\* the transport sees the bank the last Swap exposed
ExposedStable ==
    exposed = snap

\* a buffer is back in the ring only after every datagram it carried is in
\* a bank: with UDP_GRO, after the copy of its last segment.  bufdg holds
\* only the datagrams still to copy, so a returned buffer holds none (and a
\* returned buffer keeps no history, which keeps the state space small)
BufferReturnedAfterCopy ==
    \A b \in BufIds : bstate[b] = "Ring" => bufdg[b] = {}

\* Freed only with no kernel owned slot, no filled buffer and an empty cq
FreedOnlyWhenDrained ==
    phase = "Freed" =>
        /\ kown = {}
        /\ \A b \in BufIds : bstate[b] # "Filled"
        /\ cq = {}

\* every Reap step of a send or receive call ends within the call's
\* budget: no call reaps more than Budget CQEs, so a call returns however
\* many CQEs the kernel keeps posting
ReapWithinBudget ==
    work <= Budget

\* the fill bank never holds more than Room datagrams: a receive CQE whose
\* segments would not fit waits in cq, whatever the kernel posts between
\* two swaps and whichever call reaps
FillWithinRoom ==
    Cardinality(fill) <= Room

\* every slot the kernel was handed is eventually free again
SubmittedEventuallyFree ==
    \A s \in SlotIds : (sstate[s] = "Kernel") ~> (sstate[s] = "Free")

\* destroy always ends in the freed driver
DestroyEventuallyFrees ==
    (phase = "Destroying") ~> (phase = "Freed")

\* a disarmed multishot is re-armed by a receive while the driver runs, or
\* destroy ends the run; a receive with no retry never re-arms, a silent
\* permanent receive stall
ReceiveLiveness ==
    (phase = "Running" /\ armed = FALSE) ~> (armed = TRUE \/ phase # "Running")

=============================================================================
