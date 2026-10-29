------------------------ MODULE MCRNChannelQueues -------------------------
\* Small instance of RNChannelQueues. MCRNChannelQueues.cfg is the passing
\* control (BrokenStale = FALSE). MCRNChannelQueuesBrokenStale.cfg is the
\* reversed witness: a stale cancel or ack removes the entry and violates
\* NamedGenerationOnly.
EXTENDS RNChannelQueues
=============================================================================
