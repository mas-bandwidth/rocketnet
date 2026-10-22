----------------------- MODULE MCRNClientIoLiveness -----------------------
\* Split only finite conjunctions of temporal obligations.
\* All checks use the complete RNClientIo state graph, both send rings,
\* every slot, and unchanged SpecFair. The committed-ring checks together
\* are the original obligation because Rings has exactly these two elements.
EXTENDS RNClientIo

ASSUME Rings = {"A", "B"}

CommittedRing(r) ==
    \A s \in Slots :
        owner[r][s] = "main" ~> (owner[r][s] = "io" \/ s \in dropped)

CommittedA == CommittedRing("A")
CommittedB == CommittedRing("B")

\* Partition 1..MaxDatagrams into 1..(MaxDatagrams-1) and its last element.
\* Both obligations together are exactly ArrivedEventuallyDelivered.
ASSUME MaxDatagrams > 0

ArrivedBeforeLast ==
    \A n \in 1..(MaxDatagrams - 1) :
        arrived >= n ~> (delivered >= n \/ phase # "Running")

ArrivedLast ==
    arrived >= MaxDatagrams ~> (delivered >= MaxDatagrams \/ phase # "Running")
=============================================================================
