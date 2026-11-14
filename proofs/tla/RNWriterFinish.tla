--------------------------- MODULE RNWriterFinish ---------------------------
(***************************************************************************)
(* Who may touch a client's send state, and when, once a writer finishes  *)
(* its own packets (sdk/rocketnet_message_server.cpp,                     *)
(* rn_message_object_start, rn_message_object_finish_client behind        *)
(* rocketnet_server_finish_client_message, rn_message_object_abort).      *)
(*                                                                         *)
(* A frame alternates a main-thread operation (send_packets, the send      *)
(* pass over the clients) with a fill phase in which writers start,       *)
(* finish and abort messages on clients. In a fill phase a client's send  *)
(* state (its open start, its packet build, its queues) is touched by one *)
(* writer only: the first to start on it owns it until the phase ends     *)
(* (RNFillPhase OneFiller; the code refuses a second filler).             *)
(*                                                                         *)
(* A start is of one of three kinds, the caller's choice:                 *)
(*   StartSmall     an unreliable message one packet carries. It takes a  *)
(*                  frame of the update's capacity, packed ("inplace") or *)
(*                  a frame of its own ("whole", the form a start takes   *)
(*                  when the bucket pays a single-form frame and not a    *)
(*                  packed one); or it is held behind an unreliable       *)
(*                  message already queued on its client ("behind", the   *)
(*                  order rule); or it finds no capacity ("nocap"; on a   *)
(*                  buffered transport "ring", below). With a send budget *)
(*                  (Budgeted) the bucket may refuse it while frames are  *)
(*                  left.                                                *)
(*   StartLarge     an unreliable reservation over one packet: the       *)
(*                  channel route, the form decided at finish ("large").  *)
(*   StartReliable  a reliable message ("reliable").                      *)
(* The writer's finish then, by what the start found:                     *)
(*   Finish         a frame: sealed and handed to the transport in the    *)
(*                  call (SealAtFinish), not left open; no capacity:     *)
(*                  refused and counted (RefuseNoCapacity), not queued;   *)
(*                  behind or ring: queued for send_packets; reliable:    *)
(*                  held on the reliable path for send_packets.          *)
(*   FinishOver     a large start finished over one packet: promoted to  *)
(*                  its reliable twin and held.                           *)
(*   FinishRestart  a large start finished at a size one packet carries:  *)
(*                  a small unreliable message, so the reservation is     *)
(*                  released and the start's decision made again at that *)
(*                  size, then settled as above (RestartAtFinish).        *)
(*   FinishBadBytes bytes_written outside 1..reserved: the start aborted, *)
(*                  refused as the caller's error whatever it found, never *)
(*                  counted as no capacity (ValidateFirst).               *)
(*   FinishLost     a frame whose seal sends nothing: the seal's          *)
(*                  over-budget drop, a failed record add, a failed      *)
(*                  finish_packet, each counted. The code keeps these     *)
(*                  defensively; a walk through the API cannot reach them *)
(*                  (tools/tla_trace_check.py names why).                 *)
(*   Abort          the start aborted; a frame it held goes back to the   *)
(*                  transport, but the update's capacity stays charged:   *)
(*                  charged per frame start, kept on abort (RNServerSendFair *)
(*                  FirstVisit), as for a bad finish and a lost seal.     *)
(* So send_packets, the main thread's walk, carries only what the header  *)
(* says it carries (sdk/rocketnet.h, rocketnet_server_finish_client_message): *)
(* a reliable or larger message, one held behind a queued message, one   *)
(* the ring had no slot for. On a staged transport no plain unreliable    *)
(* message is ever queued (StagedQueuesNothing), no packet is left open   *)
(* (SealedInCall), and every start is sent, refused, carried, cancelled   *)
(* or lost once (Accounted). The walk carries what the update's capacity, *)
(* the ring and the bucket allow; the rest waits for the next walk.       *)
(* "walked" is what left the queues for the send pass, not what reached   *)
(* the client: Accounted is the writers' settlement, not delivery.        *)
(*                                                                         *)
(* Scope: the model's unreliable messages are plain                       *)
(* ROCKETNET_RELIABILITY_UNRELIABLE, the trace walk's workload, and the   *)
(* one cause of "behind" it holds is an unreliable message already queued *)
(* on the client. An UNRELIABLE_SEQUENCED message is also held behind its *)
(* channel's queued reliable work or the ordered resend list              *)
(* (rn_endpoint_in_place_header, behind_queue in rn_message_object_start) *)
(* and so queued on a staged transport, as the header's order exception   *)
(* allows, including after a short finish's restart. That cause is not    *)
(* modelled or traced, and StagedQueuesNothing does not cover it.         *)
(*                                                                         *)
(* Buffered: the transport sends through the server's own ring, sized one  *)
(* slot a client, all of it the round the main thread's send keeps. There  *)
(* a start the ring had no slot for is not a refusal: it is queued and the *)
(* main thread's send carries it, as the serial finish's is, so nothing is *)
(* lost (BufferedNeverRefuses, Accounted). With Buffered the model's      *)
(* capacity is the ring's alone: a start past the update's own send limit *)
(* or its budget is still refused on such a transport, and is the         *)
(* Budgeted staged instance's case.                                       *)
(*                                                                         *)
(* Reversed witnesses: MCRNWriterFinishBrokenNoOwner (no ownership check)  *)
(* breaks OneWriter; MCRNWriterFinishBrokenNotSealed (SealAtFinish FALSE,  *)
(* the serial finish's open build) breaks SealedInCall;                   *)
(* MCRNWriterFinishBrokenQueuesNoCapacity (RefuseNoCapacity FALSE) and     *)
(* MCRNWriterFinishBrokenDowngrade (RestartAtFinish FALSE: the large      *)
(* start finished short queued as unreliable, the defect this restart     *)
(* fixes) break StagedQueuesNothing; MCRNWriterFinishBrokenCountsBadBytes *)
(* (ValidateFirst FALSE: a bad finish of a no-capacity start counted as   *)
(* no capacity) breaks BadBytesNeverRefused;                              *)
(* MCRNWriterFinishBrokenRingRefuses (the ring's shortfall marked as no    *)
(* capacity) breaks BufferedNeverRefuses; MCRNWriterFinishBrokenWalkDrops   *)
(* (KeepLeft FALSE: the walk clears what it did not carry, uncounted)       *)
(* breaks Accounted.                                                       *)
(***************************************************************************)
EXTENDS Naturals, FiniteSets

CONSTANTS Writers, Clients, NoOwner, Capacity, MaxStarts,
          SealAtFinish, RefuseNoCapacity, CheckOwner,
          Buffered,        \* the transport sends through the server's own ring (the in-memory and plain socket transports)
          RingRefuses,     \* reversed witness: a start the ring had no slot for is marked as having no capacity
          Budgeted,        \* a send budget: the bucket may refuse a small start, or pay only a frame of its own
          RestartAtFinish, \* FALSE, reversed witness: a large start finished short is queued as unreliable
          ValidateFirst    \* FALSE, reversed witness: a bad finish of a no-capacity start is counted as no capacity

VARIABLES phase,        \* "main" or "fill"
          owner,        \* client -> the writer that owns it this fill phase, or NoOwner
          touched,      \* client -> the writers that touched its send state this fill phase
          open,         \* client -> the start open on it (OpenStates)
          build,        \* client -> 1 while an in-place packet is held open, unsealed
          queued,       \* client -> writer messages in its unreliable queue, for the walk
          held,         \* client -> writer messages on its reliable path not yet taken by the walk
          cap,          \* frames of the update's capacity left
          starts,       \* writer starts so far (bounds the model)
          sent,         \* writer messages sealed and handed off by their writer
          refused,      \* writer messages refused at finish for want of capacity
          cancelled,    \* writer starts aborted, or refused for a bad bytes_written
          lost,         \* writer messages whose seal sent nothing (FinishLost)
          walked        \* writer messages the walk took

vars == << phase, owner, touched, open, build, queued, held, cap, starts, sent, refused, cancelled, lost, walked >>

OpenStates == {"none", "inplace", "whole", "nocap", "ring", "behind", "large", "reliable"}
Framed == {"inplace", "whole"}                   \* the starts that hold a frame of the capacity

TypeOK ==
    /\ phase \in {"main", "fill"}
    /\ owner \in [Clients -> Writers \cup {NoOwner}]
    /\ touched \in [Clients -> SUBSET Writers]
    /\ open \in [Clients -> OpenStates]
    /\ build \in [Clients -> 0..1]
    /\ queued \in [Clients -> Nat]
    /\ held \in [Clients -> Nat]
    /\ cap \in 0..Capacity
    /\ starts \in 0..MaxStarts
    /\ sent \in Nat /\ refused \in Nat /\ cancelled \in Nat /\ lost \in Nat /\ walked \in Nat

Init ==
    /\ phase = "main"
    /\ owner = [c \in Clients |-> NoOwner]
    /\ touched = [c \in Clients |-> {}]
    /\ open = [c \in Clients |-> "none"]
    /\ build = [c \in Clients |-> 0]
    /\ queued = [c \in Clients |-> 0]
    /\ held = [c \in Clients |-> 0]
    /\ cap = Capacity
    /\ starts = 0 /\ sent = 0 /\ refused = 0 /\ cancelled = 0 /\ lost = 0 /\ walked = 0

\* the fill phase opens: every client unowned
BeginFill ==
    /\ phase = "main"
    /\ phase' = "fill"
    /\ owner' = [c \in Clients |-> NoOwner]
    /\ touched' = [c \in Clients |-> {}]
    /\ UNCHANGED << open, build, queued, held, cap, starts, sent, refused, cancelled, lost, walked >>

\* the fill-phase check: the first writer owns the client; another is refused
MayTouch(w, c) == ~CheckOwner \/ owner[c] \in {NoOwner, w}

\* a writer's call on c: the claim and the history
Claim(w, c) ==
    /\ phase = "fill"
    /\ MayTouch(w, c)
    /\ owner' = [owner EXCEPT ![c] = w]
    /\ touched' = [touched EXCEPT ![c] = @ \cup {w}]

\* what a small unreliable start finds on c now: behind a queued unreliable
\* message (the order rule, ahead of capacity); a frame while one is left,
\* the bucket permitting; otherwise no capacity, or on a buffered transport
\* the ring's shortfall
NoRoom == IF Buffered /\ ~RingRefuses THEN "ring" ELSE "nocap"
SmallOutcomes(c) ==
    IF queued[c] > 0 THEN {"behind"}
    ELSE IF cap > 0 /\ build[c] = 0
         THEN {"inplace"} \cup (IF Budgeted THEN {"whole", "nocap"} ELSE {})
         ELSE {NoRoom}

TakesFrame(o) == IF o \in Framed THEN 1 ELSE 0

StartAs(w, c, o) ==
    /\ Claim(w, c)
    /\ starts < MaxStarts
    /\ open[c] = "none"
    /\ starts' = starts + 1
    /\ cap' = cap - TakesFrame(o)
    /\ UNCHANGED << phase, build, queued, held, sent, refused, cancelled, lost, walked >>

StartSmall(w, c) == \E o \in SmallOutcomes(c) : StartAs(w, c, o) /\ open' = [open EXCEPT ![c] = o]
StartLarge(w, c) == StartAs(w, c, "large") /\ open' = [open EXCEPT ![c] = "large"]
StartReliable(w, c) == StartAs(w, c, "reliable") /\ open' = [open EXCEPT ![c] = "reliable"]

\* a valid finish of a start that found o: what happens to the message
Settle(c, o) ==
    CASE o \in Framed ->
            IF SealAtFinish \/ o = "whole"
               THEN /\ sent' = sent + 1
                    /\ UNCHANGED << build, queued, held, refused >>
               ELSE /\ build' = [build EXCEPT ![c] = 1]
                    /\ UNCHANGED << queued, held, sent, refused >>
      [] o = "nocap" ->
            IF RefuseNoCapacity
               THEN /\ refused' = refused + 1
                    /\ UNCHANGED << build, queued, held, sent >>
               ELSE /\ queued' = [queued EXCEPT ![c] = @ + 1]
                    /\ UNCHANGED << build, held, sent, refused >>
      [] o \in {"ring", "behind"} ->
            /\ queued' = [queued EXCEPT ![c] = @ + 1]
            /\ UNCHANGED << build, held, sent, refused >>
      [] o \in {"reliable", "large"} ->
            /\ held' = [held EXCEPT ![c] = @ + 1]
            /\ UNCHANGED << build, queued, sent, refused >>

Close(w, c) ==
    /\ Claim(w, c)
    /\ open' = [open EXCEPT ![c] = "none"]

Finish(w, c) ==
    /\ open[c] \notin {"none", "large"}
    /\ Close(w, c)
    /\ Settle(c, open[c])
    /\ UNCHANGED << phase, cap, starts, cancelled, lost, walked >>

\* a large start finished over one packet: its reliable twin, held
FinishOver(w, c) ==
    /\ open[c] = "large"
    /\ Close(w, c)
    /\ Settle(c, "large")
    /\ UNCHANGED << phase, cap, starts, cancelled, lost, walked >>

\* a large start finished at a size one packet carries: the reservation
\* released and the start made again at that size, then settled
FinishRestart(w, c) ==
    /\ open[c] = "large"
    /\ Close(w, c)
    /\ IF RestartAtFinish
          THEN \E o \in SmallOutcomes(c) :
                  /\ cap' = cap - TakesFrame(o)
                  /\ Settle(c, o)
          ELSE /\ queued' = [queued EXCEPT ![c] = @ + 1]
               /\ UNCHANGED << cap, build, held, sent, refused >>
    /\ UNCHANGED << phase, starts, cancelled, lost, walked >>

\* bytes_written outside 1..reserved: the start aborted, its frame back
FinishBadBytes(w, c) ==
    /\ open[c] # "none"
    /\ Close(w, c)
    /\ UNCHANGED cap
    /\ IF ~ValidateFirst /\ open[c] = "nocap"
          THEN /\ refused' = refused + 1
               /\ UNCHANGED cancelled
          ELSE /\ cancelled' = cancelled + 1
               /\ UNCHANGED refused
    /\ UNCHANGED << phase, build, queued, held, starts, sent, lost, walked >>

\* a frame whose seal sends nothing: the frame spent, or given back
FinishLost(w, c) ==
    /\ open[c] \in Framed
    /\ Close(w, c)
    /\ lost' = lost + 1
    /\ UNCHANGED cap
    /\ UNCHANGED << phase, build, queued, held, starts, sent, refused, cancelled, walked >>

Abort(w, c) ==
    /\ open[c] # "none"
    /\ Close(w, c)
    /\ UNCHANGED cap
    /\ cancelled' = cancelled + 1
    /\ UNCHANGED << phase, build, queued, held, starts, sent, refused, lost, walked >>

\* send_packets on the main thread: the fill is joined (no start open) and
\* the walk takes what a writer left: an open build always (none is left,
\* SealedInCall), and of the queued and held messages what the update's
\* capacity, the ring's slots and the bucket carry; the rest waits for the
\* next walk. How much the walk carries, and in what order, is not this
\* model's (RNSendRotation and RNServerSendFair hold the walk's rounds): here
\* it may carry anything from all to none, a safety bound only. The next
\* update's capacity opens.
\* A main-thread operation ends the phase's claims (RNFillPhase MainEpoch: the
\* epoch advances at its end), so no client is owned after it; touched, the
\* history OneWriter reads, stays until the next phase opens
\* KeepLeft: what the walk does not carry stays queued or held for the next
\* walk (the code leaves its records in place). A definition, so the reversed
\* witness MCRNWriterFinishBrokenWalkDrops.cfg can replace it with FALSE: the
\* walk clears what it did not carry without counting it, and Accounted fails
KeepLeft == TRUE
Left(c) == build[c] + queued[c] + held[c]
Total(f) == LET Sum[S \in SUBSET Clients] ==
                    IF S = {} THEN 0
                    ELSE LET c == CHOOSE x \in S : TRUE IN f[c] + Sum[S \ {c}]
            IN Sum[Clients]
\* the per-client counts each at most f's
Range(f) == {f[c] : c \in Clients}
AtMost(f) == LET top == CHOOSE m \in Range(f) : \A c \in Clients : f[c] <= m
             IN {g \in [Clients -> 0..top] : \A c \in Clients : g[c] <= f[c]}
SendPackets ==
    /\ phase = "fill"
    /\ \A c \in Clients : open[c] = "none"
    /\ phase' = "main"
    /\ owner' = [c \in Clients |-> NoOwner]
    /\ build' = [c \in Clients |-> 0]
    /\ \E q \in AtMost(queued), h \in AtMost(held) :
          /\ queued' = IF KeepLeft THEN q ELSE [c \in Clients |-> 0]
          /\ held' = IF KeepLeft THEN h ELSE [c \in Clients |-> 0]
          /\ walked' = walked + Total([c \in Clients |-> Left(c)]) - Total(q) - Total(h)
    /\ cap' = Capacity
    /\ UNCHANGED << touched, open, starts, sent, refused, cancelled, lost >>

Next ==
    \/ BeginFill
    \/ \E w \in Writers, c \in Clients :
          \/ StartSmall(w, c) \/ StartLarge(w, c) \/ StartReliable(w, c)
          \/ Finish(w, c) \/ FinishOver(w, c) \/ FinishRestart(w, c)
          \/ FinishBadBytes(w, c) \/ FinishLost(w, c) \/ Abort(w, c)
    \/ SendPackets

Spec == Init /\ [][Next]_vars

\* one writer per client per fill phase
OneWriter == \A c \in Clients : Cardinality(touched[c]) <= 1

\* the writer's finish leaves no packet open for the walk
SealedInCall == \A c \in Clients : build[c] = 0

\* on a staged transport no plain unreliable message waits for the walk: a
\* small one is sealed or refused in the call, and nothing starts the queue
\* a message would be held behind (a sequenced message held behind its
\* channel's reliable work is out of this model's scope, above)
StagedQueuesNothing == ~Buffered => \A c \in Clients : queued[c] = 0

\* on a buffered transport the ring holds one round, the main thread's: a
\* start it had no slot for waits for that round and is never refused
BufferedNeverRefuses == Buffered => refused = 0

\* every writer start is open, waiting for the walk, or was sent, refused,
\* taken by the walk, cancelled or lost, once
OpenCount == Cardinality({c \in Clients : open[c] # "none"})
Accounted == starts = sent + refused + walked + cancelled + lost + OpenCount + Total([c \in Clients |-> Left(c)])

\* a bad bytes_written is never counted as no capacity. With ValidateFirst
\* it holds by FinishBadBytes's own text: its force on the code is the trace
\* walk's, which checks every bad finish returned 0 and moved no refusal
\* counter, and the trace check, which rejects a refused' that moved
BadBytesNeverRefused == [][\A w \in Writers, c \in Clients : FinishBadBytes(w, c) => refused' = refused]_vars
=============================================================================
