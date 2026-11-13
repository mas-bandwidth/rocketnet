------------------- MODULE TraceRNUringQueueReceiveLayout -------------------
(***************************************************************************)
(* The trace check of RNUringQueueReceiveLayout, copied from the template  *)
(* TraceRNTraceExample.tla (README-TraceCheck.md).  TraceLog is the record *)
(* sequence tests/tla_trace_uring_receive.cpp writes over                  *)
(* sdk/rocketnet_uring_receive_queue.cpp with a fake kernel: one record    *)
(* per call, with the call (op), its arguments and what it gave back       *)
(* (args), the model action its own result says it took (act), and every  *)
(* model variable after it (state).  The kernel's take is KernelReceive    *)
(* with the buffer the ring's head gave; a completion is Complete with the *)
(* datagrams cqe returned it banked; a drain is Drain with the count it    *)
(* returned; an arm that returned true is Arm.  Everything but EXTENDS,    *)
(* VarsRec, OpActs, Act and the reach predicates is the template's text.   *)
(***************************************************************************)
EXTENDS RNUringQueueReceiveLayout, Sequences

CONSTANT TraceLog

\* the log as a definition of no arguments, which TLC evaluates once when
\* it starts; every formula below reads Trace, never TraceLog. Read through
\* the substituted constant, TLC rebuilds the whole log at each access, and a
\* 3200-record trace takes 68 s instead of 3 s (bench-nyc-6, 2026-11-12)
Trace == TraceLog

\* checked once, when TLC starts
ASSUME /\ Len(TraceLog) >= 2
       /\ \A n \in DOMAIN TraceLog : DOMAIN TraceLog[n] = {"op", "act", "args", "state"}

VARIABLES ti,        \* the index of the record the state matches
          rejected   \* the record after ti is not a model step

\* every model variable by the field name the harness writes
VarsRec ==
    [arrived |-> arrived, nextId |-> nextId, sock |-> sock, karmed |-> karmed, cq |-> cq,
     armed |-> armed, stopping |-> stopping, brHead |-> brHead, brTail |-> brTail,
     entries |-> entries, owner |-> owner, slotD |-> slotD, tail |-> tail, head |-> head,
     headSeen |-> headSeen, exposed |-> exposed, reading |-> reading, vidx |-> vidx,
     view |-> view, banked |-> banked, delivered |-> delivered, named |-> named,
     full |-> full, spurious |-> spurious, dup |-> dup, torn |-> torn]

\* the actions each operation of the code can be
OpActs == [arrive |-> {"Arrive"}, receive |-> {"KernelReceive"}, noBuffers |-> {"KernelNoBuffers"},
           cancel |-> {"KernelCancel"}, stop |-> {"Stop"}, arm |-> {"Arm"}, cqe |-> {"Complete"},
           drain |-> {"Drain"}, done |-> {"Done"}]

\* the action a record names, with the record's arguments; a name this CASE
\* lacks stops TLC with an error
Act(a, x) == CASE a = "Arrive" -> Arrive
               [] a = "KernelReceive" -> KernelReceive(x[1])
               [] a = "KernelNoBuffers" -> KernelNoBuffers
               [] a = "KernelCancel" -> KernelCancel
               [] a = "Stop" -> Stop
               [] a = "Arm" -> Arm
               [] a = "Complete" -> Complete(x[1])
               [] a = "Drain" -> Drain(x[1])
               [] a = "Done" -> Done

TraceInit ==
    /\ Init
    /\ ti = 1
    /\ rejected = (Trace[1].op # "init" \/ Trace[1].act # "init" \/ Trace[1].args # <<>>
                   \/ VarsRec # Trace[1].state)

TraceStep ==
    /\ ~rejected
    /\ ti < Len(Trace)
    /\ LET r == Trace[ti + 1]
       IN /\ r.op \in DOMAIN OpActs
          /\ r.act \in OpActs[r.op]
          /\ Act(r.act, r.args)
          /\ Next
          /\ VarsRec' = r.state
    /\ ti' = ti + 1
    /\ UNCHANGED rejected

TraceReject ==
    /\ ~rejected
    /\ ti < Len(Trace)
    /\ ~ENABLED TraceStep
    /\ rejected' = TRUE
    /\ UNCHANGED <<vars, ti>>

TraceDone ==
    /\ rejected \/ ti = Len(Trace)
    /\ UNCHANGED <<vars, ti, rejected>>

TraceSpec == TraceInit /\ [][TraceStep \/ TraceReject \/ TraceDone]_<<vars, ti, rejected>>

\* every record of the trace is the model step it names, from the one before
TraceConsistent == ~rejected

\* the row's reach predicates: some clean trace fills the bank (BankFull)
\* and runs the buffer ring dry (RingDry)
BankFull == tail - head = S
RingDry == brTail = brHead

=============================================================================
