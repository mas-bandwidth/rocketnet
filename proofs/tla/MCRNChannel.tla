---------------------------- MODULE MCRNChannel ----------------------------
EXTENDS RNChannel
\* Negative reachability probes, used only by documented private diagnostics.
\* Their expected failures establish the relevant healthy paths exist.
RoomNeverReturns == ~roomReturned
ChunksNeverReorder == ~(3 \in received /\ 2 \notin received)
SmallMessagesNeverPacked == ~(\E i \in DOMAIN smap : smap[i].nums = {0,1})
NotReceivedNeverEnds == ~misused
=============================================================================
