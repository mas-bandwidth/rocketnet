--------------------------- MODULE MCRNSlots ---------------------------
EXTENDS RNSlots
\* One transport ending for the larger instances (TwoSlots, Exhaustion), whose
\* state space the three endings triple: MCRNSlots.cfg checks all three.
MCOneTransportEnd == {"TIMED_OUT"}
=============================================================================
