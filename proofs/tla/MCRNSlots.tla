--------------------------- MODULE MCRNSlots ---------------------------
EXTENDS RNSlots
\* One transport ending for the larger instances (TwoSlots, Exhaustion), whose
\* state space the three endings triple: MCRNSlots.cfg checks all three.
MCOneTransportEnd == {"TIMED_OUT"}
\* No update with a view held for the same two larger instances: the update
\* that takes the views back and continues (RejectUnreleasedUpdate) takes
\* their state space past the budget. MCRNSlots.cfg checks it.
MCNoMisuse == FALSE
=============================================================================
