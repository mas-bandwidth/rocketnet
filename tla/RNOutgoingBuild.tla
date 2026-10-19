-------------------------- MODULE RNOutgoingBuild --------------------------
(***************************************************************************)
(* The outgoing build of one connection (docs/OUTGOING-BUILD.md): how a    *)
(* message the game starts, writes and finishes reaches the transport.     *)
(*                                                                         *)
(* A fitting unreliable message (UNRELIABLE, its receipt twin, and         *)
(* UNRELIABLE_SEQUENCED, every header and the form byte inside one wire    *)
(* frame) is written in place: start takes a transport frame and hands     *)
(* out a pointer inside it, finish seals and stages that same frame        *)
(* (sdk/rocketnet_message_server.cpp, rn_message_object_start and          *)
(* rn_message_object_finish; sdk/rocketnet_endpoint.cpp,                   *)
(* rn_endpoint_in_place_header and rn_endpoint_in_place_finish).  Every    *)
(* other message -- a reliable one, an oversize unreliable one, and a      *)
(* fitting unreliable one that fell back because the transport gave no     *)
(* frame or unreliable messages already wait in the queue -- is written    *)
(* into its region record and copied into a frame by the write pass in     *)
(* send_packets (rn_message_connection_write).                             *)
(*                                                                         *)
(* Buffers have identities: frames 1..NumFrames, and the region record of  *)
(* message m is NumFrames + m.  A message's bytes are one value, Data(m),  *)
(* which the game writes once.  Acks are the transport's: each packet      *)
(* sealed carries the acks due at its seal.                                *)
(*                                                                         *)
(* CopyStep is the rule switch of the reversed witness: TRUE restores the  *)
(* build before this change, in which a fitting unreliable message is      *)
(* written into its region record and copied into the frame at             *)
(* send_packets.                                                           *)
(***************************************************************************)
EXTENDS Naturals, Sequences, FiniteSets

CONSTANTS
    N,              \* messages 1..N, started in this order (the game's send order)
    Kind,           \* Kind[m] \in {"U", "S", "R"}: unreliable (with or without receipt), sequenced, reliable
    Fits,           \* Fits[m]: every header and the form byte fit one wire frame
    NumFrames,      \* the frames the transport can have out at once
    MaxPackets,     \* the queued and in-place packet allowance in one update
    MaxAcks,        \* the acks the peer makes due in a run
    MaxFaults,      \* the faults in a run: a frame refused at start, an abort, an over-budget drop
    CopyStep        \* TRUE: the copy step of the build before this change

Msgs == 1..N
Frames == 1..NumFrames
None == 0
Region(m) == NumFrames + m
Data(m) == m
Unreliable == {"U", "S"}
FitUnr(m) == Kind[m] \in Unreliable /\ Fits[m]
Outcomes == {"none", "started", "queued", "staged", "sent", "dropped", "aborted"}

VARIABLES
    next,           \* the next message the game starts
    pending,        \* the message whose start is pending, or None
    handed,         \* handed[m]: the buffer start returned for m, or None
    fellback,       \* fellback[m]: a fitting unreliable m took the queued path (no frame, or unreliable queued ahead)
    frameRecs,      \* frameRecs[f]: the records in frame f, a sequence of [msg, data]
    regionData,     \* regionData[m]: the bytes in m's region record (0 unwritten)
    frameFree,      \* frames the transport can hand out now
    queue,          \* the queued path: finished messages waiting for the write pass, in finish order
    staged,         \* sealed packets waiting for the flush: [buf, recs, acks]
    wire,           \* packets the transport sent, in order
    acksDue,        \* acks due and not yet carried
    acksMade,       \* acks the peer made due in all
    finished,       \* messages finished (not aborted), in finish order
    faults,         \* the faults so far
    outcome,
    packetsThisUpdate \* queued and in-place packet attempts since BeginUpdate

vars == << next, pending, handed, fellback, frameRecs, regionData, frameFree, queue,
           staged, wire, acksDue, acksMade, finished, faults, outcome, packetsThisUpdate >>

Rec == [msg : Msgs, data : 0..N]
Packet == [buf : Frames, recs : Seq(Rec), acks : 0..MaxAcks]

TypeOK ==
    /\ next \in 1..(N + 1)
    /\ pending \in Msgs \cup {None}
    /\ handed \in [Msgs -> Frames \cup {Region(m) : m \in Msgs} \cup {None}]
    /\ fellback \in [Msgs -> BOOLEAN]
    /\ frameRecs \in [Frames -> Seq(Rec)]
    /\ regionData \in [Msgs -> 0..N]
    /\ frameFree \subseteq Frames
    /\ queue \in Seq(Msgs)
    /\ staged \in Seq(Packet)
    /\ wire \in Seq(Packet)
    /\ acksDue \in 0..MaxAcks
    /\ acksMade \in 0..MaxAcks
    /\ finished \in Seq(Msgs)
    /\ faults \in 0..MaxFaults
    /\ outcome \in [Msgs -> Outcomes]
    /\ packetsThisUpdate \in Nat

Init ==
    /\ next = 1
    /\ pending = None
    /\ handed = [m \in Msgs |-> None]
    /\ fellback = [m \in Msgs |-> FALSE]
    /\ frameRecs = [f \in Frames |-> << >>]
    /\ regionData = [m \in Msgs |-> 0]
    /\ frameFree = Frames
    /\ queue = << >>
    /\ staged = << >>
    /\ wire = << >>
    /\ acksDue = 0
    /\ acksMade = 0
    /\ finished = << >>
    /\ faults = 0
    /\ outcome = [m \in Msgs |-> "none"]
    /\ packetsThisUpdate = 0

UnreliableQueued == \E i \in 1..Len(queue) : Kind[queue[i]] \in Unreliable

\* start: a fitting unreliable message takes a frame when the transport has
\* one and no unreliable message waits ahead of it in the queue.
StartInPlace ==
    /\ next <= N /\ pending = None
    /\ ~CopyStep
    /\ FitUnr(next)
    /\ packetsThisUpdate < MaxPackets
    /\ ~UnreliableQueued
    /\ \E f \in frameFree :
        /\ frameFree' = frameFree \ {f}
        /\ handed' = [handed EXCEPT ![next] = f]
        /\ frameRecs' = [frameRecs EXCEPT ![f] = << [msg |-> next, data |-> 0] >>]
    /\ pending' = next
    /\ next' = next + 1
    /\ outcome' = [outcome EXCEPT ![next] = "started"]
    /\ UNCHANGED << fellback, regionData, queue, staged, wire, acksDue, acksMade, finished, faults, packetsThisUpdate >>

\* start on the queued path: a reliable or oversize message always; a fitting
\* unreliable one when the transport gave no frame (it may refuse at any
\* time: a full ring, a client not yet connected), when unreliable messages
\* wait ahead of it, or, under CopyStep, always.
StartQueued ==
    /\ next <= N /\ pending = None
    /\ LET refused == FitUnr(next) /\ ~CopyStep /\ frameFree # {} /\ ~UnreliableQueued IN
       /\ refused => faults < MaxFaults
       /\ faults' = IF refused THEN faults + 1 ELSE faults
    /\ handed' = [handed EXCEPT ![next] = Region(next)]
    /\ fellback' = [fellback EXCEPT ![next] = FitUnr(next) /\ ~CopyStep]
    /\ pending' = next
    /\ next' = next + 1
    /\ outcome' = [outcome EXCEPT ![next] = "started"]
    /\ UNCHANGED << frameRecs, regionData, frameFree, queue, staged, wire, acksDue, acksMade, finished, packetsThisUpdate >>

\* the game writes its bytes at the pointer start returned, once
Write ==
    /\ pending # None
    /\ LET m == pending IN
       IF handed[m] \in Frames
       THEN /\ frameRecs[handed[m]][1].data = 0
            /\ frameRecs' = [frameRecs EXCEPT ![handed[m]] = << [msg |-> m, data |-> Data(m)] >>]
            /\ UNCHANGED regionData
       ELSE /\ regionData[m] = 0
            /\ regionData' = [regionData EXCEPT ![m] = Data(m)]
            /\ UNCHANGED frameRecs
    /\ UNCHANGED << next, pending, handed, fellback, frameFree, queue, staged, wire, acksDue, acksMade, finished, faults, outcome, packetsThisUpdate >>

Written(m) == IF handed[m] \in Frames THEN frameRecs[handed[m]][1].data # 0 ELSE regionData[m] # 0

\* finish of an in-place message: the header and form byte are written
\* around the bytes and the same frame is sealed and staged; the acks due
\* ride in it (finish_packet writes the ack block)
FinishInPlace ==
    /\ pending # None /\ handed[pending] \in Frames /\ Written(pending)
    /\ LET m == pending
           f == handed[m] IN
       /\ staged' = Append(staged, [buf |-> f, recs |-> frameRecs[f], acks |-> acksDue])
       /\ acksDue' = 0
       /\ finished' = Append(finished, m)
       /\ outcome' = [outcome EXCEPT ![m] = "staged"]
    /\ packetsThisUpdate' = packetsThisUpdate + 1
    /\ pending' = None
    /\ UNCHANGED << next, handed, fellback, frameRecs, regionData, frameFree, queue, wire, acksMade, faults >>

\* finish of an in-place message the send budget cannot pay: dropped and
\* counted (MESSAGE_UNRELIABLE_OVER_BUDGET), the frame given back
FinishInPlaceOverBudget ==
    /\ pending # None /\ handed[pending] \in Frames /\ Written(pending)
    /\ faults < MaxFaults
    /\ faults' = faults + 1
    /\ LET m == pending IN
       /\ frameFree' = frameFree \cup {handed[m]}
       /\ frameRecs' = [frameRecs EXCEPT ![handed[m]] = << >>]
       /\ finished' = Append(finished, m)
       /\ outcome' = [outcome EXCEPT ![m] = "dropped"]
    /\ pending' = None
    /\ UNCHANGED << next, handed, fellback, regionData, queue, staged, wire, acksDue, acksMade, packetsThisUpdate >>

FinishQueued ==
    /\ pending # None /\ handed[pending] \notin Frames /\ Written(pending)
    /\ queue' = Append(queue, pending)
    /\ finished' = Append(finished, pending)
    /\ outcome' = [outcome EXCEPT ![pending] = "queued"]
    /\ pending' = None
    /\ UNCHANGED << next, handed, fellback, frameRecs, regionData, frameFree, staged, wire, acksDue, acksMade, faults, packetsThisUpdate >>

\* abort: an in-place frame goes back to the transport (abort_packet); a
\* region record's room goes back to the region
Abort ==
    /\ pending # None
    /\ faults < MaxFaults
    /\ faults' = faults + 1
    /\ IF handed[pending] \in Frames
       THEN /\ frameFree' = frameFree \cup {handed[pending]}
            /\ frameRecs' = [frameRecs EXCEPT ![handed[pending]] = << >>]
            /\ UNCHANGED regionData
       ELSE /\ regionData' = [regionData EXCEPT ![pending] = 0]
            /\ UNCHANGED << frameFree, frameRecs >>
    /\ outcome' = [outcome EXCEPT ![pending] = "aborted"]
    /\ pending' = None
    /\ UNCHANGED << next, handed, fellback, queue, staged, wire, acksDue, acksMade, finished, packetsThisUpdate >>

\* the write pass, one packet: the first k queued records copied from their
\* region records into a frame, sealed and staged with the acks due.  A k
\* below the queue's length is the packet full: the rest wait for the next
\* packet (one record, or the whole queue, in this model).  No in-place start is pending here: the transport forbids a
\* flush with a fill open, so the object aborts it first (Abort).
SendPackets ==
    /\ queue # << >>
    /\ packetsThisUpdate < MaxPackets
    /\ IF pending = None THEN TRUE ELSE handed[pending] \notin Frames
    /\ \E f \in frameFree, k \in {1, Len(queue)} :
        LET recs == [i \in 1..k |-> [msg |-> queue[i], data |-> regionData[queue[i]]]] IN
        /\ frameFree' = frameFree \ {f}
        /\ frameRecs' = [frameRecs EXCEPT ![f] = recs]
        /\ staged' = Append(staged, [buf |-> f, recs |-> recs, acks |-> acksDue])
        /\ queue' = SubSeq(queue, k + 1, Len(queue))
        /\ regionData' = [m \in Msgs |-> IF \E i \in 1..k : queue[i] = m THEN 0 ELSE regionData[m]]
        /\ outcome' = [m \in Msgs |-> IF \E i \in 1..k : queue[i] = m THEN "staged" ELSE outcome[m]]
    /\ acksDue' = 0
    /\ packetsThisUpdate' = packetsThisUpdate + 1
    /\ UNCHANGED << next, pending, handed, fellback, wire, acksMade, finished, faults >>

\* the transport sends the oldest staged packet and its frame is free again
TransportSend ==
    /\ staged # << >>
    /\ wire' = Append(wire, Head(staged))
    /\ staged' = Tail(staged)
    /\ frameFree' = frameFree \cup {Head(staged).buf}
    /\ frameRecs' = [frameRecs EXCEPT ![Head(staged).buf] = << >>]
    /\ outcome' = [m \in Msgs |-> IF \E i \in 1..Len(Head(staged).recs) : Head(staged).recs[i].msg = m THEN "sent" ELSE outcome[m]]
    /\ UNCHANGED << next, pending, handed, fellback, regionData, queue, acksDue, acksMade, finished, faults, packetsThisUpdate >>

\* the peer's packets make acks due
BeginUpdate ==
    /\ pending = None
    /\ packetsThisUpdate > 0
    /\ packetsThisUpdate' = 0
    /\ UNCHANGED << next, pending, handed, fellback, frameRecs, regionData, frameFree, queue, staged, wire, acksDue, acksMade, finished, faults, outcome >>

AckDue ==
    /\ acksMade < MaxAcks
    /\ acksDue' = acksDue + 1
    /\ acksMade' = acksMade + 1
    /\ UNCHANGED << next, pending, handed, fellback, frameRecs, regionData, frameFree, queue, staged, wire, finished, faults, outcome, packetsThisUpdate >>

Next ==
    \/ StartInPlace \/ StartQueued \/ Write
    \/ FinishInPlace \/ FinishInPlaceOverBudget \/ FinishQueued \/ Abort
    \/ SendPackets \/ TransportSend \/ AckDue \/ BeginUpdate

Spec == Init /\ [][Next]_vars

-----------------------------------------------------------------------------
\* the packets the transport has: staged, then sent

Out == wire \o staged

RECURSIVE SumAcks(_)
SumAcks(s) == IF s = << >> THEN 0 ELSE Head(s).acks + SumAcks(Tail(s))

RECURSIVE Flat(_)
Flat(s) == IF s = << >> THEN << >> ELSE [i \in 1..Len(Head(s).recs) |-> Head(s).recs[i].msg] \o Flat(Tail(s))

OutMsgs == Flat(wire) \o Flat(staged)

OfKind(s, k) == SelectSeq(s, LAMBDA m : Kind[m] = k)
Delivered(s) == SelectSeq(s, LAMBDA m : outcome[m] # "dropped")

IsPrefix(a, b) == Len(a) <= Len(b) /\ a = SubSeq(b, 1, Len(a))

\* every byte the game wrote reaches the transport unchanged
PacketQuota == packetsThisUpdate <= MaxPackets

BytesUnchangedInPlace ==
    \A i \in 1..Len(Out) : \A j \in 1..Len(Out[i].recs) :
        Out[i].recs[j].data = Data(Out[i].recs[j].msg)

\* a fitting unreliable message that did not fall back is in the very
\* buffer start handed out: buffer identity, not equal contents
NoCopy ==
    \A i \in 1..Len(Out) : \A j \in 1..Len(Out[i].recs) :
        LET m == Out[i].recs[j].msg IN
        FitUnr(m) /\ ~fellback[m] => Out[i].buf = handed[m]

\* every ack made due is still due or rides a staged or sent packet
AcksNeverLost == acksMade = acksDue + SumAcks(wire) + SumAcks(staged)

\* no message is lost or duplicated, and each mode keeps its order: within
\* each kind the messages the transport has are a prefix of that kind's
\* finished, undropped messages in finish order (the sequenced drop rule is
\* the receiver's and starts from this order); every finished, undropped
\* message is queued or with the transport
ModeOrder ==
    /\ \A k \in {"U", "S", "R"} : IsPrefix(OfKind(OutMsgs, k), OfKind(Delivered(finished), k))
    /\ \A i \in 1..Len(finished) :
        LET m == finished[i] IN
        outcome[m] # "dropped" => (\E a \in 1..Len(queue) : queue[a] = m) \/ (\E b \in 1..Len(OutMsgs) : OutMsgs[b] = m)
    /\ \A a, b \in 1..Len(OutMsgs) : a # b => OutMsgs[a] # OutMsgs[b]
=============================================================================
