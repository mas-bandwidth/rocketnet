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
(*  threads joined, submits one SENDMSG per slot with bytes not yet       *)
(*  submitted (Submit) and reaps within its budget (Reap).  It resets the *)
(*  reservation index to 0 only when every send has its CQE (ResetIndex); *)
(*  otherwise it returns with the index unchanged (SendReturn) and a      *)
(*  later call retires the rest.  Between Submit and the CQE the kernel   *)
(*  owns the frame and msghdr (kown).                                     *)
(*                                                                         *)
(*  BUDGET.  The main thread reaps only inside a call: send_packets       *)
(*  (call = "send") or receive_packets (call = "recv", from RecvBegin to  *)
(*  Swap).  work counts the CQEs the current call has reaped, and a call  *)
(*  reaps at most Budget of them; CQEs past it stay in cq for a later     *)
(*  call.  ReapWithinBudget is the property every Reap step keeps.        *)
(*                                                                         *)
(*  RECEIVE.  One multishot RECVMSG over a ring of Buffers provided       *)
(*  buffers.  The kernel fills a ring buffer and posts a receive CQE      *)
(*  (KernelFill); the main thread copies the datagram into the FILL bank  *)
(*  (Reap) and returns the buffer to the ring (ReturnBuffer).  Receive    *)
(*  CQEs may be reaped inside send_packets and always go to the fill      *)
(*  bank.  receive_packets swaps the banks (Swap): the old fill bank      *)
(*  becomes the EXPOSED bank the transport reads (TransportReads) until   *)
(*  the next Swap.  A CQE without IORING_CQE_F_MORE (KernelStop, the      *)
(*  ENOBUFS case included) re-arms the multishot when it is reaped.       *)
(*                                                                         *)
(*  DESTROY.  Cancels the multishot (Destroy) and waits for every         *)
(*  outstanding CQE, sends and cancels, before freeing slots, banks and   *)
(*  the ring (Free).                                                      *)
(*                                                                         *)
(* Three rule switches carry the reversed witnesses.  ResetWaitsForReap = *)
(* FALSE lets ResetIndex run right after Submit; ReapIntoFill = FALSE     *)
(* writes reaped receive datagrams into the exposed bank; BoundedReap =   *)
(* FALSE is the drain until empty: no budget guard on Reap, and a call    *)
(* ends only with cq empty (receive) or every send reaped (send).         *)
(***************************************************************************)
EXTENDS Naturals, FiniteSets

CONSTANTS Slots,              \* send staging slots per phase
          Buffers,            \* provided receive buffers in the ring
          MaxSteps,           \* bound on datagrams and on bank swaps
          Budget,             \* CQEs one send or receive call may reap
          ResetWaitsForReap,  \* ResetIndex waits for every send CQE
          ReapIntoFill,       \* reaped receive datagrams go to the fill bank
          BoundedReap         \* Reap stops at Budget; a call may return early

ASSUME Slots \in Nat \ {0} /\ Buffers \in Nat \ {0} /\ MaxSteps \in Nat \ {0}
ASSUME Budget \in Nat \ {0}
ASSUME ResetWaitsForReap \in BOOLEAN /\ ReapIntoFill \in BOOLEAN
ASSUME BoundedReap \in BOOLEAN

SlotIds == 1..Slots
BufIds  == 1..Buffers
Dgs     == 1..MaxSteps

\* completion queue entries: a send CQE names its slot, a receive CQE its
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
          pending,  \* slots submitted by this send_packets, CQE not reaped
          sending,  \* the main thread is inside send_packets
          bstate,   \* per ring buffer: Ring, Filled or Copied
          bufdg,    \* per ring buffer: the datagram last filled into it
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

vars == <<sstate, bytes, idx, kown, pending, sending, bstate, bufdg, copied,
          fill, exposed, snap, gen, cq, armed, nextdg, phase, call, work>>

Init ==
    /\ sstate = [s \in SlotIds |-> "Free"]
    /\ bytes = [s \in SlotIds |-> FALSE]
    /\ idx = 0
    /\ kown = {}
    /\ pending = {}
    /\ sending = FALSE
    /\ bstate = [b \in BufIds |-> "Ring"]
    /\ bufdg = [b \in BufIds |-> 0]
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
SendVars == <<sstate, bytes, idx, kown, pending, sending>>
CallVars == <<call, work>>

