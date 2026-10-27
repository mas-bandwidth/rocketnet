------------------------- MODULE RNDriverSendStaging -------------------------
(***************************************************************************)
(* The staged send path of the server transport: the reserve/commit seam  *)
(* every driver serves (sdk/rocketnet_server_transport.cpp,               *)
(* rocketnet_server_reserve_send_frame at line 260 and                    *)
(* rocketnet_server_commit_send_frame at line 288), the staged branch of  *)
(* rocketnet_server_transport_send_packets (line 1725, the counter test   *)
(* at line 1745), the maintenance half of                                 *)
(* rocketnet_server_transport_update (line 983), and the out-of-band      *)
(* rocketnet_server_transport_disconnect_client (line 1812).              *)
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
          FinishRecordsRefusal      \* finish after reset records the refusal

ASSUME MaxFrames \in Nat \ {0} /\ MaxStaged \in 1..MaxFrames
ASSUME MaxBurst \in 1..MaxFrames /\ DisconnectAdvancesStaged \in BOOLEAN
ASSUME XdpBuffers \in Nat /\ XdpBuffers >= 2
ASSUME FinishRecordsRefusal \in BOOLEAN

Frames == 1..MaxFrames
MaxLeaseGen == 1                \* one epoch bump; the model does not wrap onto a held generation

VARIABLES staged,     \* the staged counter: frames committed, ever
          flushed,    \* the flushed counter: frames flushed, ever
          buffer,     \* the send buffer: committed frames not yet sent
          sent,       \* frames that have left by a flush
          dropped,    \* frames whose reserve failed on a full staging
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

transportVars == <<staged, flushed, buffer, sent, dropped, reserved, discOwed,
          ranSend, tick>>

xdpVars == <<xdpQueue, xdpFilling, xdpStaged, xdpDirty, xdpRefused,
             xdpRanFlush, xdpReadyBeforeFlush, xdpCountersDiffer>>
leaseVars == <<open, held, gen, heldGen, refused, silent>>
vars == <<transportVars, xdpVars, leaseVars>>

Fresh == Frames \ (buffer \cup sent \cup dropped \cup {reserved})

TransportInit ==
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
    /\ reserved = 0 /\ discOwed = {} /\ ranSend = FALSE /\ tick = 0
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

Publish == PublishWith(TRUE)
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
SpecFair == Spec /\ WF_vars(SendPackets /\ LeaseAtInit /\ UNCHANGED xdpVars /\ UNCHANGED leaseVars)
                 /\ WF_vars(Publish /\ LeaseAtInit /\ UNCHANGED transportVars /\ UNCHANGED leaseVars)
                 /\ WF_vars(WorkerRetire /\ LeaseAtInit /\ UNCHANGED transportVars /\ UNCHANGED leaseVars)

-----------------------------------------------------------------------------

NoRefusedSend == ~xdpRefused
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

\* a finish after reset is recorded. silent is the reversed witness, the
\* finish that returns as if nothing was owed.
FinishAfterResetRecorded == silent = FALSE

=============================================================================
