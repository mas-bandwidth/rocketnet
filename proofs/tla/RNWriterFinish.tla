--------------------------- MODULE RNWriterFinish ---------------------------
(***************************************************************************)
(* Who may touch a client's send state, and when, once a writer finishes  *)
(* its own packets (sdk/rocketnet_message_server.cpp,                     *)
(* rn_message_object_finish_client behind                                 *)
(* rocketnet_server_finish_client_message).                               *)
(*                                                                         *)
(* A frame alternates a main-thread operation (send_packets, which walks   *)
(* every client) with a fill phase in which writers start and finish      *)
(* unreliable messages on clients. In a fill phase a client's send state  *)
(* (its open start, its packet build, its sequence) is touched by one     *)
(* writer only: the first to start on it owns it until the phase ends    *)
(* (RNFillPhase OneFiller; the code refuses a second filler). A start     *)
(* takes a frame of the update's capacity when one is left (in place) or  *)
(* finds none (no capacity). The writer's finish then:                    *)
(*   SealAtFinish      the in-place packet is sealed and handed to the    *)
(*                     transport in the finish, not left open;           *)
(*   RefuseNoCapacity  a start with no capacity is refused at the finish, *)
(*                     not queued.                                       *)
(* So send_packets, the main thread's walk, finishes nothing a writer      *)
(* wrote (NothingLeftForWalk), and every writer message is sent or        *)
(* refused once (Accounted).                                              *)
(*                                                                         *)
(* Reversed witness: MCRNWriterFinishBrokenNoOwner (no ownership check, a  *)
(* second writer on a client in one phase) breaks OneWriter. SealAtFinish  *)
(* and RefuseNoCapacity set FALSE describe the serial finish (the build    *)
(* left open, the no-capacity start queued); no committed run uses them.   *)
(***************************************************************************)
EXTENDS Naturals, FiniteSets

CONSTANTS Writers, Clients, NoOwner, Capacity, MaxStarts,
          SealAtFinish, RefuseNoCapacity, CheckOwner

VARIABLES phase,        \* "main" or "fill"
          owner,        \* client -> the writer that owns it this fill phase, or NoOwner
          touched,      \* client -> the writers that touched its send state this fill phase
          open,         \* client -> "none", "inplace" or "nocap": the start open on it
          build,        \* client -> 1 while an in-place packet is held open, unsealed
          queued,       \* client -> writer messages queued for the walk
          cap,          \* frames of the update's capacity left
          starts,       \* writer starts so far (bounds the model)
          sent,         \* writer messages sealed and handed off by their writer
          refused,      \* writer messages refused at finish
          walked        \* writer messages the walk finished

vars == << phase, owner, touched, open, build, queued, cap, starts, sent, refused, walked >>

TypeOK ==
    /\ phase \in {"main", "fill"}
    /\ owner \in [Clients -> Writers \cup {NoOwner}]
    /\ touched \in [Clients -> SUBSET Writers]
    /\ open \in [Clients -> {"none", "inplace", "nocap"}]
    /\ build \in [Clients -> 0..1]
    /\ queued \in [Clients -> Nat]
    /\ cap \in 0..Capacity
    /\ starts \in 0..MaxStarts
    /\ sent \in Nat /\ refused \in Nat /\ walked \in Nat

Init ==
    /\ phase = "main"
    /\ owner = [c \in Clients |-> NoOwner]
    /\ touched = [c \in Clients |-> {}]
    /\ open = [c \in Clients |-> "none"]
    /\ build = [c \in Clients |-> 0]
    /\ queued = [c \in Clients |-> 0]
    /\ cap = Capacity
    /\ starts = 0 /\ sent = 0 /\ refused = 0 /\ walked = 0

\* the fill phase opens: every client unowned
BeginFill ==
    /\ phase = "main"
    /\ phase' = "fill"
    /\ owner' = [c \in Clients |-> NoOwner]
    /\ touched' = [c \in Clients |-> {}]
    /\ UNCHANGED << open, build, queued, cap, starts, sent, refused, walked >>

\* the fill-phase check: the first writer owns the client; another is refused
MayTouch(w, c) == ~CheckOwner \/ owner[c] \in {NoOwner, w}

Start(w, c) ==
    /\ phase = "fill"
    /\ starts < MaxStarts
    /\ open[c] = "none"
    /\ MayTouch(w, c)
    /\ owner' = [owner EXCEPT ![c] = w]
    /\ touched' = [touched EXCEPT ![c] = @ \cup {w}]
    /\ starts' = starts + 1
    /\ IF cap > 0 /\ build[c] = 0
          THEN /\ open' = [open EXCEPT ![c] = "inplace"]
               /\ cap' = cap - 1
          ELSE /\ open' = [open EXCEPT ![c] = "nocap"]
               /\ cap' = cap
    /\ UNCHANGED << phase, build, queued, sent, refused, walked >>

Finish(w, c) ==
    /\ phase = "fill"
    /\ open[c] # "none"
    /\ MayTouch(w, c)
    /\ owner' = [owner EXCEPT ![c] = w]
    /\ touched' = [touched EXCEPT ![c] = @ \cup {w}]
    /\ open' = [open EXCEPT ![c] = "none"]
    /\ IF open[c] = "inplace"
          THEN IF SealAtFinish
                  THEN /\ sent' = sent + 1
                       /\ UNCHANGED << build, queued, refused >>
                  ELSE /\ build' = [build EXCEPT ![c] = 1]
                       /\ UNCHANGED << queued, sent, refused >>
          ELSE IF RefuseNoCapacity
                  THEN /\ refused' = refused + 1
                       /\ UNCHANGED << build, queued, sent >>
                  ELSE /\ queued' = [queued EXCEPT ![c] = @ + 1]
                       /\ UNCHANGED << build, sent, refused >>
    /\ UNCHANGED << phase, cap, starts, walked >>

\* send_packets on the main thread: the fill is joined (no start open), the
\* walk finishes whatever a writer left, and the next update's capacity opens
SendPackets ==
    /\ phase = "fill"
    /\ \A c \in Clients : open[c] = "none"
    /\ phase' = "main"
    /\ walked' = walked + ( LET Sum[S \in SUBSET Clients] ==
                                   IF S = {} THEN 0
                                   ELSE LET c == CHOOSE x \in S : TRUE
                                        IN build[c] + queued[c] + Sum[S \ {c}]
                             IN Sum[Clients] )
    /\ build' = [c \in Clients |-> 0]
    /\ queued' = [c \in Clients |-> 0]
    /\ cap' = Capacity
    /\ UNCHANGED << owner, touched, open, starts, sent, refused >>

Next ==
    \/ BeginFill
    \/ \E w \in Writers, c \in Clients : Start(w, c) \/ Finish(w, c)
    \/ SendPackets

Spec == Init /\ [][Next]_vars

\* one writer per client per fill phase
OneWriter == \A c \in Clients : Cardinality(touched[c]) <= 1

\* the walk never finishes a writer's message
NothingLeftForWalk == walked = 0 /\ \A c \in Clients : build[c] = 0 /\ queued[c] = 0

\* every finished writer message was sent or refused, once
OpenCount == Cardinality({c \in Clients : open[c] # "none"})
Accounted == phase = "main" => starts = sent + refused + walked + OpenCount
=============================================================================