\* start_packet: one fetch_add takes slot idx + 1; NULL (no step) when full
Reserve ==
    /\ phase = "Running" /\ ~sending
    /\ idx < Slots
    /\ sstate' = [sstate EXCEPT ![idx + 1] = "Reserved"]
    /\ bytes' = [bytes EXCEPT ![idx + 1] = FALSE]
    /\ idx' = idx + 1
    /\ UNCHANGED <<kown, pending, sending, cq, phase>>
    /\ UNCHANGED RecvVars
    /\ UNCHANGED CallVars

\* finish_packet: the bytes are recorded
Finish(s) ==
    /\ sstate[s] = "Reserved"
    /\ sstate' = [sstate EXCEPT ![s] = "Finished"]
    /\ bytes' = [bytes EXCEPT ![s] = TRUE]
    /\ UNCHANGED <<idx, kown, pending, sending, cq, phase>>
    /\ UNCHANGED RecvVars
    /\ UNCHANGED CallVars

\* abort_packet: the bytes are zeroed; the slot is skipped by send_packets
Abort(s) ==
    /\ sstate[s] = "Reserved"
    /\ sstate' = [sstate EXCEPT ![s] = "Finished"]
    /\ bytes' = [bytes EXCEPT ![s] = FALSE]
    /\ UNCHANGED <<idx, kown, pending, sending, cq, phase>>
    /\ UNCHANGED RecvVars
    /\ UNCHANGED CallVars

\* send_packets, fill threads joined: one SENDMSG per slot with bytes in
\* 0..min(reserved, size), then submit; the kernel owns those slots
Submit ==
    /\ phase = "Running" /\ ~sending /\ call = "none"
    /\ \A s \in SlotIds : sstate[s] # "Reserved"
    /\ LET out == {s \in SlotIds : sstate[s] = "Finished" /\ bytes[s]}
       IN /\ sstate' = [s \in SlotIds |->
                           IF s \in out THEN "Kernel" ELSE sstate[s]]
          /\ kown' = kown \cup out
          /\ pending' = pending \cup out
    /\ sending' = TRUE
    /\ call' = "send"
    /\ work' = 0
    /\ UNCHANGED <<bytes, idx, cq, phase>>
    /\ UNCHANGED RecvVars

\* the kernel finishes a send (any res, a negative one is SEND_DROPPED):
\* it gives up the frame and posts the send CQE
KernelComplete(s) ==
    /\ s \in kown
    /\ kown' = kown \ {s}
    /\ cq' = cq \cup {SendCqe(s)}
    /\ UNCHANGED <<sstate, bytes, idx, pending, sending, phase>>
    /\ UNCHANGED RecvVars
    /\ UNCHANGED CallVars

\* the kernel fills a ring buffer from the armed multishot receive
KernelFill(b) ==
    /\ armed
    /\ bstate[b] = "Ring"
    /\ nextdg <= MaxSteps
    /\ bstate' = [bstate EXCEPT ![b] = "Filled"]
    /\ bufdg' = [bufdg EXCEPT ![b] = nextdg]
    /\ nextdg' = nextdg + 1
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

