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
(*  threads joined, submits one SENDMSG per slot with bytes (Submit),     *)
(*  reaps until every send it submitted has its CQE (Reap), and only then *)
(*  resets the reservation index to 0 (ResetIndex).  Between Submit and   *)
(*  the CQE the kernel owns the frame and msghdr (kown).                  *)
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
(* Two rule switches carry the reversed witnesses.  ResetWaitsForReap =   *)
(* FALSE lets ResetIndex run right after Submit; ReapIntoFill = FALSE     *)
(* writes reaped receive datagrams into the exposed bank.                 *)
(***************************************************************************)
EXTENDS Naturals, FiniteSets

CONSTANTS Slots,              \* send staging slots per phase
          Buffers,            \* provided receive buffers in the ring
          MaxSteps,           \* bound on datagrams and on bank swaps
          ResetWaitsForReap,  \* ResetIndex waits for every send CQE
          ReapIntoFill        \* reaped receive datagrams go to the fill bank

ASSUME Slots \in Nat \ {0} /\ Buffers \in Nat \ {0} /\ MaxSteps \in Nat \ {0}
ASSUME ResetWaitsForReap \in BOOLEAN /\ ReapIntoFill \in BOOLEAN

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
          phase     \* Running, Destroying or Freed

vars == <<sstate, bytes, idx, kown, pending, sending, bstate, bufdg, copied,
          fill, exposed, snap, gen, cq, armed, nextdg, phase>>

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

RecvVars == <<bstate, bufdg, copied, fill, exposed, snap, gen, armed, nextdg>>
SendVars == <<sstate, bytes, idx, kown, pending, sending>>

\* start_packet: one fetch_add takes slot idx + 1; NULL (no step) when full
Reserve ==
    /\ phase = "Running" /\ ~sending
    /\ idx < Slots
    /\ sstate' = [sstate EXCEPT ![idx + 1] = "Reserved"]
    /\ bytes' = [bytes EXCEPT ![idx + 1] = FALSE]
    /\ idx' = idx + 1
    /\ UNCHANGED <<kown, pending, sending, cq, phase>>
    /\ UNCHANGED RecvVars

\* finish_packet: the bytes are recorded
Finish(s) ==
    /\ sstate[s] = "Reserved"
    /\ sstate' = [sstate EXCEPT ![s] = "Finished"]
    /\ bytes' = [bytes EXCEPT ![s] = TRUE]
    /\ UNCHANGED <<idx, kown, pending, sending, cq, phase>>
    /\ UNCHANGED RecvVars

\* abort_packet: the bytes are zeroed; the slot is skipped by send_packets
Abort(s) ==
    /\ sstate[s] = "Reserved"
    /\ sstate' = [sstate EXCEPT ![s] = "Finished"]
    /\ bytes' = [bytes EXCEPT ![s] = FALSE]
    /\ UNCHANGED <<idx, kown, pending, sending, cq, phase>>
    /\ UNCHANGED RecvVars

\* send_packets, fill threads joined: one SENDMSG per slot with bytes in
\* 0..min(reserved, size), then submit; the kernel owns those slots
Submit ==
    /\ phase = "Running" /\ ~sending
    /\ \A s \in SlotIds : sstate[s] # "Reserved"
    /\ LET out == {s \in SlotIds : sstate[s] = "Finished" /\ bytes[s]}
       IN /\ sstate' = [s \in SlotIds |->
                           IF s \in out THEN "Kernel" ELSE sstate[s]]
          /\ kown' = kown \cup out
          /\ pending' = out
    /\ sending' = TRUE
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

\* the multishot ends with a CQE without F_MORE (ENOBUFS among the causes)
KernelStop ==
    /\ armed
    /\ Term \notin cq
    /\ armed' = FALSE
    /\ cq' = cq \cup {Term}
    /\ UNCHANGED <<bstate, bufdg, copied, fill, exposed, snap, gen, nextdg, phase>>
    /\ UNCHANGED SendVars

\* the main thread reaps one CQE, inside send_packets or out of it
Reap(c) ==
    /\ c \in cq
    /\ phase # "Freed"
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

\* the end of send_packets: reset the reservation index to 0, only when no
\* slot is kernel owned and every send submitted in this call is reaped
ResetIndex ==
    /\ sending
    /\ ResetWaitsForReap => (kown = {} /\ pending = {})
    /\ sstate' = [s \in SlotIds |-> "Free"]
    /\ bytes' = [s \in SlotIds |-> FALSE]
    /\ idx' = 0
    /\ sending' = FALSE
    /\ UNCHANGED <<kown, pending, cq, phase>>
    /\ UNCHANGED RecvVars

\* receive_packets: the fill bank becomes the exposed bank, a new fill
\* bank is emptied
Swap ==
    /\ phase = "Running" /\ ~sending
    /\ gen < MaxSteps
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
    /\ phase = "Running" /\ ~sending
    /\ UNCHANGED vars

\* destroy: cancel every multishot receive; the cancel posts its own CQE
\* and an armed multishot ends with a CQE without F_MORE
Destroy ==
    /\ phase = "Running" /\ ~sending
    /\ phase' = "Destroying"
    /\ armed' = FALSE
    /\ cq' = cq \cup {Cancel} \cup (IF armed THEN {Term} ELSE {})
    /\ UNCHANGED <<bstate, bufdg, copied, fill, exposed, snap, gen, nextdg>>
    /\ UNCHANGED SendVars

\* destroy's drain ends: every outstanding CQE, sends and cancels, is in;
\* slots, banks and the ring are freed
Free ==
    /\ phase = "Destroying"
    /\ kown = {} /\ cq = {} /\ ~armed
    /\ phase' = "Freed"
    /\ sstate' = [s \in SlotIds |-> "Free"]
    /\ UNCHANGED <<bytes, idx, kown, pending, sending, cq>>
    /\ UNCHANGED RecvVars

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
    \/ Swap
    \/ TransportReads
    \/ Destroy
    \/ Free
    \/ Done

Spec == Init /\ [][Next]_vars

\* the kernel completes every send, the main thread reaps every CQE and
\* returns every copied buffer, send_packets runs to its reset, and
\* destroy's drain ends; a CQ is FIFO, so each CQE is fair on its own
SpecFair ==
    /\ Spec
    /\ \A s \in SlotIds : WF_vars(KernelComplete(s))
    /\ \A c \in Cqes : WF_vars(Reap(c))
    /\ \A b \in BufIds : WF_vars(ReturnBuffer(b))
    /\ WF_vars(ResetIndex)
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

\* every slot the kernel was handed is eventually free again
SubmittedEventuallyFree ==
    \A s \in SlotIds : (sstate[s] = "Kernel") ~> (sstate[s] = "Free")

\* destroy always ends in the freed driver
DestroyEventuallyFrees ==
    (phase = "Destroying") ~> (phase = "Freed")

=============================================================================
