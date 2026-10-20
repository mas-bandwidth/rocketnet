------------------------- MODULE RNDriverSendStaging -------------------------
(***************************************************************************)
(* The staged send path of the server transport: the reserve/commit seam  *)
(* every driver serves in rocketnet_server_reserve_send_frame and          *)
(* rocketnet_server_commit_send_frame, the staged branch of                *)
(* rocketnet_server_transport_send_packets, maintenance in                 *)
(* rocketnet_server_transport_update, and out-of-band disconnects in       *)
(* rocketnet_server_transport_disconnect_client.                           *)
(*                                                                         *)
(* On a staged driver a committed frame seals straight into the driver's  *)
(* own staging and leaves only when the driver flushes.  The transport    *)
(* publishes staged work by counter: send_packets flushes exactly when    *)
(* the staged counter differs from the flushed counter.  The rule this    *)
(* model states is that every commit through the seam advances the staged *)
(* counter, disconnect bursts included, so the next send_packets after a  *)
(* disconnect flushes the burst.  DisconnectAdvancesStaged = FALSE        *)
(* removes that advance: disconnect_client seals its burst into the send  *)
(* buffer and calls send_packets, the counters still agree, nothing       *)
(* flushes, and the burst waits for a later, unrelated staged commit.     *)
(* That is the reversed witness, and TLC must reject it.                  *)
(*                                                                         *)
(* Frames are abstract wire packets.  A frame is fresh until it is        *)
(* reserved, committed, sent or dropped, so the small instance replays    *)
(* the whole life of a bounded set of frames.  One reservation is         *)
(* outstanding at a time: every commit through the seam runs on the main  *)
(* thread.  update's unconditional counter bump is a bare generation      *)
(* tick here; it stages no frame and strands none.                        *)
(***************************************************************************)
EXTENDS Naturals, FiniteSets

CONSTANTS MaxFrames,                \* the abstract frames are 1..MaxFrames
          MaxStaged,                \* the staging capacity, in frames
          MaxBurst,                 \* the most frames one disconnect seals
          DisconnectAdvancesStaged  \* commit through disconnect advances staged

ASSUME MaxFrames \in Nat \ {0} /\ MaxStaged \in 1..MaxFrames
ASSUME MaxBurst \in 1..MaxFrames /\ DisconnectAdvancesStaged \in BOOLEAN

Frames == 1..MaxFrames

VARIABLES staged,     \* the staged counter: frames committed, ever
          flushed,    \* the flushed counter: frames flushed, ever
          buffer,     \* the send buffer: committed frames not yet sent
          sent,       \* frames that have left by a flush
          dropped,    \* frames whose reserve failed on a full staging
          reserved,   \* the frame between reserve and commit; 0 for none
          discOwed,   \* disconnect frames committed and not yet sent
          ranSend,    \* the last action was send_packets
          tick        \* update's generation tick

vars == <<staged, flushed, buffer, sent, dropped, reserved, discOwed,
          ranSend, tick>>

Fresh == Frames \ (buffer \cup sent \cup dropped \cup {reserved})

Init ==
    /\ staged = 0
    /\ flushed = 0
    /\ buffer = {}
    /\ sent = {}
    /\ dropped = {}
    /\ reserved = 0
    /\ discOwed = {}
    /\ ranSend = FALSE
    /\ tick = 0

\* rocketnet_server_reserve_send_frame: take a staging slot for one frame
Reserve(f) ==
    /\ reserved = 0
    /\ f \in Fresh
    /\ Cardinality(buffer) < MaxStaged
    /\ reserved' = f
    /\ ranSend' = FALSE
    /\ UNCHANGED <<staged, flushed, buffer, sent, dropped, discOwed, tick>>

\* rocketnet_server_commit_send_frame: seal the reserved frame into staging
Commit ==
    /\ reserved # 0
    /\ buffer' = buffer \cup {reserved}
    /\ staged' = staged + 1
    /\ reserved' = 0
    /\ ranSend' = FALSE
    /\ UNCHANGED <<flushed, sent, dropped, discOwed, tick>>

\* a failed send: reserve finds the staging full and the frame is dropped
FailedSend(f) ==
    /\ f \in Fresh
    /\ Cardinality(buffer) = MaxStaged
    /\ dropped' = dropped \cup {f}
    /\ ranSend' = FALSE
    /\ UNCHANGED <<staged, flushed, buffer, sent, reserved, discOwed, tick>>

\* rocketnet_server_transport_send_packets, staged branch: flush exactly
\* when the counters differ, and everything staged leaves together
SendPackets ==
    /\ ranSend' = TRUE
    /\ IF staged # flushed
       THEN /\ sent' = sent \cup buffer
            /\ buffer' = {}
            /\ flushed' = staged
            /\ discOwed' = {}
       ELSE UNCHANGED <<sent, buffer, flushed, discOwed>>
    /\ UNCHANGED <<staged, dropped, reserved, tick>>

\* rocketnet_server_transport_update: the generation tick, and maintenance
\* may seal one frame through the seam while it runs
Update ==
    /\ tick' = 1 - tick
    /\ ranSend' = FALSE
    /\ \/ \E f \in Fresh :
            /\ Cardinality(buffer) < MaxStaged
            /\ buffer' = buffer \cup {f}
            /\ staged' = staged + 1
       \/ UNCHANGED <<buffer, staged>>
    /\ UNCHANGED <<flushed, sent, dropped, reserved, discOwed>>

\* rocketnet_server_transport_disconnect_client: seal the burst through the
\* seam, out of band.  The rule advances the staged counter by the burst, so
\* the next send_packets flushes it.  Without the rule the burst is sealed,
\* send_packets runs at once, the counters agree, and nothing leaves.
DisconnectClient ==
    \E burst \in SUBSET Fresh :
        /\ Cardinality(burst) <= MaxBurst
        /\ Cardinality(buffer) + Cardinality(burst) <= MaxStaged
        /\ buffer' = buffer \cup burst
        /\ discOwed' = discOwed \cup burst
        /\ IF DisconnectAdvancesStaged
           THEN /\ staged' = staged + Cardinality(burst)
                /\ ranSend' = FALSE
           ELSE /\ ranSend' = TRUE
                /\ UNCHANGED staged
        /\ UNCHANGED <<flushed, sent, dropped, reserved, tick>>

Next ==
    \/ \E f \in Frames : Reserve(f)
    \/ Commit
    \/ \E f \in Frames : FailedSend(f)
    \/ SendPackets
    \/ Update
    \/ DisconnectClient

Spec == Init /\ [][Next]_vars

\* StagedFramesEventuallySent is checked under weak fairness of send_packets
SpecFair == Spec /\ WF_vars(SendPackets)

-----------------------------------------------------------------------------

TypeOK ==
    /\ staged \in 0..MaxFrames
    /\ flushed \in 0..MaxFrames
    /\ buffer \subseteq Frames
    /\ sent \subseteq Frames
    /\ dropped \subseteq Frames
    /\ reserved \in 0..MaxFrames
    /\ discOwed \subseteq Frames
    /\ ranSend \in BOOLEAN
    /\ tick \in 0..1

\* every frame committed by disconnect_client leaves by the next send_packets
StagedDisconnectFlushed ==
    ranSend => discOwed = {}

\* the flushed counter never passes the staged counter
FlushedNeverAhead ==
    flushed <= staged

\* a disconnect frame owed is always still buffered or already sent
DiscOwedTracked ==
    discOwed \subseteq (buffer \cup sent)

\* every frame staged is flushed out under weak fairness of send_packets
StagedFramesEventuallySent ==
    \A f \in Frames : (f \in buffer) ~> (f \in sent)

=============================================================================