\* the main thread reaps one CQE: inside a send_packets or receive_packets
\* call, at most Budget of them per call under BoundedReap, or in destroy's
\* drain (destroy's rounds are bounded in the code, not here)
Reap(c) ==
    /\ c \in cq
    /\ phase # "Freed"
    /\ \/ /\ phase = "Running"
          /\ call # "none"
          /\ BoundedReap => work < Budget
          /\ work' = work + 1
       \/ /\ phase = "Destroying"
          /\ UNCHANGED work
    /\ UNCHANGED call
    /\ cq' = cq \ {c}
    /\ CASE c.k = "send" ->
              \* frees nothing by itself: the slot is reaped
              /\ pending' = pending \ {c.x}
              /\ UNCHANGED <<sstate, bytes, idx, kown, sending>>
              /\ UNCHANGED RecvVars
         [] c.k = "recv" ->
              /\ bstate' = [bstate EXCEPT ![c.x] = "Copied"]
              /\ copied' = copied \cup {bufdg[c.x]}
              /\ IF ReapIntoFill
                 THEN /\ fill' = fill \cup {bufdg[c.x]}
                      /\ UNCHANGED exposed
                 ELSE /\ exposed' = exposed \cup {bufdg[c.x]}
                      /\ UNCHANGED fill
              /\ UNCHANGED <<bufdg, snap, gen, armed, nextdg>>
              /\ UNCHANGED SendVars
         [] c.k = "term" ->
              \* re-arm the multishot while running; never during destroy
              /\ armed' = (phase = "Running")
              /\ UNCHANGED <<bstate, bufdg, copied, fill, exposed, snap, gen,
                             nextdg>>
              /\ UNCHANGED SendVars
         [] c.k = "cancel" ->
              /\ UNCHANGED RecvVars
              /\ UNCHANGED SendVars
    /\ UNCHANGED phase

\* the copied buffer goes back to the provided buffer ring
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
    /\ UNCHANGED <<kown, pending, cq, phase>>
    /\ UNCHANGED RecvVars

\* the other end of send_packets: some send is still without its CQE and
\* the call has spent its budget or found the cq empty after its one wait;
\* it returns with the index and every slot as they are, and a later
\* send_packets retires the rest
SendReturn ==
    /\ sending
    /\ BoundedReap
    /\ pending # {}
    /\ (cq = {} \/ work >= Budget)
    /\ sending' = FALSE
    /\ call' = "none"
    /\ work' = 0
    /\ UNCHANGED <<sstate, bytes, idx, kown, pending, cq, phase>>
    /\ UNCHANGED RecvVars

\* receive_packets begins: its reaps count against the budget
RecvBegin ==
    /\ phase = "Running" /\ ~sending /\ call = "none"
    /\ gen < MaxSteps
    /\ call' = "recv"
    /\ work' = 0
    /\ UNCHANGED <<bstate, bufdg, copied, fill, exposed, snap, gen, armed,
                   nextdg, cq, phase>>
    /\ UNCHANGED SendVars

\* the end of receive_packets, once its reap stops (the cq is empty, or
\* the budget is spent): the fill bank becomes the exposed bank, a new fill
\* bank is emptied
Swap ==
    /\ phase = "Running" /\ call = "recv"
    /\ (cq = {} \/ (BoundedReap /\ work >= Budget))
    /\ call' = "none"
    /\ work' = 0
    /\ exposed' = fill
    /\ snap' = fill
    /\ fill' = {}
    /\ gen' = gen + 1
    /\ UNCHANGED <<bstate, bufdg, copied, armed, nextdg, cq, phase>>
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
    /\ UNCHANGED <<bytes, idx, kown, pending, sending, cq>>
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
    /\ WF_vars(Submit)
    /\ WF_vars(ResetIndex)
    /\ WF_vars(SendReturn)
    /\ WF_vars(Swap)
    /\ WF_vars(Free)

-----------------------------------------------------------------------------

TypeOK ==
    /\ sstate \in [SlotIds -> {"Free", "Reserved", "Finished", "Kernel"}]
    /\ bytes \in [SlotIds -> BOOLEAN]
    /\ idx \in 0..Slots
    /\ kown \subseteq SlotIds
    /\ pending \subseteq SlotIds
    /\ sending \in BOOLEAN
    /\ bstate \in [BufIds -> {"Ring", "Filled", "Copied"}]
    /\ bufdg \in [BufIds -> 0..MaxSteps]
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

\* Reserve never returns a slot the kernel owns: a slot held by a fill
\* thread (reserved or finished) is never one the kernel still owns
NoReuseWhileKernelOwned ==
    \A s \in SlotIds : sstate[s] \in {"Reserved", "Finished"} => s \notin kown

\* the exposed bank never changes between two Swaps: every read of it by
\* the transport sees the bank the last Swap exposed
ExposedStable ==
    exposed = snap

\* a buffer is back in the ring only after its datagram is in a bank
BufferReturnedAfterCopy ==
    \A b \in BufIds : (bstate[b] = "Ring" /\ bufdg[b] # 0) => bufdg[b] \in copied

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

\* every slot the kernel was handed is eventually free again
SubmittedEventuallyFree ==
    \A s \in SlotIds : (sstate[s] = "Kernel") ~> (sstate[s] = "Free")

\* destroy always ends in the freed driver
DestroyEventuallyFrees ==
    (phase = "Destroying") ~> (phase = "Freed")

=============================================================================
