------------------------- MODULE RNDriverSendStaging -------------------------
(***************************************************************************)
(* The staged send path of the server transport: the reserve/commit seam  *)
(* every driver serves (sdk/rocketnet_server_transport.cpp,               *)
(* rocketnet_server_reserve_send_frame and                               *)
(* rocketnet_server_commit_send_frame), the staged branch of             *)
(* rocketnet_server_transport_send_packets, the maintenance half of      *)
(* rocketnet_server_transport_update, and the out-of-band                *)
(* rocketnet_server_transport_disconnect_client.                         *)
(*                                                                         *)
(* On a staged driver a committed frame seals straight into the driver's  *)
(* own staging and is handed on only when the driver flushes.  The        *)
(* transport flushes when shutdown is set, when the transport declares    *)
(* ROCKETNET_TRANSPORT_REFLUSHES, or when the counters differ.            *)
(* SendPackets is the branch a transport without REFLUSHES takes: it      *)
(* flushes exactly when the counters differ.  XDP declares REFLUSHES, and *)
(* its retry with equal counters is the XDP submodel's Publish, which     *)
(* runs at every flush.  Commit projects both the sealing helper and its  *)
(* caller's bookkeeping: it advances the counter, disconnect bursts       *)
(* included, so the next send_packets after a disconnect flushes the      *)
(* burst.  DisconnectAdvancesStaged = FALSE removes that advance:         *)
(* disconnect_client seals its burst into the send buffer and calls       *)
(* send_packets, the counters still agree, nothing flushes, and the burst *)
(* waits for a later, unrelated staged commit.  That is the reversed      *)
(* witness, and TLC must reject it.                                       *)
(*                                                                         *)
(* "sent" is a frame handed to the driver's flush, not a frame on the     *)
(* wire.  On XDP that flush is rocketnet_server_socket_send_packets,      *)
(* which may put the publish off (the handoff lock's bounded try fails,   *)
(* or no replacement buffer is free); the producer stays staged and a     *)
(* later flush publishes it, which the XDP submodel checks                *)
(* (XdpStagedEventuallyPublished, under the assumption that the lock is   *)
(* not held at every try).  The two graphs are not composed: no state     *)
(* here says which frames a retained XDP producer holds.                  *)
(*                                                                         *)
(* A staging slot can go back without a flush: the link simulator's       *)
(* commit (rocketnet_server_commit_staged hands it the datagram and calls *)
(* io_abort_packet) and rocketnet_server_abort_send_frame (maintenance    *)
(* that reserved a frame and found nothing due, or a disconnect seal      *)
(* whose write failed).  Such a frame is "withdrawn" and must never also  *)
(* stay in the staging; AbortReturnsSlot = FALSE leaves it there, and TLC *)
(* must reject that (WithdrawnNeverStaged).  The simulator's choice is    *)
(* drawn at Commit; the other commit sites go through the same helper.    *)
(*                                                                         *)
(* Frames are abstract wire packets.  A frame is fresh until it is        *)
(* reserved, committed, sent or dropped, so the small instance replays    *)
(* the whole life of a bounded set of frames.  One reservation is         *)
(* outstanding at a time: every commit through the seam runs on the main  *)
(* thread.  update's unconditional counter bump is a bare generation      *)
(* tick here; it stages no frame and strands none.                        *)
(*                                                                         *)
(* Packet leases are a separate graph, attached only at transport idle.   *)
(* start_packet records one lease at the current epoch. update,           *)
(* send_packets and stop reset that epoch and leave the caller holding    *)
(* the old context. finish of the current epoch releases it. finish of    *)
(* an older epoch is caller misuse: the good spec records the refusal,    *)
(* and FinishRecordsRefusal = FALSE drops it silently. That silent drop   *)
(* is the reversed witness, and TLC must reject it. Lease actions run     *)
(* only from the initial transport and XDP state, and those actions run   *)
(* only while the lease is still initial, so the two graphs do not        *)
(* multiply.                                                               *)
(***************************************************************************)
EXTENDS Naturals, FiniteSets

CONSTANTS MaxFrames,                \* the abstract frames are 1..MaxFrames
          MaxStaged,                \* the staging capacity, in frames
          MaxBurst,                 \* the most frames one disconnect seals
          DisconnectAdvancesStaged, \* commit through disconnect advances staged
          XdpBuffers,               \* ROCKETNET_XDP_NUM_BUFFERS: physical XDP send buffers
          FinishRecordsRefusal,     \* finish after reset records the refusal
          AbortReturnsSlot          \* io_abort_packet takes a withdrawn frame out of the staging

ASSUME MaxFrames \in Nat \ {0} /\ MaxStaged \in 1..MaxFrames
ASSUME MaxBurst \in 1..MaxFrames /\ DisconnectAdvancesStaged \in BOOLEAN
ASSUME XdpBuffers \in Nat /\ XdpBuffers >= 2
ASSUME FinishRecordsRefusal \in BOOLEAN
ASSUME AbortReturnsSlot \in BOOLEAN

Frames == 1..MaxFrames
MaxLeaseGen == 1                \* one epoch bump; the model does not wrap onto a held generation

VARIABLES staged,     \* the staged counter: frames committed, ever
          flushed,    \* the flushed counter: frames flushed, ever
          buffer,     \* the send buffer: committed frames not yet flushed
          sent,       \* frames handed to the driver by a flush
          dropped,    \* frames whose reserve failed on a full staging
          withdrawn,  \* frames whose staging slot went back without a flush
          reserved,   \* the frame between reserve and commit; 0 for none
          discOwed,   \* disconnect frames committed and not yet sent
          ranSend,    \* the last action was send_packets
          xdpQueue, xdpFilling, xdpStaged, xdpDirty, xdpRefused,
          xdpRanFlush, xdpReadyBeforeFlush, xdpCountersDiffer,
          tick,       \* update's generation tick
          open,       \* a lease is current at gen
          held,       \* the caller still holds a context
          gen,        \* the lease epoch
          heldGen,    \* the epoch of the held context
          refused,    \* a finish after reset was recorded
          silent      \* a finish after reset was dropped

transportVars == <<staged, flushed, buffer, sent, dropped, withdrawn, reserved, discOwed,
          ranSend, tick>>

xdpVars == <<xdpQueue, xdpFilling, xdpStaged, xdpDirty, xdpRefused,
             xdpRanFlush, xdpReadyBeforeFlush, xdpCountersDiffer>>
leaseVars == <<open, held, gen, heldGen, refused, silent>>
vars == <<transportVars, xdpVars, leaseVars>>

Fresh == Frames \ (buffer \cup sent \cup dropped \cup withdrawn \cup {reserved})

\* the staging after a withdrawn frame's io_abort_packet: unchanged, or, in
\* the reversed witness, still holding the frame
AfterAbort(f) == IF AbortReturnsSlot THEN buffer ELSE buffer \cup {f}

TransportInit ==
    /\ staged = 0
    /\ flushed = 0
    /\ buffer = {}
    /\ sent = {}
    /\ dropped = {}
    /\ withdrawn = {}
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
    /\ UNCHANGED <<staged, flushed, buffer, sent, dropped, withdrawn, discOwed, tick>>

\* rocketnet_server_commit_send_frame seals the frame; Commit also includes
\* its caller advancing send_staged. The helper alone does not advance it.
\* rocketnet_server_commit_staged: the staging finishes the frame, or, when
\* the link simulator is active, the simulator takes the datagram and
\* io_abort_packet returns the staging slot (the caller still advances
\* send_staged, which costs one flush and strands nothing).
Commit ==
    /\ reserved # 0
    /\ \/ /\ buffer' = buffer \cup {reserved}
          /\ UNCHANGED withdrawn
       \/ /\ withdrawn' = withdrawn \cup {reserved}
          /\ buffer' = AfterAbort(reserved)
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
    /\ UNCHANGED <<staged, flushed, buffer, sent, withdrawn, reserved, discOwed, tick>>

\* rocketnet_server_transport_send_packets, staged branch, on a transport
\* without ROCKETNET_TRANSPORT_REFLUSHES and not at shutdown: flush exactly
\* when the counters differ, and everything staged is handed to the driver
\* together (on XDP the driver may then retain it: the XDP submodel)
SendPackets ==
    /\ ranSend' = TRUE
    /\ IF staged # flushed
       THEN /\ sent' = sent \cup buffer
            /\ buffer' = {}
            /\ flushed' = staged
            /\ discOwed' = {}
       ELSE UNCHANGED <<sent, buffer, flushed, discOwed>>
    /\ UNCHANGED <<staged, dropped, withdrawn, reserved, tick>>

\* rocketnet_server_transport_update: the generation tick, and maintenance
\* may seal one frame through the seam while it runs, or reserve one, find
\* nothing due and abort it (rocketnet_server_abort_send_frame, whose
\* io_abort_packet returns the staging slot)
Update ==
    /\ tick' = 1 - tick
    /\ ranSend' = FALSE
    /\ \/ \E f \in Fresh :
            /\ Cardinality(buffer) < MaxStaged
            /\ buffer' = buffer \cup {f}
            /\ staged' = staged + 1
            /\ UNCHANGED withdrawn
       \/ \E f \in Fresh :
            /\ Cardinality(buffer) < MaxStaged
            /\ withdrawn' = withdrawn \cup {f}
            /\ buffer' = AfterAbort(f)
            /\ UNCHANGED staged
       \/ UNCHANGED <<buffer, staged, withdrawn>>
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
        /\ UNCHANGED <<flushed, sent, dropped, withdrawn, reserved, tick>>

\* XDP worker/publication submodel: rocketnet_xdp_buffer_exchange_t in
\* sdk/rocketnet_server_socket_linux.cpp over XdpBuffers physical buffers.
\* One buffer is always the producer; xdpQueue counts the published buffers
\* not yet recycled (ready or claimed by the worker). claim moves a buffer
\* from ready to claimed and changes neither count, so it is folded into
\* WorkerRetire, which is recycle. Packet-slot exhaustion is separate;
\* XdpStart represents a send with capacity left.
XdpInit ==
    /\ xdpQueue = 0
    /\ xdpFilling = TRUE
    /\ xdpStaged = FALSE
    /\ xdpDirty = FALSE
    /\ xdpRefused = FALSE
    /\ xdpRanFlush = FALSE
    /\ xdpReadyBeforeFlush = FALSE
    /\ xdpCountersDiffer = FALSE

LeaseAtInit ==
    /\ open = FALSE
    /\ held = FALSE
    /\ gen = 0
    /\ heldGen = 0
    /\ refused = FALSE
    /\ silent = FALSE

LeaseInit == LeaseAtInit

\* The transport and XDP valuation Init gives them. Lease actions run only
\* here, so a lease step cannot fork the staged-send graph.
TransportIdle ==
    /\ staged = 0 /\ flushed = 0 /\ buffer = {} /\ sent = {} /\ dropped = {}
    /\ withdrawn = {} /\ reserved = 0 /\ discOwed = {} /\ ranSend = FALSE /\ tick = 0
    /\ xdpQueue = 0 /\ xdpFilling = TRUE /\ xdpStaged = FALSE
    /\ xdpDirty = FALSE /\ xdpRefused = FALSE /\ xdpRanFlush = FALSE
    /\ xdpReadyBeforeFlush = FALSE /\ xdpCountersDiffer = FALSE

\* rocketnet_server_transport_start_packet: one current lease, held by the caller
StartPacket ==
    /\ TransportIdle
    /\ ~open
    /\ ~held
    /\ open' = TRUE
    /\ held' = TRUE
    /\ heldGen' = gen
    /\ UNCHANGED <<gen, refused, silent>>

\* update, send_packets and stop: bump the epoch. The caller may still hold
\* the context. The entry is retired, not kept.
ResetLeases ==
    /\ TransportIdle
    /\ gen < MaxLeaseGen
    /\ gen' = gen + 1
    /\ open' = FALSE
    /\ UNCHANGED <<held, heldGen, refused, silent>>

\* rocketnet_server_transport_finish_packet. The current epoch releases the
\* lease. An older epoch is misuse: record it, or, in the reversed witness,
\* drop it with no record.
FinishPacket ==
    /\ TransportIdle
    /\ held
    /\ IF heldGen = gen
       THEN /\ open' = FALSE
            /\ held' = FALSE
            /\ UNCHANGED <<gen, heldGen, refused, silent>>
       ELSE /\ open' = FALSE
            /\ held' = FALSE
            /\ UNCHANGED <<gen, heldGen>>
            /\ IF FinishRecordsRefusal
               THEN /\ refused' = TRUE
                    /\ UNCHANGED silent
               ELSE /\ silent' = TRUE
                    /\ UNCHANGED refused

LeaseNext == StartPacket \/ ResetLeases \/ FinishPacket

Init == TransportInit /\ XdpInit /\ LeaseInit

XdpStart ==
    /\ IF xdpFilling
       THEN /\ xdpStaged' = TRUE
            /\ xdpDirty' = TRUE
            /\ xdpCountersDiffer' = TRUE
            /\ UNCHANGED xdpRefused
       ELSE /\ xdpRefused' = TRUE
            /\ UNCHANGED <<xdpStaged, xdpDirty, xdpCountersDiffer>>
    /\ xdpRanFlush' = FALSE
    /\ UNCHANGED <<xdpQueue, xdpFilling, xdpReadyBeforeFlush>>

\* rocketnet_xdp_buffer_exchange_t::publish retains the producer if no
\* replacement is free (free_count == XdpBuffers - 1 - xdpQueue == 0).
\* rocketnet_server_transport_send_packets retries even when counters agree.
\* The broken form publishes the producer itself when no replacement is free.
PublishWith(retainProducer) ==
    /\ xdpRanFlush' = TRUE
    /\ xdpReadyBeforeFlush' = (xdpStaged /\ xdpQueue < XdpBuffers - 1)
    /\ IF xdpStaged /\ (xdpDirty \/ retainProducer)
                    /\ xdpQueue < (IF retainProducer THEN XdpBuffers - 1 ELSE XdpBuffers)
       THEN /\ xdpQueue' = xdpQueue + 1
            /\ xdpStaged' = FALSE
            /\ xdpFilling' = (xdpQueue + 1 < XdpBuffers)
       ELSE UNCHANGED <<xdpQueue, xdpStaged, xdpFilling>>
    /\ xdpCountersDiffer' = FALSE
    /\ xdpDirty' = FALSE
    /\ UNCHANGED xdpRefused

\* rocketnet_server_socket_send_packets: the publish with the handoff lock
\* taken, or the bounded try of the lock failing (the xdp thread holds it
\* to claim or recycle a buffer): the publish is put off and counted, the
\* producer stays staged for the next call, and the flush has run.
Publish ==
    \/ PublishWith(TRUE)
    \/ /\ xdpRanFlush' = TRUE
       /\ xdpReadyBeforeFlush' = FALSE
       /\ xdpCountersDiffer' = FALSE
       /\ xdpDirty' = FALSE
       /\ UNCHANGED <<xdpQueue, xdpStaged, xdpFilling, xdpRefused>>
PublishBrokenRefuses == PublishWith(FALSE)

WorkerRetire ==
    /\ xdpQueue > 0
    /\ xdpQueue' = xdpQueue - 1
    /\ xdpFilling' = TRUE
    /\ xdpRanFlush' = FALSE
    /\ UNCHANGED <<xdpStaged, xdpDirty, xdpRefused, xdpReadyBeforeFlush,
                  xdpCountersDiffer>>

TransportNext ==
    \/ \E f \in Frames : Reserve(f)
    \/ Commit
    \/ \E f \in Frames : FailedSend(f)
    \/ SendPackets
    \/ Update
    \/ DisconnectClient

Next ==
    \/ /\ TransportNext /\ LeaseAtInit /\ UNCHANGED xdpVars /\ UNCHANGED leaseVars
    \/ /\ (XdpStart \/ Publish \/ WorkerRetire) /\ LeaseAtInit
          /\ UNCHANGED transportVars /\ UNCHANGED leaseVars
    \/ /\ LeaseNext /\ UNCHANGED transportVars /\ UNCHANGED xdpVars

NextBrokenRefuses ==
    \/ /\ TransportNext /\ LeaseAtInit /\ UNCHANGED xdpVars /\ UNCHANGED leaseVars
    \/ /\ (XdpStart \/ PublishBrokenRefuses \/ WorkerRetire) /\ LeaseAtInit
          /\ UNCHANGED transportVars /\ UNCHANGED leaseVars
    \/ /\ LeaseNext /\ UNCHANGED transportVars /\ UNCHANGED xdpVars

Spec == Init /\ [][Next]_vars
SpecBrokenRefuses == Init /\ [][NextBrokenRefuses]_vars

\* StagedFramesEventuallySent is checked under weak fairness of send_packets.
\* Fairness is the real Next step, including the lease still at init: a
\* recorded refusal parks the lease off init and must not be asked to flush.
\* The publish's fairness is on the publish with the lock taken: the
\* assumption that the xdp thread, which takes the lock only to claim or
\* recycle a buffer, does not hold it at every try of every flush.
\* SpecFairLockAlwaysBusy puts the fairness on the whole publish step, busy
\* tries included, and XdpStagedEventuallyPublished fails under it.
SpecFair == Spec /\ WF_vars(SendPackets /\ LeaseAtInit /\ UNCHANGED xdpVars /\ UNCHANGED leaseVars)
                 /\ WF_vars(PublishWith(TRUE) /\ LeaseAtInit /\ UNCHANGED transportVars /\ UNCHANGED leaseVars)
                 /\ WF_vars(WorkerRetire /\ LeaseAtInit /\ UNCHANGED transportVars /\ UNCHANGED leaseVars)
SpecFairLockAlwaysBusy ==
    Spec /\ WF_vars(SendPackets /\ LeaseAtInit /\ UNCHANGED xdpVars /\ UNCHANGED leaseVars)
         /\ WF_vars(Publish /\ LeaseAtInit /\ UNCHANGED transportVars /\ UNCHANGED leaseVars)
         /\ WF_vars(WorkerRetire /\ LeaseAtInit /\ UNCHANGED transportVars /\ UNCHANGED leaseVars)

-----------------------------------------------------------------------------

NoRefusedSend == ~xdpRefused
\* a flush that took the handoff lock with a replacement free publishes the
\* staged producer (a busy try sets xdpReadyBeforeFlush FALSE)
RetryFlushPublishes == (xdpRanFlush /\ xdpReadyBeforeFlush) => ~xdpStaged
XdpStagedEventuallyPublished == xdpStaged ~> ~xdpStaged

TypeOK ==
    /\ xdpQueue \in 0..XdpBuffers
    /\ xdpFilling \in BOOLEAN
    /\ xdpStaged \in BOOLEAN
    /\ xdpDirty \in BOOLEAN
    /\ xdpRefused \in BOOLEAN
    /\ xdpRanFlush \in BOOLEAN
    /\ xdpReadyBeforeFlush \in BOOLEAN
    /\ xdpCountersDiffer \in BOOLEAN
    /\ staged \in 0..MaxFrames
    /\ flushed \in 0..MaxFrames
    /\ buffer \subseteq Frames
    /\ sent \subseteq Frames
    /\ dropped \subseteq Frames
    /\ withdrawn \subseteq Frames
    /\ reserved \in 0..MaxFrames
    /\ discOwed \subseteq Frames
    /\ ranSend \in BOOLEAN
    /\ tick \in 0..1
    /\ open \in BOOLEAN
    /\ held \in BOOLEAN
    /\ gen \in 0..MaxLeaseGen
    /\ heldGen \in 0..MaxLeaseGen
    /\ refused \in BOOLEAN
    /\ silent \in BOOLEAN

\* every frame committed by disconnect_client is handed to the driver's flush
\* by the next send_packets (on XDP the driver may retain it: see above)
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

\* a frame whose staging slot went back is never also flushed: the simulator
\* already sent it, or nothing was written into it
WithdrawnNeverStaged == withdrawn \cap (buffer \cup sent) = {}

\* a finish after reset is recorded. silent is the reversed witness, the
\* finish that returns as if nothing was owed.
FinishAfterResetRecorded == silent = FALSE

=============================================================================
