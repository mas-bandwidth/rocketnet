--------------------------- MODULE MCChannels ---------------------------
EXTENDS Channels
SmallTraffic == <<[channel |-> 1, reliability |-> 3],
                  [channel |-> 1, reliability |-> 3],
                  [channel |-> 2, reliability |-> 0],
                  [channel |-> 2, reliability |-> 3]>>
\* Stronger finite-workload check: both monotone histories eventually hold
\* every reliable message, hence every admitted reliable message eventually
\* reaches each history. One temporal branch instead of six per-message ones.
WorkloadCompletes == <>(\A m \in Messages :
    IsReliable(m) => m \in acked /\ m \in Elems(delivered))
\* Pattern="arbitrary" never consults dataSent to choose loss. Its update is
\* independent bookkeeping. Preserve every endpoint variable, wire and pulse:
\* Send still changes wire, Receive still differs from Drop, and all fairness
\* labels and checked message properties factor through this projection.
ProtocolView == <<endpoint, dataWire, dataPulse>>
=============================================================================
